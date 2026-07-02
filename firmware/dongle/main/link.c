/*
 * espkvm dongle — ESP-NOW receive path + pairing state machine.
 *
 * Steady state is RX-only: the hub unicasts LMK-encrypted input packets at
 * us and the Wi-Fi MAC layer acknowledges them (that ACK is what the hub
 * uses for its per-slot online/offline display — we never have to transmit
 * application data).
 *
 * Defensive posture on every received frame (in order):
 *   1. length/magic/version/type validated by kvm_pkt_validate()
 *   2. source MAC must be the paired hub (except during pairing)
 *   3. epoch/seq anti-replay window (persisted epoch floor across reboots)
 * Anything failing any step is silently dropped.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"

#include "kvm_proto.h"
#include "kvm_crypto.h"
#include "store.h"
#include "usb_dev.h"
#include "link.h"

static const char *TAG = "link";

#define RX_QUEUE_LEN        24
#define RX_MAX_LEN          80    /* largest espkvm packet is 62 bytes    */
#define PAIRING_TIMEOUT_MS  15000
#define WATCHDOG_MS         1500  /* release-all if link dies mid-keypress */

typedef struct {
    uint8_t src[6];
    uint8_t len;
    uint8_t data[RX_MAX_LEN];
} rx_item_t;

static QueueHandle_t s_rxq;
static volatile dlink_state_t s_state = DLINK_UNPAIRED;
static dongle_pairing_t s_pair;             /* valid when s_state == RUN  */
static bool s_have_pair;
static kvm_replay_t s_replay;
static volatile int64_t s_last_rx_us;
static volatile bool s_active;   /* KVM_FLAG_ACTIVE on the last packet */

/* Pairing-session scratch (only meaningful while DLINK_PAIRING) */
static struct {
    kvm_ecdh_t *ecdh;
    uint8_t     my_pub[KVM_PUBKEY_LEN];
    uint8_t     hub_mac[6];
    uint32_t    session;
    bool        req_sent;
    int64_t     started_us;
} s_ps;

static volatile bool s_pairing_requested;

/* Hub public key from the most recent beacon (needed at CONFIRM time). */
static uint8_t s_hub_pub[KVM_PUBKEY_LEN];

/* ------------------------------------------------------------------ */
/* ESP-NOW plumbing                                                   */
/* ------------------------------------------------------------------ */

static void recv_cb(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    /* Runs in the Wi-Fi task — copy out and return immediately. */
    if (len <= 0 || len > RX_MAX_LEN) {
        return;
    }
    rx_item_t it;
    memcpy(it.src, info->src_addr, 6);
    it.len = (uint8_t)len;
    memcpy(it.data, data, len);
    xQueueSend(s_rxq, &it, 0);   /* drop on overflow — never block Wi-Fi */
}

static esp_err_t wifi_up(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPKVM_CHANNEL,
                                         WIFI_SECOND_CHAN_NONE));
    /* Power save off: we must be awake for every unicast the hub sends. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    return ESP_OK;
}

/* (Re)register `mac` as our only unicast peer, optionally encrypted. */
static esp_err_t set_peer(const uint8_t mac[6], const uint8_t *lmk)
{
    esp_now_del_peer(mac);   /* ignore result — may not exist */

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = CONFIG_ESPKVM_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    if (lmk) {
        peer.encrypt = true;
        memcpy(peer.lmk, lmk, KVM_KEY_LEN);
    }
    return esp_now_add_peer(&peer);
}

/* Apply a stored pairing: PMK + encrypted hub peer + fresh replay window
 * floored at the last persisted epoch. */
static void apply_pairing(const dongle_pairing_t *p)
{
    ESP_ERROR_CHECK(esp_now_set_pmk(p->pmk));
    ESP_ERROR_CHECK(set_peer(p->hub_mac, p->lmk));
    kvm_replay_reset(&s_replay);
    uint32_t floor_epoch = store_get_epoch();
    if (floor_epoch > 0) {
        /* Pretend we already saw (floor_epoch, 0) so anything from an
         * older hub boot is rejected even right after our own reboot. */
        kvm_replay_accept(&s_replay, floor_epoch, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Pairing handshake (dongle side)                                    */
/* ------------------------------------------------------------------ */

static void pairing_abort(const char *why)
{
    ESP_LOGW(TAG, "pairing aborted: %s", why);
    kvm_ecdh_free(s_ps.ecdh);
    memset(&s_ps, 0, sizeof(s_ps));

    if (s_have_pair) {
        apply_pairing(&s_pair);   /* fall back to the previous hub */
        s_state = DLINK_RUN;
    } else {
        s_state = DLINK_UNPAIRED;
    }
}

static void pairing_begin(void)
{
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps.ecdh = kvm_ecdh_new(s_ps.my_pub);
    if (!s_ps.ecdh) {
        pairing_abort("keygen failed");
        return;
    }
    s_ps.started_us = esp_timer_get_time();
    s_state = DLINK_PAIRING;
    ESP_LOGI(TAG, "pairing: listening for hub beacon on channel %d",
             CONFIG_ESPKVM_CHANNEL);
}

static void pairing_on_beacon(const rx_item_t *it)
{
    if (s_ps.req_sent) {
        return;   /* already answering one hub */
    }
    kvm_pkt_pair_beacon_t b;
    memcpy(&b, it->data, sizeof(b));

    memcpy(s_ps.hub_mac, it->src, 6);
    s_ps.session = b.session;

    /* The hub is not a registered peer yet — add it plaintext so we can
     * answer. It gets upgraded to encrypted after PAIR_CONFIRM. */
    if (set_peer(s_ps.hub_mac, NULL) != ESP_OK) {
        pairing_abort("add peer failed");
        return;
    }

    kvm_pkt_pair_req_t req;
    kvm_hdr_fill(&req.h, KVM_PKT_PAIR_REQ, 0, 0);
    req.session = b.session;
    memcpy(req.dev_pub, s_ps.my_pub, KVM_PUBKEY_LEN);
    req.fw_ver = 1;

    /* Store the hub's public key inside the beacon by deriving now — we
     * need it again at CONFIRM time, so stash the shared secret early. */
    if (esp_now_send(s_ps.hub_mac, (uint8_t *)&req, sizeof(req)) != ESP_OK) {
        pairing_abort("send req failed");
        return;
    }
    s_ps.req_sent = true;
    /* The hub's public key was stashed in s_hub_pub by the task loop;
     * we'll need it when PAIR_CONFIRM arrives. */
}

static void pairing_on_confirm(const rx_item_t *it)
{
    if (!s_ps.req_sent || memcmp(it->src, s_ps.hub_mac, 6) != 0) {
        return;
    }
    kvm_pkt_pair_confirm_t c;
    memcpy(&c, it->data, sizeof(c));
    if (c.session != s_ps.session) {
        return;
    }

    uint8_t shared[32], kek[KVM_KEY_LEN], lmk[KVM_KEY_LEN];
    uint8_t blob[KVM_PAIR_BLOB_LEN];
    if (kvm_ecdh_secret(s_ps.ecdh, s_hub_pub, shared) != ESP_OK) {
        pairing_abort("ecdh failed");
        return;
    }
    kvm_kdf(shared, "espkvm kek v1", kek, sizeof(kek));
    kvm_kdf(shared, "espkvm lmk v1", lmk, sizeof(lmk));

    if (kvm_gcm_open(kek, c.nonce, c.ct, sizeof(blob), c.tag, blob) != ESP_OK) {
        /* Wrong tag => not the hub we did ECDH with. Ignore; the real
         * CONFIRM may still arrive. */
        ESP_LOGW(TAG, "pairing: CONFIRM tag mismatch, ignoring");
        return;
    }

    dongle_pairing_t p;
    memcpy(p.hub_mac, s_ps.hub_mac, 6);
    memcpy(p.pmk, blob, KVM_KEY_LEN);
    memcpy(p.lmk, lmk, KVM_KEY_LEN);
    p.slot = blob[KVM_KEY_LEN];
    /* blob[KVM_KEY_LEN + 1] carries the hub's channel — informational for
     * now since both sides are built with the same fixed channel. */

    if (store_save_pairing(&p) != ESP_OK) {
        pairing_abort("nvs save failed");
        return;
    }

    s_pair = p;
    s_have_pair = true;
    apply_pairing(&p);

    /* Give the hub a moment to flip our peer entry to encrypted before we
     * send the (encrypted) DONE — see hub link.c for the matching note. */
    vTaskDelay(pdMS_TO_TICKS(150));

    kvm_pkt_pair_done_t done;
    kvm_hdr_fill(&done.h, KVM_PKT_PAIR_DONE, 0, 0);
    done.session = s_ps.session;
    done.slot = p.slot;
    kvm_kdf(shared, "espkvm done v1", done.proof, sizeof(done.proof));

    for (int i = 0; i < 3; i++) {   /* small burst — DONE has no retry loop */
        esp_now_send(p.hub_mac, (uint8_t *)&done, sizeof(done));
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    memset(shared, 0, sizeof(shared));
    memset(kek, 0, sizeof(kek));
    kvm_ecdh_free(s_ps.ecdh);
    memset(&s_ps, 0, sizeof(s_ps));

    s_state = DLINK_RUN;
    s_last_rx_us = esp_timer_get_time();
    ESP_LOGI(TAG, "paired to hub as slot %u", p.slot);
}

/* ------------------------------------------------------------------ */
/* RUN-state dispatch                                                 */
/* ------------------------------------------------------------------ */

static void handle_data(int type, const rx_item_t *it)
{
    kvm_hdr_t h;
    memcpy(&h, it->data, sizeof(h));

    if (!kvm_replay_accept(&s_replay, h.epoch, h.seq)) {
        return;   /* duplicate or replayed frame */
    }
    if (h.epoch > store_get_epoch()) {
        store_set_epoch(h.epoch);   /* raise the reboot-persistent floor */
    }
    s_last_rx_us = esp_timer_get_time();
    s_active = (h.flags & KVM_FLAG_ACTIVE) != 0;

    switch (type) {
    case KVM_PKT_KEYBOARD: {
        kvm_pkt_kbd_t p;
        memcpy(&p, it->data, sizeof(p));
        usb_dev_kbd(p.mods, p.keys);
        break;
    }
    case KVM_PKT_MOUSE: {
        kvm_pkt_mouse_t p;
        memcpy(&p, it->data, sizeof(p));
        usb_dev_mouse(p.buttons, p.dx, p.dy, p.wheel, p.pan);
        break;
    }
    case KVM_PKT_CONSUMER: {
        kvm_pkt_consumer_t p;
        memcpy(&p, it->data, sizeof(p));
        usb_dev_consumer(p.usage);
        break;
    }
    case KVM_PKT_CONTROL: {
        kvm_pkt_control_t p;
        memcpy(&p, it->data, sizeof(p));
        if (p.op == KVM_CTL_RELEASE_ALL) {
            usb_dev_release_all();
        } else if (p.op == KVM_CTL_UNPAIR) {
            ESP_LOGW(TAG, "hub requested unpair — wiping and rebooting");
            store_clear();
            esp_restart();
        }
        /* KVM_CTL_PING needs no action: the MAC-layer ACK the radio
         * already sent is the whole point. */
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Main link task                                                     */
/* ------------------------------------------------------------------ */

static void link_task(void *arg)
{
    (void)arg;
    rx_item_t it;

    for (;;) {
        bool got = xQueueReceive(s_rxq, &it, pdMS_TO_TICKS(100)) == pdTRUE;
        int64_t now = esp_timer_get_time();

        if (s_pairing_requested) {
            s_pairing_requested = false;
            pairing_begin();
        }

        if (got) {
            int type = kvm_pkt_validate(it.data, it.len);
            if (type < 0) {
                continue;
            }

            if (s_state == DLINK_PAIRING) {
                if (type == KVM_PKT_PAIR_BEACON) {
                    kvm_pkt_pair_beacon_t b;
                    memcpy(&b, it.data, sizeof(b));
                    memcpy(s_hub_pub, b.hub_pub, KVM_PUBKEY_LEN);
                    pairing_on_beacon(&it);
                } else if (type == KVM_PKT_PAIR_CONFIRM) {
                    pairing_on_confirm(&it);
                }
            } else if (s_state == DLINK_RUN) {
                /* Only the paired hub may talk to us. */
                if (memcmp(it.src, s_pair.hub_mac, 6) == 0 &&
                    type >= KVM_PKT_KEYBOARD && type <= KVM_PKT_CONTROL) {
                    handle_data(type, &it);
                }
            }
        }

        /* Pairing window timeout */
        if (s_state == DLINK_PAIRING &&
            now - s_ps.started_us > (int64_t)PAIRING_TIMEOUT_MS * 1000) {
            pairing_abort("timeout");
        }

        /* Stuck-key watchdog: if the hub goes silent (out of range, power
         * cut, switched away and crashed...) while something is held down,
         * release everything. The hub pings the active slot every 500 ms,
         * so 1.5 s of silence means the link is really gone. */
        if (s_state == DLINK_RUN && usb_dev_anything_held() &&
            now - s_last_rx_us > (int64_t)WATCHDOG_MS * 1000) {
            ESP_LOGW(TAG, "link lost with keys held — releasing all");
            usb_dev_release_all();
        }
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

esp_err_t link_init(void)
{
    s_rxq = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
    if (!s_rxq) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(wifi_up());
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    if (store_get_pairing(&s_pair)) {
        s_have_pair = true;
        apply_pairing(&s_pair);
        s_state = DLINK_RUN;
        ESP_LOGI(TAG, "paired (slot %u), running", s_pair.slot);
    } else {
        s_state = DLINK_UNPAIRED;
        ESP_LOGI(TAG, "unpaired — press BOOT while the hub is in pairing mode");
    }

    xTaskCreate(link_task, "kvm_link", 6144, NULL, 10, NULL);
    return ESP_OK;
}

dlink_state_t link_state(void)
{
    return s_state;
}

void link_start_pairing(void)
{
    s_pairing_requested = true;
}

void link_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    store_clear();
    esp_restart();
}

uint32_t link_ms_since_rx(void)
{
    return (uint32_t)((esp_timer_get_time() - s_last_rx_us) / 1000);
}

uint8_t link_slot(void)
{
    return s_have_pair ? s_pair.slot : KVM_SLOT_NONE;
}

bool link_is_active(void)
{
    return s_active;
}

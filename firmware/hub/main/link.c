/*
 * espkvm hub — ESP-NOW transmit path, pairing (hub side), link health.
 *
 * ESP-NOW key handling in one paragraph: esp_now_set_pmk() installs one
 * global Primary Master Key; each peer registered with encrypt=true gets a
 * Local Master Key (LMK). Frames to encrypted peers are CCMP-protected
 * with a key derived from the LMK (the PMK protects the LMKs themselves).
 * Both ends must therefore know the same PMK *and* the same per-link LMK.
 * espkvm derives the LMK from an ephemeral X25519 exchange at pairing time
 * and ships the hub's PMK to the dongle inside an AES-GCM blob — so no key
 * ever crosses the air in the clear, and each dongle has a different LMK
 * (compromising one dongle never exposes traffic to the others).
 *
 * Link health: ESP-NOW unicast frames are MAC-ACKed by the receiver, and
 * the send callback reports that ACK as ESP_NOW_SEND_SUCCESS. We ping the
 * active slot every 500 ms (which doubles as the dongle's stuck-key
 * watchdog keepalive) and sweep the other paired slots every 2 s, deriving
 * online/offline from consecutive ACK failures. Nothing is ever queued for
 * an offline dongle — input packets are fire-and-forget by design.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "kvm_crypto.h"
#include "store.h"
#include "link.h"

static const char *TAG = "link";

#define RX_MAX_LEN          80
#define PAIR_WINDOW_MS      30000
#define PAIR_DONE_TIMEOUT_MS 5000
#define BEACON_PERIOD_MS    300
#define TICK_PERIOD_MS      500
#define OFFLINE_AFTER_FAILS 3

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef enum {
    MSG_RX, MSG_TICK, MSG_BEACON,
    MSG_PAIR_START, MSG_PAIR_CANCEL, MSG_UNPAIR,
} msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t slot;                 /* MSG_UNPAIR */
    uint8_t src[6];               /* MSG_RX */
    uint8_t len;
    uint8_t data[RX_MAX_LEN];
} link_msg_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock;      /* guards active slot + seq + sends */
static link_event_cb_t s_cb;

static uint32_t s_seq;                /* monotonic within this boot epoch */
static uint8_t s_active = KVM_SLOT_NONE;

static struct {
    uint8_t fails;
    bool online;
} s_health[KVM_MAX_SLOTS];

/* Pairing session (hub side) */
static struct {
    enum { PS_IDLE, PS_BEACON, PS_WAIT_DONE } st;
    kvm_ecdh_t *ecdh;
    uint8_t     pub[KVM_PUBKEY_LEN];
    uint32_t    session;
    int64_t     window_end_us;
    /* pending dongle awaiting PAIR_DONE */
    uint8_t     mac[6];
    uint8_t     lmk[KVM_KEY_LEN];
    uint8_t     proof[KVM_TAG_LEN];
    uint8_t     slot;
    bool        slot_was_paired;      /* re-pair of an existing slot      */
    int64_t     done_deadline_us;
} s_ps;

/* ------------------------------------------------------------------ */
/* Peer helpers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t set_peer(const uint8_t mac[6], const uint8_t *lmk)
{
    esp_now_del_peer(mac);   /* ignore result — may not exist */

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = CONFIG_ESPKVM_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    if (lmk) {
        p.encrypt = true;
        memcpy(p.lmk, lmk, KVM_KEY_LEN);
    }
    return esp_now_add_peer(&p);
}

/* ------------------------------------------------------------------ */
/* ESP-NOW callbacks (Wi-Fi task context — keep them tiny)            */
/* ------------------------------------------------------------------ */

static void recv_cb(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    if (len <= 0 || len > RX_MAX_LEN) {
        return;
    }
    link_msg_t m = { .type = MSG_RX, .len = (uint8_t)len };
    memcpy(m.src, info->src_addr, 6);
    memcpy(m.data, data, len);
    xQueueSend(s_q, &m, 0);
}

static void send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    uint8_t slot = store_slot_for_mac(mac);
    if (slot == KVM_SLOT_NONE) {
        return;   /* broadcast or mid-pairing peer */
    }

    bool changed = false;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_health[slot].fails = 0;
        if (!s_health[slot].online) {
            s_health[slot].online = true;
            changed = true;
        }
    } else {
        if (s_health[slot].fails < 0xFF) {
            s_health[slot].fails++;
        }
        if (s_health[slot].online &&
            s_health[slot].fails >= OFFLINE_AFTER_FAILS) {
            s_health[slot].online = false;
            changed = true;
        }
    }
    if (changed && s_cb) {
        s_cb(LINK_EVT_STATUS, slot);
    }
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

/* Send an input/control packet to a specific slot. Caller holds s_lock. */
static void send_to_slot_locked(uint8_t slot, void *pkt, size_t len)
{
    const hub_pairing_t *p = store_pairing(slot);
    if (!p) {
        return;
    }
    kvm_hdr_t *h = (kvm_hdr_t *)pkt;
    h->seq = ++s_seq;
    /* Tell the dongle whether it is the currently-selected target — it
     * shows this on its LCD/LED so you can tell slots apart at a glance. */
    h->flags = (slot == s_active) ? KVM_FLAG_ACTIVE : 0;
    esp_now_send(p->mac, (uint8_t *)pkt, len);
}

static void send_to_active(void *pkt, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active != KVM_SLOT_NONE) {
        send_to_slot_locked(s_active, pkt, len);
    }
    xSemaphoreGive(s_lock);
}

void link_send_kbd(uint8_t mods, const uint8_t keys[6])
{
    kvm_pkt_kbd_t p;
    kvm_hdr_fill(&p.h, KVM_PKT_KEYBOARD, store_epoch(), 0);
    p.mods = mods;
    p.reserved = 0;
    memcpy(p.keys, keys, 6);
    send_to_active(&p, sizeof(p));
}

void link_send_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                     int8_t wheel, int8_t pan)
{
    kvm_pkt_mouse_t p;
    kvm_hdr_fill(&p.h, KVM_PKT_MOUSE, store_epoch(), 0);
    p.buttons = buttons;
    p.dx = dx;
    p.dy = dy;
    p.wheel = wheel;
    p.pan = pan;
    send_to_active(&p, sizeof(p));
}

void link_send_consumer(uint16_t usage)
{
    kvm_pkt_consumer_t p;
    kvm_hdr_fill(&p.h, KVM_PKT_CONSUMER, store_epoch(), 0);
    p.usage = usage;
    send_to_active(&p, sizeof(p));
}

static void send_control_locked(uint8_t slot, uint8_t op)
{
    kvm_pkt_control_t p;
    kvm_hdr_fill(&p.h, KVM_PKT_CONTROL, store_epoch(), 0);
    p.op = op;
    send_to_slot_locked(slot, &p, sizeof(p));
}

/* ------------------------------------------------------------------ */
/* Switching                                                          */
/* ------------------------------------------------------------------ */

void link_switch(uint8_t slot)
{
    if (slot >= KVM_MAX_SLOTS || !store_pairing(slot)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t prev = s_active;
    s_active = slot;   /* before the sends, so their ACTIVE flags are right */
    if (prev != slot && prev != KVM_SLOT_NONE) {
        /* Release-all to the machine we're leaving, twice for luck (the
         * frames are fire-and-forget; a stuck Ctrl on an unattended box is
         * the worst failure mode this device could have). The dongle-side
         * watchdog is the third safety net. */
        send_control_locked(prev, KVM_CTL_RELEASE_ALL);
        send_control_locked(prev, KVM_CTL_RELEASE_ALL);
    }
    store_set_last_slot(slot);
    send_control_locked(slot, KVM_CTL_PING);   /* instant health probe */
    xSemaphoreGive(s_lock);

    if (s_cb) {
        s_cb(LINK_EVT_SWITCHED, slot);
    }
}

uint8_t link_active_slot(void)
{
    return s_active;
}

bool link_slot_online(uint8_t slot)
{
    return slot < KVM_MAX_SLOTS && s_health[slot].online;
}

/* ------------------------------------------------------------------ */
/* Pairing (hub side)                                                 */
/* ------------------------------------------------------------------ */

static void pairing_cleanup(bool notify_timeout)
{
    if (s_ps.st == PS_WAIT_DONE) {
        /* Roll back the tentative peer. If this MAC was already paired
         * (re-pair attempt), restore its committed LMK so the old link
         * keeps working. */
        uint8_t old = store_slot_for_mac(s_ps.mac);
        if (old != KVM_SLOT_NONE) {
            set_peer(s_ps.mac, store_pairing(old)->lmk);
        } else {
            esp_now_del_peer(s_ps.mac);
        }
    }
    kvm_ecdh_free(s_ps.ecdh);
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps.st = PS_IDLE;
    if (notify_timeout && s_cb) {
        s_cb(LINK_EVT_PAIR_TIMEOUT, KVM_SLOT_NONE);
    }
}

static void pairing_start(void)
{
    if (s_ps.st != PS_IDLE) {
        return;
    }
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps.ecdh = kvm_ecdh_new(s_ps.pub);
    if (!s_ps.ecdh) {
        ESP_LOGE(TAG, "pairing: keygen failed");
        return;
    }
    kvm_rand((uint8_t *)&s_ps.session, sizeof(s_ps.session));
    s_ps.window_end_us = esp_timer_get_time() + (int64_t)PAIR_WINDOW_MS * 1000;
    s_ps.st = PS_BEACON;
    ESP_LOGI(TAG, "pairing mode: beaconing for %d s", PAIR_WINDOW_MS / 1000);
}

static void pairing_beacon(void)
{
    kvm_pkt_pair_beacon_t b;
    kvm_hdr_fill(&b.h, KVM_PKT_PAIR_BEACON, 0, 0);
    b.session = s_ps.session;
    memcpy(b.hub_pub, s_ps.pub, KVM_PUBKEY_LEN);
    esp_now_send(BCAST, (uint8_t *)&b, sizeof(b));
}

static void pairing_on_req(const link_msg_t *m)
{
    if (s_ps.st != PS_BEACON) {
        return;   /* one dongle at a time */
    }
    kvm_pkt_pair_req_t req;
    memcpy(&req, m->data, sizeof(req));
    if (req.session != s_ps.session) {
        return;
    }

    /* Slot assignment: a re-pairing dongle keeps its slot; a new one gets
     * the lowest free slot. */
    uint8_t slot = store_slot_for_mac(m->src);
    bool was_paired = slot != KVM_SLOT_NONE;
    if (!was_paired) {
        slot = store_free_slot();
        if (slot == KVM_SLOT_NONE) {
            ESP_LOGW(TAG, "pairing: all %d slots in use", KVM_MAX_SLOTS);
            if (s_cb) {
                s_cb(LINK_EVT_PAIR_FAIL, KVM_SLOT_NONE);
            }
            return;
        }
    }

    uint8_t shared[32], kek[KVM_KEY_LEN];
    if (kvm_ecdh_secret(s_ps.ecdh, req.dev_pub, shared) != ESP_OK) {
        return;
    }
    kvm_kdf(shared, "espkvm kek v1", kek, sizeof(kek));
    kvm_kdf(shared, "espkvm lmk v1", s_ps.lmk, sizeof(s_ps.lmk));
    kvm_kdf(shared, "espkvm done v1", s_ps.proof, sizeof(s_ps.proof));
    memset(shared, 0, sizeof(shared));

    kvm_pkt_pair_confirm_t c;
    kvm_hdr_fill(&c.h, KVM_PKT_PAIR_CONFIRM, 0, 0);
    c.session = s_ps.session;
    kvm_rand(c.nonce, sizeof(c.nonce));
    uint8_t blob[KVM_PAIR_BLOB_LEN];
    memcpy(blob, store_pmk(), KVM_KEY_LEN);
    blob[KVM_KEY_LEN] = slot;
    blob[KVM_KEY_LEN + 1] = CONFIG_ESPKVM_CHANNEL;
    if (kvm_gcm_seal(kek, c.nonce, blob, sizeof(blob), c.ct, c.tag) != ESP_OK) {
        memset(kek, 0, sizeof(kek));
        return;
    }
    memset(kek, 0, sizeof(kek));
    memset(blob, 0, sizeof(blob));

    /* CONFIRM must go out plaintext (the dongle doesn't have the LMK yet),
     * then we immediately flip the peer to encrypted so we can receive the
     * CCMP-protected PAIR_DONE. The dongle waits ~150 ms before sending it
     * to let this switch land. */
    if (set_peer(m->src, NULL) != ESP_OK) {
        return;
    }
    esp_now_send(m->src, (uint8_t *)&c, sizeof(c));
    esp_now_send(m->src, (uint8_t *)&c, sizeof(c));   /* belt & braces */
    set_peer(m->src, s_ps.lmk);

    memcpy(s_ps.mac, m->src, 6);
    s_ps.slot = slot;
    s_ps.slot_was_paired = was_paired;
    s_ps.done_deadline_us = esp_timer_get_time()
                          + (int64_t)PAIR_DONE_TIMEOUT_MS * 1000;
    s_ps.st = PS_WAIT_DONE;
}

static void pairing_on_done(const link_msg_t *m)
{
    if (s_ps.st != PS_WAIT_DONE || memcmp(m->src, s_ps.mac, 6) != 0) {
        return;
    }
    kvm_pkt_pair_done_t d;
    memcpy(&d, m->data, sizeof(d));
    if (d.session != s_ps.session || d.slot != s_ps.slot ||
        memcmp(d.proof, s_ps.proof, KVM_TAG_LEN) != 0) {
        ESP_LOGW(TAG, "pairing: bad DONE, ignoring");
        return;
    }

    char name[KVM_NAME_LEN + 1];
    const hub_pairing_t *old = store_pairing(s_ps.slot);
    if (s_ps.slot_was_paired && old && old->name[0]) {
        strncpy(name, old->name, sizeof(name) - 1);   /* keep the name */
        name[sizeof(name) - 1] = '\0';
    } else {
        snprintf(name, sizeof(name), "PC %u", s_ps.slot);
    }

    if (store_save_pairing(s_ps.slot, s_ps.mac, s_ps.lmk, name) != ESP_OK) {
        ESP_LOGE(TAG, "pairing: NVS save failed");
        pairing_cleanup(true);
        return;
    }

    uint8_t slot = s_ps.slot;
    ESP_LOGI(TAG, "paired dongle %02x:%02x:%02x:%02x:%02x:%02x as slot %u",
             s_ps.mac[0], s_ps.mac[1], s_ps.mac[2],
             s_ps.mac[3], s_ps.mac[4], s_ps.mac[5], slot);

    kvm_ecdh_free(s_ps.ecdh);
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps.st = PS_IDLE;

    if (s_cb) {
        s_cb(LINK_EVT_PAIRED, slot);
    }

    /* First pairing on a fresh hub: make it the active slot right away. */
    if (link_active_slot() == KVM_SLOT_NONE) {
        link_switch(slot);
    }
}

/* ------------------------------------------------------------------ */
/* Health                                                             */
/* ------------------------------------------------------------------ */

static void health_tick(void)
{
    static uint8_t sweep = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active != KVM_SLOT_NONE) {
        /* Keepalive for the active slot: also feeds the dongle's
         * stuck-key watchdog. */
        send_control_locked(s_active, KVM_CTL_PING);
    }
    if (++sweep >= 4) {   /* every 2 s, probe everyone else */
        sweep = 0;
        for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
            if (i != s_active && store_pairing(i)) {
                send_control_locked(i, KVM_CTL_PING);
            }
        }
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Link task + timers                                                 */
/* ------------------------------------------------------------------ */

static void tick_timer_cb(void *arg)
{
    (void)arg;
    link_msg_t m = { .type = MSG_TICK };
    xQueueSend(s_q, &m, 0);
}

static void beacon_timer_cb(void *arg)
{
    (void)arg;
    link_msg_t m = { .type = MSG_BEACON };
    xQueueSend(s_q, &m, 0);
}

static void link_task(void *arg)
{
    (void)arg;
    link_msg_t m;

    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (m.type) {
        case MSG_RX: {
            int type = kvm_pkt_validate(m.data, m.len);
            if (type < 0) {
                break;
            }
            /* The hub only ever expects pairing traffic; anything else
             * from the air is ignored (dongles are RX-only in RUN). */
            if (type == KVM_PKT_PAIR_REQ) {
                pairing_on_req(&m);
            } else if (type == KVM_PKT_PAIR_DONE) {
                pairing_on_done(&m);
            }
            break;
        }
        case MSG_TICK:
            health_tick();
            if (s_ps.st != PS_IDLE &&
                esp_timer_get_time() > s_ps.window_end_us) {
                pairing_cleanup(true);
            }
            if (s_ps.st == PS_WAIT_DONE &&
                esp_timer_get_time() > s_ps.done_deadline_us) {
                ESP_LOGW(TAG, "pairing: DONE timeout, still listening");
                pairing_cleanup(false);
                pairing_start();   /* reopen the window */
            }
            break;
        case MSG_BEACON:
            if (s_ps.st == PS_BEACON) {
                pairing_beacon();
            }
            break;
        case MSG_PAIR_START:
            pairing_start();
            break;
        case MSG_PAIR_CANCEL:
            if (s_ps.st != PS_IDLE) {
                pairing_cleanup(true);
            }
            break;
        case MSG_UNPAIR: {
            uint8_t slot = m.slot;
            const hub_pairing_t *p = store_pairing(slot);
            if (!p) {
                break;
            }
            /* Best effort: tell the dongle to wipe itself (encrypted, so
             * only the real dongle can act on it), then forget locally. */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < 3; i++) {
                send_control_locked(slot, KVM_CTL_UNPAIR);
            }
            uint8_t mac[6];
            memcpy(mac, p->mac, 6);
            if (s_active == slot) {
                s_active = KVM_SLOT_NONE;
            }
            xSemaphoreGive(s_lock);

            vTaskDelay(pdMS_TO_TICKS(50));   /* let the frames drain */
            esp_now_del_peer(mac);
            store_erase_pairing(slot);
            s_health[slot].online = false;
            s_health[slot].fails = 0;
            if (s_cb) {
                s_cb(LINK_EVT_STATUS, slot);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

esp_err_t link_init(link_event_cb_t cb)
{
    s_cb = cb;
    s_q = xQueueCreate(24, sizeof(link_msg_t));
    s_lock = xSemaphoreCreateMutex();
    if (!s_q || !s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPKVM_CHANNEL,
                                         WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* latency! */

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk(store_pmk()));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    /* Broadcast pseudo-peer (for pairing beacons; never encrypted). */
    ESP_ERROR_CHECK(set_peer(BCAST, NULL));

    /* Re-register every paired dongle as an encrypted peer. */
    for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
        const hub_pairing_t *p = store_pairing(i);
        if (p) {
            ESP_ERROR_CHECK(set_peer(p->mac, p->lmk));
        }
    }

    /* Failsafe: come back on the machine that was active before reboot. */
    uint8_t last = store_last_slot();
    if (store_pairing(last)) {
        s_active = last;
    } else {
        for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
            if (store_pairing(i)) {
                s_active = i;
                break;
            }
        }
    }

    xTaskCreate(link_task, "kvm_link", 6144, NULL, 10, NULL);

    const esp_timer_create_args_t tick_args = {
        .callback = tick_timer_cb, .name = "kvm_tick",
    };
    esp_timer_handle_t tick;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, TICK_PERIOD_MS * 1000));

    const esp_timer_create_args_t beacon_args = {
        .callback = beacon_timer_cb, .name = "kvm_beacon",
    };
    esp_timer_handle_t beacon;
    ESP_ERROR_CHECK(esp_timer_create(&beacon_args, &beacon));
    ESP_ERROR_CHECK(esp_timer_start_periodic(beacon, BEACON_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "up on channel %d, active slot %u",
             CONFIG_ESPKVM_CHANNEL, s_active);
    return ESP_OK;
}

void link_start_pairing(void)
{
    link_msg_t m = { .type = MSG_PAIR_START };
    xQueueSend(s_q, &m, 0);
}

void link_cancel_pairing(void)
{
    link_msg_t m = { .type = MSG_PAIR_CANCEL };
    xQueueSend(s_q, &m, 0);
}

bool link_pairing_active(void)
{
    return s_ps.st != PS_IDLE;
}

void link_unpair(uint8_t slot)
{
    link_msg_t m = { .type = MSG_UNPAIR, .slot = slot };
    xQueueSend(s_q, &m, 0);
}

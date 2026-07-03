/*
 * espkvm hub-air — phone web UI.
 *
 * One ESP-IDF HTTP server does two jobs:
 *   GET /    serves the single-page app embedded into the firmware image
 *            (main/web/index.html, pulled in via EMBED_TXTFILES — no
 *            SPIFFS partition, it just lives in .rodata).
 *   /ws      a WebSocket carrying small JSON messages both ways. Input
 *            events (keyboard/mouse/consumer) flow phone -> hub and are
 *            handed straight to kvm_hublink's link_send_*(); slot state
 *            flows hub -> phone whenever kvm_hublink reports an event.
 *
 * Wire format is documented at the top of main/web/index.html next to the
 * JS that speaks it, so the two stay next to each other in one glance.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "kvm_proto.h"
#include "store.h"
#include "link.h"
#include "webui.h"

static const char *TAG = "webui";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server;

/* ------------------------------------------------------------------ */
/* Outbound: hub -> every connected phone                              */
/* ------------------------------------------------------------------ */

static void broadcast_json(cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return;
    }

    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)body,
        .len = strlen(body),
    };

    /* esp_http_server tracks open sockets for us; we just filter for the
     * ones that completed a WebSocket handshake. httpd_ws_send_frame_async
     * is explicitly documented as safe to call from a task other than the
     * httpd worker — exactly our case, called from the kvm_hublink task. */
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t fd_count = CONFIG_LWIP_MAX_SOCKETS;
    if (s_server && httpd_get_client_list(s_server, &fd_count, fds) == ESP_OK) {
        for (size_t i = 0; i < fd_count; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(s_server, fds[i], &pkt);
            }
        }
    }
    free(body);
}

static cJSON *build_slots_snapshot(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "t", "slots");
    cJSON_AddNumberToObject(root, "active", link_active_slot());
    cJSON_AddBoolToObject(root, "pairing", link_pairing_active());

    cJSON *arr = cJSON_AddArrayToObject(root, "slots");
    for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
        const hub_pairing_t *p = store_pairing(i);
        if (!p) {
            continue;
        }
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "n", i);
        cJSON_AddStringToObject(s, "name", p->name);
        cJSON_AddBoolToObject(s, "online", link_slot_online(i));
        cJSON_AddItemToArray(arr, s);
    }
    return root;
}

static void broadcast_slots(void)
{
    broadcast_json(build_slots_snapshot());
}

static void broadcast_toast(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "t", "msg");
    cJSON_AddStringToObject(root, "text", text);
    broadcast_json(root);
}

void webui_link_event(link_event_t evt, uint8_t slot)
{
    switch (evt) {
    case LINK_EVT_PAIRED: {
        const hub_pairing_t *p = store_pairing(slot);
        char msg[40];
        snprintf(msg, sizeof(msg), "Paired %s", p ? p->name : "dongle");
        broadcast_toast(msg);
        broadcast_slots();
        break;
    }
    case LINK_EVT_PAIR_FAIL:
        broadcast_toast("All 10 slots are already paired");
        break;
    case LINK_EVT_PAIR_TIMEOUT:
        broadcast_toast("Pairing window closed");
        broadcast_slots();
        break;
    case LINK_EVT_STATUS:
    case LINK_EVT_SWITCHED:
        broadcast_slots();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Inbound: phone -> hub                                               */
/* ------------------------------------------------------------------ */

static uint8_t json_u8(const cJSON *o, const char *key, uint8_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (uint8_t)v->valueint : dflt;
}

static int16_t json_i16(const cJSON *o, const char *key, int16_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int16_t)v->valueint : dflt;
}

static void handle_message(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return;
    }
    const cJSON *tj = cJSON_GetObjectItemCaseSensitive(root, "t");
    const char *t = cJSON_IsString(tj) ? tj->valuestring : "";

    if (strcmp(t, "hello") == 0) {
        broadcast_slots();
    } else if (strcmp(t, "kbd") == 0) {
        uint8_t mods = json_u8(root, "m", 0);
        uint8_t keys[6] = {0};
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "k");
        if (cJSON_IsArray(k)) {
            int i = 0;
            const cJSON *e;
            cJSON_ArrayForEach(e, k) {
                if (i >= 6) break;
                keys[i++] = cJSON_IsNumber(e) ? (uint8_t)e->valueint : 0;
            }
        }
        link_send_kbd(mods, keys);
    } else if (strcmp(t, "mouse") == 0) {
        link_send_mouse(json_u8(root, "b", 0),
                        json_i16(root, "dx", 0), json_i16(root, "dy", 0),
                        (int8_t)json_i16(root, "w", 0),
                        (int8_t)json_i16(root, "p", 0));
    } else if (strcmp(t, "cons") == 0) {
        link_send_consumer((uint16_t)json_i16(root, "u", 0));
    } else if (strcmp(t, "switch") == 0) {
        link_switch(json_u8(root, "slot", KVM_SLOT_NONE));
    } else if (strcmp(t, "pair") == 0) {
        link_start_pairing();
        broadcast_slots();
    } else if (strcmp(t, "cancel") == 0) {
        link_cancel_pairing();
        broadcast_slots();
    } else if (strcmp(t, "unpair") == 0) {
        link_unpair(json_u8(root, "slot", KVM_SLOT_NONE));
        broadcast_slots();
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "phone connected (fd %d)", httpd_req_to_sockfd(req));
        return ESP_OK;   /* handshake only */
    }

    httpd_ws_frame_t pkt = {0};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK || pkt.len == 0 || pkt.len > 512) {
        return err;
    }

    uint8_t *buf = malloc(pkt.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;
    err = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (err == ESP_OK) {
        buf[pkt.len] = '\0';
        handle_message((const char *)buf, pkt.len);
    }
    free(buf);
    return err;
}

esp_err_t webui_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = CONFIG_ESPKVM_AP_MAX_CLIENTS + 2;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler,
    };
    const httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &ws_uri);

    ESP_LOGI(TAG, "web UI up: connect to Wi-Fi \"%s\" then browse to "
             "http://192.168.4.1/", CONFIG_ESPKVM_AP_SSID);
    return ESP_OK;
}

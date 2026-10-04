/* ESP32-C3 standalone projectMac -> Tuya BLE-beacon lamp bridge.
 *
 * The whole pipeline runs here, no host process needed: listens for
 * projectMac's /projectmac/... OSC scene stream on UDP (CONFIG_BRIDGE_OSC_PORT),
 * maps it to lamp commands (lamp.c), encrypts them into 26-byte Tuya beacon
 * frames and broadcasts those as BLE advertising data. A web panel on port 80
 * shows live meters and takes manual overrides / mode changes.
 *
 * Pairing keys, WiFi credentials and the sn/revision starting points are set
 * via `idf.py menuconfig` -> "Lamp bridge" (stored in the gitignored sdkconfig).
 *
 * lamp.c holds the pure protocol/mapping logic (host-tested by
 * test/test_lamp.c); this file the pipeline, OSC listener and web panel.
 *
 * BLE advertising: real capture showed the phone app broadcasting legacy,
 * connectable, scannable advertising (ADV_IND) with the 26-byte frame disguised
 * as a fake "Complete List of 16-bit Service Class UUIDs" AD structure (type
 * 0x03) -- esp_ble_gap_config_adv_data_raw() takes the complete AD payload
 * literally, so we build that exact structure ourselves, preceded by a standard
 * Flags AD structure (which Android's stack adds automatically and which real
 * captures also show) -- 3 + 28 = 31 bytes, exactly the legacy adv limit.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lamp.h"

static const char *TAG = "lamp_bridge";

#define FRAME_LEN LAMP_FRAME_LEN
#define ADV_LEN (3 + 2 + FRAME_LEN) /* flags AD (3) + our AD header (2) + frame */
#define FRAME_OFFSET 5 /* index into adv_payload where the 26 frame bytes start */

static uint8_t adv_payload[ADV_LEN] = {
    0x02, 0x01, 0x06, /* Flags: LE General Discoverable Mode, BR/EDR Not Supported */
    0x1B, 0x03,       /* len=27 (type+26 data bytes), type=0x03 Complete 16-bit UUID list */
    /* 26 frame bytes filled in per-message below */
};

static volatile bool s_advertising = false;

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (!s_advertising) {
            static esp_ble_adv_params_t adv_params = {
                .adv_int_min = 0x20, /* 20ms */
                .adv_int_max = 0x40, /* 40ms */
                .adv_type = ADV_TYPE_IND, /* legacy, connectable, scannable */
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            };
            esp_err_t err = esp_ble_gap_start_advertising(&adv_params);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "start_advertising failed: %d", err);
            }
        }
        /* else: already advertising -- the controller applies updated adv
         * data live, no restart needed. */
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = true;
            ESP_LOGI(TAG, "advertising started");
        } else {
            ESP_LOGE(TAG, "advertising start failed: status=%d", param->adv_start_cmpl.status);
        }
        break;
    default:
        break;
    }
}

/* Pushes a freshly-received 26-byte frame to the BLE controller. */
static void apply_frame(const uint8_t *frame)
{
    memcpy(&adv_payload[FRAME_OFFSET], frame, FRAME_LEN);
    esp_err_t err = esp_ble_gap_config_adv_data_raw(adv_payload, ADV_LEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data_raw failed: %d", err);
    } else {
        ESP_LOGD(TAG, "frame queued");
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *hex, uint8_t *out, int n_bytes)
{
    if ((int)strlen(hex) != n_bytes * 2) {
        return false;
    }
    for (int i = 0; i < n_bytes; i++) {
        int hi = hex_nibble(hex[i * 2]), lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static lamp_keys_t s_keys;

static void keys_init(void)
{
    if (strlen(CONFIG_BRIDGE_LOCAL_KEY) != 16 || !parse_hex(CONFIG_BRIDGE_APP_KEY, s_keys.app_key, 16) ||
        !parse_hex(CONFIG_BRIDGE_SRC_ADDR, s_keys.src_addr, 2) || !parse_hex(CONFIG_BRIDGE_DST_ADDR, s_keys.dst_addr, 2)) {
        ESP_LOGE(TAG, "lamp keys missing/malformed -- set them in menuconfig -> Lamp bridge");
        abort();
    }
    memcpy(s_keys.local_key, CONFIG_BRIDGE_LOCAL_KEY, 16); /* 16 ASCII chars used as raw bytes */
}

static void ble_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        /* ponytail: retries forever with no backoff -- fine for a single AP */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "wifi up -- web panel at http://" IPSTR "/, point projectMac's OSC at " IPSTR ":%d",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.ip), CONFIG_BRIDGE_OSC_PORT);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_BRIDGE_WIFI_SSID,
            .password = CONFIG_BRIDGE_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep (the default, and required alongside BLE) saves battery but
     * adds up to one DTIM interval (~100-300ms) of receive latency. */
}

/* ---- persistent counters ----
 *
 * The lamp drops any frame whose sn isn't higher than the last one it took
 * from this src_addr, so these must never go backwards, across reboots too.
 * That state is per address and survives other addresses' traffic; pairing
 * mode resets it, and a never-used src_addr starts fresh (a one-time escape
 * hatch if the high-water mark is ever lost). The scene revision byte is
 * treated the same way: an earlier test script that restarted it at 1 every
 * run coincided with the lamp locking up until a power cycle. Persisting on every
 * send would wear the flash out in weeks, so NVS holds a reserved ceiling
 * instead: values up to it may have been used, and boot resumes from it. */

typedef struct {
    const char *key;
    uint32_t modulus, block, value, ceiling;
} counter_t;

static counter_t s_sn = {.key = "sn", .modulus = 0x10000, .block = 256};
static counter_t s_revision = {.key = "revision", .modulus = 256, .block = 16};
static nvs_handle_t s_nvs;

/* Values cycle 1..modulus-1, skipping 0 (treated as reserved). */
static uint32_t counter_advance(const counter_t *c, uint32_t v, uint32_t n)
{
    return (v + c->modulus - 2 + n) % (c->modulus - 1) + 1;
}

static void counter_reserve(counter_t *c)
{
    c->ceiling = counter_advance(c, c->value, c->block);
    ESP_ERROR_CHECK(nvs_set_u32(s_nvs, c->key, c->ceiling));
    ESP_ERROR_CHECK(nvs_commit(s_nvs));
}

static void counter_init(counter_t *c, uint32_t floor)
{
    if (nvs_get_u32(s_nvs, c->key, &c->value) != ESP_OK) {
        c->value = floor; /* first boot: start above the lamp's known high-water mark */
    }
    ESP_LOGI(TAG, "%s counter resuming at %lu", c->key, (unsigned long)c->value);
    counter_reserve(c);
}

static uint32_t counter_next(counter_t *c)
{
    c->value = counter_advance(c, c->value, 1);
    if (c->value == c->ceiling) {
        counter_reserve(c);
    }
    return c->value;
}

/* ---- pipeline ----
 * Event-driven: every OSC message re-evaluates the target, and a frame goes
 * out only when it differs from the last one sent, so a burst of hues in the
 * same bracket collapses to one command instead of restarting the effect. */

/* Protects the lamp's BLE receiver; the real ceiling is untested. */
#define MIN_SEND_INTERVAL_SECONDS 0.1

static const char *MODE_NAMES[] = {"color", "pattern", "combined"};
static const char *PATTERN_NAMES[] = {"static", "jump", "gradient", "flash", "breath"};

static struct {
    bool active;
    int kind; /* TARGET_COLOR | TARGET_PATTERN */
    int hue, saturation, brightness; /* dp=11; brightness shared with dp=73 */
    int pattern, colors, speed;      /* dp=73 */
} s_override = {.kind = TARGET_COLOR, .saturation = 100, .brightness = 100,
                .pattern = PATTERN_STATIC, .colors = COLOR_RED, .speed = 50};

/* ponytail: one lock over all pipeline state -- OSC, web and status push are low-rate */
static SemaphoreHandle_t s_lock;
static lamp_scene_t s_scene;
static char s_preset_name[64];
static int s_mode = MODE_COMBINED;
static lamp_target_t s_last_sent;
static bool s_sent_any;
static double s_last_send_time = -1e9;

static double now_s(void)
{
    return esp_timer_get_time() / 1e6;
}

static lamp_target_t current_target(double now)
{
    if (s_override.active) {
        if (s_override.kind == TARGET_COLOR) {
            return (lamp_target_t){TARGET_COLOR, s_override.hue, s_override.saturation, s_override.brightness, 0};
        }
        return (lamp_target_t){TARGET_PATTERN, s_override.pattern, s_override.colors, s_override.speed,
                               s_override.brightness};
    }
    return lamp_map(&s_scene, s_mode, now);
}

/* Call (with s_lock held) after anything that could change the target.
 * Only sends when the target differs from what was last sent; a rate-limited
 * send isn't lost, the next call retries it. */
static void on_state_changed(void)
{
    double now = now_s();
    lamp_target_t target = current_target(now);
    if (s_sent_any && memcmp(&target, &s_last_sent, sizeof(target)) == 0) {
        return;
    }
    if (now - s_last_send_time < MIN_SEND_INTERVAL_SECONDS) {
        return;
    }
    s_last_send_time = now;

    uint8_t frame[LAMP_FRAME_LEN];
    uint8_t revision = target.kind == TARGET_PATTERN ? counter_next(&s_revision) : 0;
    lamp_build_frame(&target, &s_keys, counter_next(&s_sn), revision, frame);
    apply_frame(frame);
    s_last_sent = target;
    s_sent_any = true;
}

/* ---- OSC listener ---- */

static int osc_padded(int len) /* string + NUL, padded to 4 bytes */
{
    return (len + 4) & ~3;
}

static float osc_float(const char *p)
{
    uint32_t u = (uint32_t)(uint8_t)p[0] << 24 | (uint32_t)(uint8_t)p[1] << 16 | (uint32_t)(uint8_t)p[2] << 8 | (uint8_t)p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Single messages only -- projectMac sends one address per datagram, no bundles. */
static void handle_osc(const char *buf, int len)
{
    int addr_len = strnlen(buf, len);
    int off = osc_padded(addr_len);
    if (addr_len == len || off >= len || buf[off] != ',' || strncmp(buf, "/projectmac/", 12) != 0) {
        return;
    }
    const char *tags = buf + off + 1;
    int tags_len = strnlen(buf + off, len - off);
    if (tags_len == len - off) {
        return;
    }
    off += osc_padded(tags_len);

    float f[3];
    int nf = 0;
    char str[sizeof(s_preset_name)] = "";
    for (const char *t = tags; *t; t++) {
        if (*t == 'f' && off + 4 <= len) {
            if (nf < 3) f[nf++] = osc_float(buf + off);
            off += 4;
        } else if (*t == 's' && off < len) {
            int slen = strnlen(buf + off, len - off);
            if (slen == len - off) return;
            snprintf(str, sizeof(str), "%.*s", slen, buf + off);
            off += osc_padded(slen);
        } else {
            return; /* unexpected type or truncated */
        }
    }

    const char *a = buf + 12;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!strcmp(a, "scene/vibrant") && nf == 3) {
        lamp_set_vibrant(&s_scene, f[0], f[1], f[2], now_s());
    } else if (!strcmp(a, "scene/brightness") && nf == 1) {
        s_scene.brightness = f[0];
    } else if (!strcmp(a, "tempo/bpm") && nf == 1) {
        s_scene.tempo_bpm = f[0];
    } else if (!strcmp(a, "visual/bpm") && nf == 1) {
        s_scene.visual_bpm = f[0];
    } else if (!strcmp(a, "preset/name")) {
        strcpy(s_preset_name, str);
    }
    /* every /projectmac message re-evaluates -- also how a rate-limited send
     * or the hue-hold bpm switch gets picked up */
    on_state_changed();
    xSemaphoreGive(s_lock);
}

static void osc_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_BRIDGE_OSC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "osc socket/bind failed: errno %d", errno);
        abort();
    }
    char buf[512];
    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len > 0) {
            handle_osc(buf, len);
        }
    }
}

/* ---- web panel ---- */

extern const char index_gz_start[] asm("_binary_index_html_gz_start");
extern const char index_gz_end[] asm("_binary_index_html_gz_end");
static httpd_handle_t s_server;

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip"); /* gzipped at build time, see CMakeLists.txt */
    return httpd_resp_send(req, index_gz_start, index_gz_end - index_gz_start);
}

/* ponytail: flat-object key lookup, enough for the panel's own messages;
 * a real JSON parser if the message shapes ever get nested */
/* Points just past `"key":` (whitespace around the colon allowed), or NULL. */
static const char *json_value(const char *j, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p++ != ':') return NULL;
    while (*p == ' ') p++;
    return p;
}

static bool json_str(const char *j, const char *key, char *out, size_t n)
{
    const char *p = json_value(j, key);
    if (!p || *p++ != '"') return false;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= n) return false;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return true;
}

static bool json_int(const char *j, const char *key, int *out)
{
    const char *p = json_value(j, key);
    if (!p) return false;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

static int name_index(const char *name, const char **names, int n)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(name, names[i])) return i;
    }
    return -1;
}

static bool pct_ok(int v)
{
    return v >= 0 && v <= 100;
}

/* Called with s_lock held. Returns false for anything malformed (ignored). */
static bool handle_command(const char *msg)
{
    char type[32], val[16];
    if (!json_str(msg, "type", type, sizeof(type))) return false;

    if (!strcmp(type, "override_set")) {
        int hue, saturation, brightness;
        if (!json_int(msg, "hue", &hue) || !json_int(msg, "saturation", &saturation) ||
            !json_int(msg, "brightness", &brightness) || hue < 0 || hue > 359 || !pct_ok(saturation) || !pct_ok(brightness)) {
            return false;
        }
        s_override.hue = hue;
        s_override.saturation = saturation;
        s_override.brightness = brightness;
        s_override.active = true;
        s_override.kind = TARGET_COLOR;
    } else if (!strcmp(type, "override_pattern_set")) {
        int pattern, colors = s_override.colors, speed = s_override.speed, brightness = s_override.brightness;
        if (!json_str(msg, "pattern", val, sizeof(val))) return false;
        pattern = name_index(val, PATTERN_NAMES, 5);
        json_int(msg, "colors", &colors);
        json_int(msg, "speed", &speed);
        json_int(msg, "brightness", &brightness);
        /* speed is a full byte on the wire, but the lamp only takes 0-100: a real
         * test at 120 made it fall back to static red */
        if (!lamp_colors_valid(pattern, colors) || !pct_ok(speed) || !pct_ok(brightness)) return false;
        s_override.active = true;
        s_override.kind = TARGET_PATTERN;
        s_override.pattern = pattern;
        s_override.colors = colors;
        s_override.speed = speed;
        s_override.brightness = brightness;
    } else if (!strcmp(type, "override_release")) {
        s_override.active = false;
    } else if (!strcmp(type, "visual_scale_set")) {
        int pct;
        if (!json_int(msg, "percent", &pct) || pct < 10 || pct > 200) return false;
        s_scene.visual_scale_pct = pct;
    } else if (!strcmp(type, "mode_set")) {
        int mode;
        if (!json_str(msg, "mode", val, sizeof(val)) || (mode = name_index(val, MODE_NAMES, 3)) < 0) return false;
        s_mode = mode;
    } else {
        return false;
    }
    return true;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK; /* handshake done */
    }
    char buf[256];
    httpd_ws_frame_t frame = {.payload = (uint8_t *)buf};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    if (err != ESP_OK || frame.type != HTTPD_WS_TYPE_TEXT) {
        return err;
    }
    buf[frame.len] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (handle_command(buf)) {
        on_state_changed(); /* don't wait for the next OSC message to react */
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void json_escape(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        unsigned char c = *in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20) {
            o += snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* Runs on the httpd task (queued by status_task). */
static volatile bool s_push_queued; /* one status push in flight at most */

static void push_status(void *arg)
{
    s_push_queued = false;
    char preset[sizeof(s_preset_name) * 6 + 1], json[1024];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on_state_changed(); /* retries a rate-limited send even with no OSC flowing, e.g. a slider drag's last value */
    double now = now_s();
    lamp_target_t color = s_override.active && s_override.kind == TARGET_COLOR
                              ? (lamp_target_t){TARGET_COLOR, s_override.hue, s_override.saturation, s_override.brightness, 0}
                              : lamp_color_follow(&s_scene);
    json_escape(preset, sizeof(preset), s_preset_name);
    int len = snprintf(json, sizeof(json),
        "{\"vibrant_hsv\":[%.4f,%.4f,%.4f],\"rate_bpm\":%.2f,\"tempo_bpm\":%.2f,\"visual_bpm\":%.2f,\"visual_scale_pct\":%d,\"preset_name\":\"%s\","
        "\"override_active\":%s,\"override_kind\":\"%s\",\"override_pattern\":\"%s\","
        "\"override_speed\":%d,\"override_colors\":%d,\"mode\":\"%s\","
        "\"hue\":%d,\"saturation\":%d,\"brightness\":%d}",
        s_scene.vibrant_h, s_scene.vibrant_s, s_scene.vibrant_v, lamp_rate_bpm(&s_scene, now), s_scene.tempo_bpm, s_scene.visual_bpm, s_scene.visual_scale_pct, preset,
        s_override.active ? "true" : "false", s_override.kind == TARGET_COLOR ? "color" : "pattern",
        PATTERN_NAMES[s_override.pattern], s_override.speed, s_override.colors, MODE_NAMES[s_mode],
        color.a, color.b, color.c);
    xSemaphoreGive(s_lock);
    if (len >= (int)sizeof(json)) {
        return; /* can't happen with the 63-char preset cap, but never send a truncated buffer */
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = len};
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                if (httpd_ws_send_frame_async(s_server, fds[i], &frame) != ESP_OK) {
                    httpd_sess_trigger_close(s_server, fds[i]); /* dead client (e.g. a locked phone): drop it */
                }
            }
        }
    }
}

static void status_task(void *arg)
{
    while (1) {
        /* skip a tick rather than pile up work while a slow client stalls httpd */
        if (!s_push_queued) {
            s_push_queued = true; /* before queueing: httpd outranks us and may run it at once */
            if (httpd_queue_work(s_server, push_status, NULL) != ESP_OK) {
                s_push_queued = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void web_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4; /* leaves LWIP sockets for OSC */
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 2; /* seconds; default 5 stalls everyone behind one dead client */
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_register_uri_handler(s_server, &index);
    httpd_register_uri_handler(s_server, &ws);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    keys_init();
    ESP_ERROR_CHECK(nvs_open("lamp", NVS_READWRITE, &s_nvs));
    counter_init(&s_sn, CONFIG_BRIDGE_SN_FLOOR);
    counter_init(&s_revision, CONFIG_BRIDGE_REVISION_FLOOR);
    s_lock = xSemaphoreCreateMutex();
    lamp_scene_init(&s_scene, now_s());

    ble_init();
    wifi_init();
    web_init();
    xTaskCreate(osc_task, "osc", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 2048, NULL, 4, NULL);
}

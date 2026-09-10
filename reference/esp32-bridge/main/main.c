/* ESP32-C3 raw BLE advertising bridge for a Tuya BLE-beacon smart lamp.
 *
 * A dumb relay: takes a pre-built, pre-encrypted 26-byte frame over USB
 * serial and broadcasts it as BLE advertising data exactly as given. Has
 * zero knowledge of the Tuya protocol itself (no keys, no frame encoding)
 * — that all happens upstream, before a frame ever reaches this firmware.
 * See ../../../PLAN.md and ../../tuya_beacon_codec.py for where these 26
 * bytes come from.
 *
 * Serial-only by design (115200 8N1): one line per frame, 52 ASCII hex
 * chars (the 26-byte encoded frame), terminated by '\n'. Malformed lines
 * are logged and ignored. An earlier version of this firmware also had an
 * untethered WiFi/UDP input path (opt-in, board runs battery-powered off a
 * cable) — deliberately not carried over here to keep this project's scope
 * to exactly what it needs; if that's wanted later, the WiFi/UDP variant is
 * a known-working reference to pull from, not something to redesign.
 *
 * Real capture showed the phone app broadcasting legacy, connectable,
 * scannable advertising (ADV_IND) with the 26-byte frame disguised as a fake
 * "Complete List of 16-bit Service Class UUIDs" AD structure (type 0x03) --
 * esp_ble_gap_config_adv_data_raw() takes the complete AD payload literally,
 * so we build that exact structure ourselves, preceded by a standard Flags AD
 * structure (which Android's stack adds automatically and which real
 * captures also show) -- 3 + 28 = 31 bytes, exactly the legacy adv limit.
 */
#include <string.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lamp_bridge";

#define FRAME_LEN 26
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
        ESP_LOGI(TAG, "frame queued");
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_frame_hex(const char *line, int len, uint8_t *out /* FRAME_LEN bytes */)
{
    if (len != FRAME_LEN * 2) {
        ESP_LOGW(TAG, "expected %d hex chars, got %d: %.*s", FRAME_LEN * 2, len, len, line);
        return false;
    }
    for (int i = 0; i < FRAME_LEN; i++) {
        int hi = hex_nibble(line[i * 2]);
        int lo = hex_nibble(line[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            ESP_LOGW(TAG, "non-hex char in frame: %.*s", len, line);
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
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

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ble_init();

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "ready -- send a %d-char hex frame + newline over serial", FRAME_LEN * 2);

    uint8_t rx_buf[256];
    char line[128];
    int line_len = 0;

    while (1) {
        int n = uart_read_bytes(UART_NUM_0, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            char c = (char)rx_buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    uint8_t frame[FRAME_LEN];
                    if (parse_frame_hex(line, line_len, frame)) {
                        apply_frame(frame);
                    }
                    line_len = 0;
                }
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
}

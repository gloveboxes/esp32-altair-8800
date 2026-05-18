#include "bt_keyboard.h"

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_hidh_bluedroid.h"
#include "esp_hidh_gattc.h"
#endif

#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

#define BT_KEYBOARD_INPUT_QUEUE_DEPTH 64
#define BT_KEYBOARD_SCAN_SECONDS 8
#define BT_KEYBOARD_TASK_STACK 4096  // observed HWM ~2.5 KB
#define BT_KEYBOARD_TASK_PRIORITY 4
#define BT_KEYBOARD_TASK_CORE 0
#define BT_KEYBOARD_OPEN_TIMEOUT_US (15LL * 1000LL * 1000LL)
#define BT_KEYBOARD_RECONNECT_SCAN_MIN_US (30LL * 1000LL * 1000LL)
#define BT_KEYBOARD_RECONNECT_SCAN_MAX_US (60LL * 1000LL * 1000LL)

static const char *TAG = "BT_Keyboard";

static QueueHandle_t s_input_queue = NULL;

#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
static volatile bool s_initialized = false;
static volatile bool s_scan_active = false;
static volatile bool s_opening = false;
static volatile bool s_connected = false;
static volatile bool s_pairing_requested = false;
static volatile bool s_disconnect_requested = false;
static volatile bool s_clear_bonds_requested = false;
static volatile bool s_pending_open = false;
static volatile bool s_connected_reported = false;
static volatile bool s_reset_reconnect_backoff_requested = false;
static int64_t s_open_started_us = 0;
static esp_bd_addr_t s_pending_bda;
static esp_ble_addr_type_t s_pending_addr_type;
static esp_hidh_dev_t *s_dev = NULL;
static uint8_t s_last_keys[6];
static int64_t s_next_reconnect_scan_us = 0;
static int64_t s_reconnect_scan_interval_us = BT_KEYBOARD_RECONNECT_SCAN_MIN_US;

static void reset_reconnect_scan_backoff(void)
{
    s_next_reconnect_scan_us = 0;
    s_reconnect_scan_interval_us = BT_KEYBOARD_RECONNECT_SCAN_MIN_US;
}

static void request_reconnect_scan_backoff_reset(void)
{
    s_reset_reconnect_backoff_requested = true;
}

static bool reconnect_scan_due(void)
{
    int64_t now = esp_timer_get_time();
    return s_next_reconnect_scan_us == 0 || now >= s_next_reconnect_scan_us;
}

static void reconnect_scan_started(void)
{
    int64_t now = esp_timer_get_time();
    s_next_reconnect_scan_us = now + s_reconnect_scan_interval_us;
    if (s_reconnect_scan_interval_us < BT_KEYBOARD_RECONNECT_SCAN_MAX_US) {
        s_reconnect_scan_interval_us *= 2;
        if (s_reconnect_scan_interval_us > BT_KEYBOARD_RECONNECT_SCAN_MAX_US) {
            s_reconnect_scan_interval_us = BT_KEYBOARD_RECONNECT_SCAN_MAX_US;
        }
    }
}

static void bt_keyboard_quiet_stack_logs(void)
{
    esp_log_level_set("BT_HCI", ESP_LOG_ERROR);
    esp_log_level_set("BT_APPL", ESP_LOG_ERROR);
    esp_log_level_set("BLE_HIDH", ESP_LOG_NONE);
}

static void report_keyboard_connected(esp_hidh_dev_t *dev)
{
    if (!s_connected_reported) {
        s_connected_reported = true;
        ESP_LOGI(TAG, "BLE keyboard connected: %s", esp_hidh_dev_name_get(dev));
    }
}

static void open_pending_keyboard(void)
{
    if (!s_pending_open || s_connected) return;

    s_pending_open = false;
    s_opening = true;
    s_connected_reported = false;
    s_open_started_us = esp_timer_get_time();

    esp_hidh_dev_t *dev = esp_hidh_dev_open(s_pending_bda, ESP_HID_TRANSPORT_BLE, s_pending_addr_type);
    if (dev == NULL) {
        ESP_LOGD(TAG, "BLE keyboard open did not complete for this advertisement; retrying scan");
        s_opening = false;
        s_open_started_us = 0;
        return;
    }

    s_dev = dev;
    s_connected = true;
    s_opening = false;
    s_open_started_us = 0;
    s_pairing_requested = false;
    memset(s_last_keys, 0, sizeof(s_last_keys));
    report_keyboard_connected(dev);
}

static const uint8_t s_keytable_us_none[] = {
    0xff, 0xff, 0xff, 0xff, 'a',  'b',  'c',  'd',  'e',  'f',  'g',  'h',  'i',  'j', 'k', 'l', 'm', 'n',
    'o',  'p',  'q',  'r',  's',  't',  'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2', '3', '4', '5', '6',
    '7',  '8',  '9',  '0',  '\r', 0x1b, 0x08, '\t', ' ',  '-',  '=',  '[',  ']',  '\\', 0xff, ';', '\'', '`',
    ',',  '.',  '/',  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const uint8_t s_keytable_us_shift[] = {
    0xff, 0xff, 0xff, 0xff, 'A',  'B',  'C',  'D',  'E',  'F',  'G',  'H',  'I',  'J', 'K', 'L', 'M', 'N',
    'O',  'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@', '#', '$', '%', '^',
    '&',  '*',  '(',  ')',  '\r', 0x1b, 0x08, '\t', ' ',  '_',  '+',  '{',  '}',  '|',  0xff, ':', '"', '~',
    '<',  '>',  '?',  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void enqueue_input(uint8_t value)
{
    if (s_input_queue == NULL) return;

    uint8_t ascii = (uint8_t)(value & ASCII_MASK_7BIT);
    if (xQueueSend(s_input_queue, &ascii, 0) != pdTRUE) {
        uint8_t discard;
        if (xQueueReceive(s_input_queue, &discard, 0) == pdTRUE) {
            xQueueSend(s_input_queue, &ascii, 0);
        }
    }
}

static void enqueue_sequence(const char *sequence)
{
    while (*sequence) {
        enqueue_input((uint8_t)*sequence++);
    }
}

static bool key_was_down(uint8_t key)
{
    for (int i = 0; i < 6; i++) {
        if (s_last_keys[i] == key) return true;
    }
    return false;
}

static void handle_key(uint8_t modifiers, uint8_t key)
{
    bool shift = (modifiers & 0x22) != 0;
    bool ctrl = (modifiers & 0x11) != 0;

    switch (key) {
        case 0x4f: enqueue_sequence("\x1b[C"); return;
        case 0x50: enqueue_sequence("\x1b[D"); return;
        case 0x51: enqueue_sequence("\x1b[B"); return;
        case 0x52: enqueue_sequence("\x1b[A"); return;
        case 0x4c: enqueue_input(0x7f); return;
        default: break;
    }

    if (key >= sizeof(s_keytable_us_none)) return;

    uint8_t ascii = shift ? s_keytable_us_shift[key] : s_keytable_us_none[key];
    if (ascii == 0xff) return;

    if (ctrl && ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z'))) {
        /* Ctrl+M (HID usage 0x10) -> byte 28 (0x1C) to toggle CPU monitor,
         * matching the web terminal's Ctrl+M mapping. Without this,
         * Ctrl+M = 0x0D = Enter (indistinguishable). */
        if (key == 0x10) {
            enqueue_input(28);
            return;
        }
        if (ascii >= 'a' && ascii <= 'z') ascii = (uint8_t)(ascii - 'a' + 'A');
        ascii = (uint8_t)CTRL_KEY(ascii);
    }

    enqueue_input(ascii);
}

static void handle_keyboard_report(const uint8_t *data, uint16_t length, uint16_t report_id)
{
    if (data == NULL || length < 8) return;

    uint16_t offset = 0;
    if (report_id != 0 && length >= 9 && data[0] == (uint8_t)report_id) {
        offset = 1;
    }
    if (length < offset + 8) return;

    uint8_t modifiers = data[offset];
    const uint8_t *keys = &data[offset + 2];

    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (key == 0 || key_was_down(key)) continue;
        handle_key(modifiers, key);
    }

    memcpy(s_last_keys, keys, sizeof(s_last_keys));
}

static bool adv_contains_uuid16(const uint8_t *data, uint8_t length, uint16_t uuid)
{
    uint8_t index = 0;
    while (index + 1 < length) {
        uint8_t field_len = data[index++];
        if (field_len == 0 || index + field_len > length) break;

        uint8_t type = data[index++];
        uint8_t payload_len = field_len - 1;
        const uint8_t *payload = &data[index];

        if (type == 0x02 || type == 0x03) {
            for (uint8_t i = 0; i + 1 < payload_len; i += 2) {
                uint16_t found = (uint16_t)payload[i] | ((uint16_t)payload[i + 1] << 8);
                if (found == uuid) return true;
            }
        }

        index += payload_len;
    }
    return false;
}

static bool adv_looks_like_keyboard(const uint8_t *data, uint8_t length)
{
    uint8_t index = 0;
    while (index + 1 < length) {
        uint8_t field_len = data[index++];
        if (field_len == 0 || index + field_len > length) break;

        uint8_t type = data[index++];
        uint8_t payload_len = field_len - 1;
        const uint8_t *payload = &data[index];

        if (type == 0x19 && payload_len >= 2) {
            uint16_t appearance = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            if (appearance == ESP_HID_APPEARANCE_KEYBOARD) return true;
        }

        index += payload_len;
    }
    return false;
}

static void adv_copy_name(const uint8_t *data, uint8_t length, char *name, size_t name_len)
{
    if (name == NULL || name_len == 0) return;
    name[0] = '\0';

    uint8_t adv_name_len = 0;
    uint8_t *adv_name = esp_ble_resolve_adv_data_by_type((uint8_t *)data, length,
                                                          ESP_BLE_AD_TYPE_NAME_CMPL,
                                                          &adv_name_len);
    if (adv_name == NULL) {
        adv_name = esp_ble_resolve_adv_data_by_type((uint8_t *)data, length,
                                                     ESP_BLE_AD_TYPE_NAME_SHORT,
                                                     &adv_name_len);
    }

    if (adv_name == NULL || adv_name_len == 0) return;
    if (adv_name_len >= name_len) adv_name_len = (uint8_t)(name_len - 1);
    memcpy(name, adv_name, adv_name_len);
    name[adv_name_len] = '\0';
}

static bool name_looks_like_keyboard(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;

    char lowered[64];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(lowered) - 1; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        lowered[i] = ch;
    }
    lowered[i] = '\0';

    return strstr(lowered, "keyboard") != NULL ||
           strstr(lowered, "keybd") != NULL ||
           strstr(lowered, "keys") != NULL ||
           strstr(lowered, "kbd") != NULL;
}

static void bt_keyboard_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            s_scan_active = false;
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "BLE scan start failed: %d", param->scan_start_cmpl.status);
                s_scan_active = false;
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                s_scan_active = false;
                ESP_LOGD(TAG, "BLE keyboard scan complete: %d response(s)", param->scan_rst.num_resps);
                break;
            }

            if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || s_connected || s_opening) {
                break;
            }

            const uint8_t *adv = param->scan_rst.ble_adv;
            uint8_t adv_len = (uint8_t)(param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len);
            char name[64];
            adv_copy_name(adv, adv_len, name, sizeof(name));

            bool hid = adv_contains_uuid16(adv, adv_len, ESP_GATT_UUID_HID_SVC);
            bool keyboard = adv_looks_like_keyboard(adv, adv_len) || name_looks_like_keyboard(name);

            if (s_pairing_requested && name[0] != '\0') {
                ESP_LOGI(TAG, "BLE seen %02x:%02x:%02x:%02x:%02x:%02x rssi=%d name='%s'%s%s",
                         param->scan_rst.bda[0], param->scan_rst.bda[1], param->scan_rst.bda[2],
                         param->scan_rst.bda[3], param->scan_rst.bda[4], param->scan_rst.bda[5],
                         param->scan_rst.rssi, name, hid ? " hid" : "", keyboard ? " keyboard" : "");
            }

            if (!hid && !keyboard) break;

            memcpy(s_pending_bda, param->scan_rst.bda, sizeof(s_pending_bda));
            s_pending_addr_type = param->scan_rst.ble_addr_type;
            s_pending_open = true;
            s_scan_active = false;
            ESP_LOGD(TAG, "Opening BLE HID keyboard %02x:%02x:%02x:%02x:%02x:%02x%s%s%s",
                     param->scan_rst.bda[0], param->scan_rst.bda[1], param->scan_rst.bda[2],
                     param->scan_rst.bda[3], param->scan_rst.bda[4], param->scan_rst.bda[5],
                     name[0] ? " name='" : "", name[0] ? name : "", name[0] ? "'" : "");
            esp_err_t stop_err = esp_ble_gap_stop_scanning();
            if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "BLE scan stop before open failed: %s", esp_err_to_name(stop_err));
                s_scan_active = false;
            }
            break;
        }

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            s_scan_active = false;
            break;

        case ESP_GAP_BLE_SCAN_TIMEOUT_EVT:
            s_scan_active = false;
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            ESP_LOGI(TAG, "BLE keyboard numeric comparison: %lu", (unsigned long)param->ble_security.key_notif.passkey);
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
            break;

        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            ESP_LOGI(TAG, "BLE keyboard passkey: %lu", (unsigned long)param->ble_security.key_notif.passkey);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (param->ble_security.auth_cmpl.success) {
                ESP_LOGI(TAG, "BLE keyboard bonded");
            } else {
                ESP_LOGW(TAG, "BLE keyboard auth failed: 0x%x", param->ble_security.auth_cmpl.fail_reason);
            }
            break;

        default:
            break;
    }
}

static void bt_keyboard_hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
        case ESP_HIDH_OPEN_EVENT:
            if (param->open.status == ESP_OK) {
                s_connected = true;
                s_opening = false;
                s_open_started_us = 0;
                s_pairing_requested = false;
                s_dev = param->open.dev;
                request_reconnect_scan_backoff_reset();
                memset(s_last_keys, 0, sizeof(s_last_keys));
                report_keyboard_connected(param->open.dev);
            } else {
                s_connected = false;
                s_connected_reported = false;
                s_opening = false;
                s_open_started_us = 0;
                ESP_LOGD(TAG, "BLE keyboard open failed: %s", esp_err_to_name(param->open.status));
            }
            break;

        case ESP_HIDH_INPUT_EVENT:
            if (param->input.usage == ESP_HID_USAGE_KEYBOARD || param->input.length >= 8) {
                handle_keyboard_report(param->input.data, param->input.length, param->input.report_id);
            }
            break;

        case ESP_HIDH_CLOSE_EVENT:
            ESP_LOGI(TAG, "BLE keyboard disconnected");
            s_connected = false;
            s_connected_reported = false;
            s_opening = false;
            s_pending_open = false;
            s_open_started_us = 0;
            s_dev = NULL;
            request_reconnect_scan_backoff_reset();
            memset(s_last_keys, 0, sizeof(s_last_keys));
            if (param->close.dev) {
                esp_hidh_dev_free(param->close.dev);
            }
            break;

        default:
            break;
    }
}

static void bt_keyboard_task(void *pvParameters)
{
    (void)pvParameters;

    bt_keyboard_quiet_stack_logs();

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "classic BT memory release failed: %s", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(bt_keyboard_gap_callback));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    esp_hidh_config_t hidh_config = {
        .callback = bt_keyboard_hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_config));

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x60,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
    };
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
    ESP_LOGI(TAG, "BLE keyboard host ready; put keyboard in pairing mode");
    s_initialized = true;

    for (;;) {
        if (s_reset_reconnect_backoff_requested) {
            s_reset_reconnect_backoff_requested = false;
            reset_reconnect_scan_backoff();
        }

        if (s_clear_bonds_requested) {
            s_clear_bonds_requested = false;
            reset_reconnect_scan_backoff();
            int bonded_count = esp_ble_get_bond_device_num();
            if (bonded_count > 0) {
                esp_ble_bond_dev_t *bonded_devices = calloc((size_t)bonded_count, sizeof(esp_ble_bond_dev_t));
                if (bonded_devices) {
                    if (esp_ble_get_bond_device_list(&bonded_count, bonded_devices) == ESP_OK) {
                        for (int i = 0; i < bonded_count; i++) {
                            esp_ble_remove_bond_device(bonded_devices[i].bd_addr);
                        }
                    }
                    free(bonded_devices);
                }
            }
            ESP_LOGI(TAG, "cleared BLE keyboard bonds");
        }

        if (s_disconnect_requested) {
            s_disconnect_requested = false;
            s_pairing_requested = false;
            s_pending_open = false;
            reset_reconnect_scan_backoff();
            if (s_scan_active) {
                esp_ble_gap_stop_scanning();
                s_scan_active = false;
            }
            if (s_dev) {
                esp_hidh_dev_close(s_dev);
            }
        }

        if (s_pending_open && !s_scan_active && !s_connected && !s_opening) {
            open_pending_keyboard();
        }

        if (s_opening && s_open_started_us != 0 &&
            (esp_timer_get_time() - s_open_started_us) > BT_KEYBOARD_OPEN_TIMEOUT_US) {
            ESP_LOGW(TAG, "BLE keyboard open timed out; retrying scan");
            s_opening = false;
            s_open_started_us = 0;
            if (s_dev) {
                esp_hidh_dev_close(s_dev);
                s_dev = NULL;
            }
        }

        bool has_bond = esp_ble_get_bond_device_num() > 0;
        bool should_scan = s_pairing_requested || (has_bond && reconnect_scan_due());
        if (should_scan && !s_connected && !s_pending_open && !s_opening && !s_scan_active) {
            err = esp_ble_gap_start_scanning(BT_KEYBOARD_SCAN_SECONDS);
            if (err == ESP_OK) {
                s_scan_active = true;
                if (!s_pairing_requested) {
                    reconnect_scan_started();
                }
            } else {
                ESP_LOGW(TAG, "BLE keyboard scan start failed: %s", esp_err_to_name(err));
                if (!s_pairing_requested) {
                    reconnect_scan_started();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

bool bt_keyboard_init(void)
{
    if (s_input_queue == NULL) {
        s_input_queue = xQueueCreate(BT_KEYBOARD_INPUT_QUEUE_DEPTH, sizeof(uint8_t));
        if (s_input_queue == NULL) {
            ESP_LOGE(TAG, "input queue alloc failed");
            return false;
        }
    }

#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    if (s_initialized) return true;

    BaseType_t ret = xTaskCreatePinnedToCore(bt_keyboard_task, "bt_keyboard", BT_KEYBOARD_TASK_STACK,
                                             NULL, BT_KEYBOARD_TASK_PRIORITY, NULL, BT_KEYBOARD_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    return true;
#else
    ESP_LOGW(TAG, "Bluetooth keyboard disabled in sdkconfig");
    return false;
#endif
}

bool bt_keyboard_try_dequeue_input(uint8_t *value)
{
    if (s_input_queue == NULL || value == NULL) return false;
    return xQueueReceive(s_input_queue, value, 0) == pdTRUE;
}

void bt_keyboard_request_pairing(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    request_reconnect_scan_backoff_reset();
    s_pairing_requested = true;
    s_disconnect_requested = false;
#endif
}

void bt_keyboard_request_disconnect(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    request_reconnect_scan_backoff_reset();
    s_disconnect_requested = true;
#endif
}

void bt_keyboard_request_clear_bonds(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    request_reconnect_scan_backoff_reset();
    s_clear_bonds_requested = true;
#endif
}

bool bt_keyboard_is_ready(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    return s_initialized;
#else
    return false;
#endif
}

bool bt_keyboard_is_connected(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    return s_connected;
#else
    return false;
#endif
}

bool bt_keyboard_is_connecting(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    return s_scan_active || s_pending_open || s_opening;
#else
    return false;
#endif
}

bool bt_keyboard_has_bond(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_BLE_ENABLED
    return esp_ble_get_bond_device_num() > 0;
#else
    return false;
#endif
}

/*============================================================================
 * Boot-time interactive Bluetooth keyboard manager
 * ---------------------------------------------------------------------------
 * Reads single-character commands from the USB serial JTAG console and drives
 * the BLE pairing / bond-management flows. Extracted from main.c.
 *==========================================================================*/

#include "driver/usb_serial_jtag.h"

static void bt_shell_serial_drain_line(uint32_t idle_timeout_ms)
{
    int64_t idle_start = esp_timer_get_time();
    while ((esp_timer_get_time() - idle_start) < ((int64_t)idle_timeout_ms * 1000))
    {
        uint8_t c;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
        if (len <= 0)
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            return;
        }
        idle_start = esp_timer_get_time();
    }
}

static int bt_shell_serial_read_command_ms(uint32_t timeout_ms)
{
    int64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) < ((int64_t)timeout_ms * 1000))
    {
        uint8_t c;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len <= 0)
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            return 0;
        }
        if (c == ' ' || c == '\t')
        {
            continue;
        }
        bt_shell_serial_drain_line(50);
        if (c >= 'a' && c <= 'z')
        {
            c = (uint8_t)(c - 'a' + 'A');
        }
        return c;
    }
    return -1;
}

void bt_keyboard_print_status(void)
{
    printf("Bluetooth keyboard: ");
    if (!bt_keyboard_is_ready())
    {
        printf("initializing");
    }
    else if (bt_keyboard_is_connected())
    {
        printf("connected");
    }
    else if (bt_keyboard_is_connecting())
    {
        printf("pairing or connecting");
    }
    else if (bt_keyboard_has_bond())
    {
        printf("bonded, waiting to reconnect");
    }
    else
    {
        printf("not paired");
    }
    printf("\n");
}

static bool bt_shell_wait_for_connection(uint32_t timeout_ms)
{
    int64_t start_time = esp_timer_get_time();
    int64_t next_status = start_time;

    while ((esp_timer_get_time() - start_time) < ((int64_t)timeout_ms * 1000))
    {
        if (bt_keyboard_is_connected())
        {
            printf("Keyboard connected, continuing boot.\n\n");
            return true;
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_status)
        {
            bt_keyboard_print_status();
            next_status = now + 5000000LL;
        }

        int cmd = bt_shell_serial_read_command_ms(250);
        if (cmd == 'Q')
        {
            printf("Leaving Bluetooth keyboard manager.\n\n");
            return true;
        }
        if (cmd == 'U')
        {
            bt_keyboard_request_clear_bonds();
        }
        if (cmd == 'D')
        {
            bt_keyboard_request_disconnect();
            return false;
        }
    }

    printf("Bluetooth keyboard pairing timed out.\n");
    return false;
}

static void bt_keyboard_print_menu(void)
{
    printf("\nBluetooth keyboard manager\n");
    printf("  P - pair with BLE keyboard\n");
    printf("  U - clear stored Bluetooth bond\n");
    printf("  D - disconnect current keyboard\n");
    printf("  S - show status\n");
    printf("  Q - return to main config menu\n");
}

void bt_keyboard_run_config_shell(void)
{
    bt_keyboard_print_menu();

    for (;;)
    {
        if (bt_keyboard_is_connected())
        {
            printf("Keyboard connected, continuing boot.\n\n");
            return;
        }

        printf("bt> ");
        int cmd = bt_shell_serial_read_command_ms(60000);
        printf("\n");
        if (cmd == -1)
        {
            printf("Bluetooth keyboard manager timed out.\n\n");
            return;
        }

        switch (cmd)
        {
        case 0:
            break;

        case 'P':
            printf("Starting BLE keyboard pairing flow...\n");
            printf("Select an empty Bluetooth slot on the keyboard, then hold the pairing key.\n");
            printf("Press Q to continue boot, U to clear bonds, or D to cancel.\n");
            bt_keyboard_request_pairing();
            if (bt_shell_wait_for_connection(45000))
            {
                return;
            }
            bt_keyboard_print_menu();
            break;

        case 'U':
            bt_keyboard_request_clear_bonds();
            bt_keyboard_print_menu();
            break;

        case 'D':
            bt_keyboard_request_disconnect();
            bt_keyboard_print_menu();
            break;

        case 'S':
            bt_keyboard_print_status();
            bt_keyboard_print_menu();
            break;

        case 'Q':
            printf("Leaving Bluetooth keyboard manager.\n\n");
            return;

        default:
            if (cmd > ' ')
            {
                printf("Unknown command '%c'. Use P, U, D, S, or Q.\n", (char)cmd);
            }
            break;
        }
    }
}

void bt_keyboard_run_boot_shell(void)
{
    if (!usb_serial_jtag_is_connected())
    {
        // No serial monitor attached, skip the interactive pairing prompt.
        return;
    }

    bt_keyboard_print_status();
    printf("Press 'B' within 5 seconds to manage Bluetooth keyboard pairing.\n");
    printf("Press Enter to continue boot now.\n");

    int c = bt_shell_serial_read_command_ms(5000);
    if (c == -1 || c == 0)
    {
        return;
    }

    if (c != 'B' && c != 'U')
    {
        return;
    }

    if (c == 'U')
    {
        bt_keyboard_request_clear_bonds();
    }

    bt_keyboard_run_config_shell();
}

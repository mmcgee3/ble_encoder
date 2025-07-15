/*
 *
 * This program is made for the use of transmitting the state of a rotary encoder over BLE
 * 
 * @author Max McGee
 * 
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"

#include "rotary_encoder.h"

#define TAG "BLE_ENCODER"
#define APP_ID_PLACEHOLDER 0

// GPIO Pin Definitions
#define ROT_ENC_A_GPIO      GPIO_NUM_8
#define ROT_ENC_B_GPIO      GPIO_NUM_9
#define BUTTON_GPIO         GPIO_NUM_10
#define RED_LED_GPIO        GPIO_NUM_2
#define GREEN_LED_GPIO      GPIO_NUM_1
#define BLUE_LED_GPIO       GPIO_NUM_0

// Configuration Constants
#define ENABLE_HALF_STEPS   false  // Set to true to enable tracking of rotary encoder at half step resolution
#define RESET_AT            0      // Set to a positive non-zero number to reset the position if this value is exceeded
#define FLIP_DIRECTION      false  // Set to true to reverse the clockwise/counterclockwise sense
#define TASK_DELAY_MS       50     // Task delay in milliseconds

// Position Thresholds for LED Colors
#define GREEN_ZONE_MIN      -5
#define GREEN_ZONE_MAX      5
#define YELLOW_ZONE_MIN     -10
#define YELLOW_ZONE_MAX     10

#define GATTS_SERVICE_UUID   0x00FF
#define GATTS_CHAR_UUID      0xFF01
#define GATTS_NUM_HANDLE     4
#define DEVICE_NAME          "BLE_Encoder"

// BLE characteristic value constraints
#define CHAR_VALUE_MAX_LEN   20
#define ADV_DATA_MAX_LEN     31

static bool connection_established = false;
static bool notifications_enabled = false;
static bool ble_service_started = false;

static uint16_t gatt_handle_table[GATTS_NUM_HANDLE];
static esp_gatt_char_prop_t property = 0;

// Fixed: Use proper buffer size and initialization
static uint8_t char_value_buffer[CHAR_VALUE_MAX_LEN] = {0x00};
static esp_attr_value_t gatts_char_val = {
    .attr_max_len = CHAR_VALUE_MAX_LEN,
    .attr_len = 1,
    .attr_value = char_value_buffer,
};

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static uint16_t notify_conn_id = 0;
static esp_gatt_if_t notify_gatts_if = 0;

static const char *CONN_TAG = "BLE_ENCODER";
static const char device_name[] = "BLE_Encoder";

// UUIDs
static uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

// Characteristic Properties
static uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

// CCCD (Client Characteristic Configuration Descriptor) default value
static uint8_t cccd[2] = {0x00, 0x00};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,  // 20ms
    .adv_int_max = 0x20,  // 20ms
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t adv_raw_data[] = {
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    0x0C, ESP_BLE_AD_TYPE_NAME_CMPL, 'B', 'L', 'E', '_', 'E', 'n', 'c', 'o', 'd', 'e', 'r',
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0x09,
};

// LED Color Control
typedef struct {
    bool red;
    bool green;
    bool blue;
} led_color_t;

static const led_color_t LED_GREEN = {false, true, false};
static const led_color_t LED_YELLOW = {true, true, false};
static const led_color_t LED_RED = {true, false, false};

/**
 * @brief Set RGB LED color
 * @param color LED color structure containing RGB values
 */
static void set_led_color(led_color_t color)
{
    gpio_set_level(RED_LED_GPIO, color.red);
    gpio_set_level(GREEN_LED_GPIO, color.green);
    gpio_set_level(BLUE_LED_GPIO, color.blue);
}

/**
 * @brief Get LED color based on encoder position
 * @param position Current encoder position
 * @return LED color structure
 */
static led_color_t get_led_color_for_position(int position)
{
    if (position >= GREEN_ZONE_MIN && position <= GREEN_ZONE_MAX) {
        return LED_GREEN;
    } else if ((position > GREEN_ZONE_MAX && position <= YELLOW_ZONE_MAX) || 
               (position < GREEN_ZONE_MIN && position >= YELLOW_ZONE_MIN)) {
        return LED_YELLOW;
    } else {
        return LED_RED;
    }
}

/**
 * @brief Update LED based on encoder position
 * @param position Current encoder position
 */
static void update_led_for_position(int position)
{
    led_color_t color = get_led_color_for_position(position);
    set_led_color(color);
}

/**
 * @brief Configure GPIO pins for button input
 */
static void configure_button_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

/**
 * @brief Configure GPIO pins for LED outputs
 */
static void configure_led_gpio(void)
{
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << RED_LED_GPIO) | (1ULL << GREEN_LED_GPIO) | (1ULL << BLUE_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&led_conf));
}

/**
 * @brief Initialize rotary encoder
 * @param info Pointer to rotary encoder info structure
 * @return Queue handle for encoder events
 */
static QueueHandle_t initialize_rotary_encoder(rotary_encoder_info_t *info)
{
    // Initialize the rotary encoder device with the GPIOs for A and B signals
    ESP_ERROR_CHECK(rotary_encoder_init(info, ROT_ENC_A_GPIO, ROT_ENC_B_GPIO));
    ESP_ERROR_CHECK(rotary_encoder_enable_half_steps(info, ENABLE_HALF_STEPS));
    
    if (FLIP_DIRECTION) {
        ESP_ERROR_CHECK(rotary_encoder_flip_direction(info));
    }

    // Create a queue for events from the rotary encoder driver
    QueueHandle_t event_queue = rotary_encoder_create_queue();
    ESP_ERROR_CHECK(rotary_encoder_set_queue(info, event_queue));
    
    return event_queue;
}

/**
 * @brief Process rotary encoder event
 * @param event Rotary encoder event structure
 */
static void process_encoder_event(rotary_encoder_event_t event)
{
    ESP_LOGI(TAG, "Event: position %d, direction %s", 
             event.state.position,
             event.state.direction ? (event.state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE ? "CW" : "CCW") : "NOT_SET");
    
    update_led_for_position(event.state.position);
}

/**
 * @brief Send BLE notification with bounds checking
 * @param value Value to send
 * @param len Length of value
 * @return ESP_OK on success
 */
static esp_err_t send_ble_notification(uint8_t *value, size_t len)
{
    // Validate inputs
    if (!value || len == 0 || len > CHAR_VALUE_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid notification parameters: len=%zu, max=%d", len, CHAR_VALUE_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if BLE is ready and notifications are enabled
    if (!notifications_enabled || !connection_established) {
        ESP_LOGW(TAG, "BLE not ready for notifications or notifications disabled");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if handles are valid
    if (notify_gatts_if == 0 || gatt_handle_table[2] == 0) {
        ESP_LOGW(TAG, "BLE handles not ready for notifications");
        return ESP_ERR_INVALID_STATE;
    }
    
    return esp_ble_gatts_send_indicate(notify_gatts_if, notify_conn_id, 
                                     gatt_handle_table[2], len, value, false);
}

typedef enum {
    ZONE_GREEN,
    ZONE_YELLOW,
    ZONE_RED
} encoder_zone_t;

static encoder_zone_t previous_zone = -1;  // invalid initially

static encoder_zone_t get_zone_for_position(int position)
{
    if (position >= GREEN_ZONE_MIN && position <= GREEN_ZONE_MAX) {
        return ZONE_GREEN;
    } else if ((position > GREEN_ZONE_MAX && position <= YELLOW_ZONE_MAX) ||
               (position < GREEN_ZONE_MIN && position >= YELLOW_ZONE_MIN)) {
        return ZONE_YELLOW;
    } else {
        return ZONE_RED;
    }
}


static void poll_encoder_state(rotary_encoder_info_t *info)
{
    rotary_encoder_state_t state = { 0 };
    ESP_ERROR_CHECK(rotary_encoder_get_state(info, &state));

    update_led_for_position(state.position);

    encoder_zone_t current_zone = get_zone_for_position(state.position);

    if (current_zone != previous_zone && ble_service_started) {
        previous_zone = current_zone;

        uint8_t notification_val = 0x00;
        switch (current_zone) {
            case ZONE_GREEN:
                notification_val = 0x02;
                ESP_LOGI(TAG, "Zone changed to GREEN");
                break;
            case ZONE_YELLOW:
                notification_val = 0x03;
                ESP_LOGI(TAG, "Zone changed to YELLOW");
                break;
            case ZONE_RED:
                notification_val = 0x01;
                ESP_LOGI(TAG, "Zone changed to RED");
                break;
        }

        esp_err_t ret = send_ble_notification(&notification_val, sizeof(notification_val));
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to send notification: %s", esp_err_to_name(ret));
        }
    }

    // Reset if position exceeds threshold
    if (RESET_AT && (state.position >= RESET_AT || state.position <= -RESET_AT)) {
        ESP_LOGI(TAG, "Reset due to position limit");
        ESP_ERROR_CHECK(rotary_encoder_reset(info));
    }
}


/**
 * @brief Handle button press/release events
 * @param info Pointer to rotary encoder info structure
 * @param prev_button_pressed Pointer to previous button state
 */
static void handle_button_events(rotary_encoder_info_t *info, bool *prev_button_pressed)
{
    bool button_pressed = (gpio_get_level(BUTTON_GPIO) == 0);  // Active low

    if (button_pressed && !(*prev_button_pressed)) {
        // Button was just pressed
        ESP_LOGI(TAG, "Button Pressed! Setting zero point");
        ESP_ERROR_CHECK(rotary_encoder_reset(info));
    } else if (!button_pressed && (*prev_button_pressed)) {
        // Button was just released
        ESP_LOGI(TAG, "Button Released!");
    }

    *prev_button_pressed = button_pressed;
}

void app_main(void)
{
    esp_err_t ret;

    // Compile-time check for advertising data size
    _Static_assert(sizeof(adv_raw_data) <= ADV_DATA_MAX_LEN, "Advertising data too large");

    //initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s gap register failed, error code = %x", __func__, ret);
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s gatts register failed, error code = %x", __func__, ret);
        return;
    }

    ret = esp_ble_gatts_app_register(APP_ID_PLACEHOLDER);
    if (ret) {
        ESP_LOGE(CONN_TAG, "%s gatts app register failed, error code = %x", __func__, ret);
        return;
    }

    ret = esp_ble_gatt_set_local_mtu(500);
    if (ret) {
        ESP_LOGE(CONN_TAG, "set local  MTU failed, error code = %x", ret);
        return;
    }

    ret = esp_ble_gap_set_device_name(device_name);
    if (ret) {
        ESP_LOGE(CONN_TAG, "set device name failed, error code = %x", ret);
        return;
    }

    ret = esp_ble_gap_config_adv_data_raw(adv_raw_data, sizeof(adv_raw_data));
    if (ret) {
        ESP_LOGE(CONN_TAG, "config adv data failed, error code = %x", ret);
    }
    
    // Install GPIO ISR service (required for rotary encoder)
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    // Configure GPIO pins
    configure_button_gpio();
    configure_led_gpio();

    // Initialize rotary encoder
    rotary_encoder_info_t info = { 0 };
    QueueHandle_t event_queue = initialize_rotary_encoder(&info);
    
    bool prev_button_pressed = false;

    // Main event loop
    while (1) {
        // Check for rotary encoder events
        rotary_encoder_event_t event = { 0 };
        if (xQueueReceive(event_queue, &event, 0) == pdTRUE) {
            process_encoder_event(event);
        } else {
            // No event received, poll current position
            poll_encoder_state(&info);
        }

        // Handle button events
        handle_button_events(&info, &prev_button_pressed);

        // Task delay
        vTaskDelay(TASK_DELAY_MS / portTICK_PERIOD_MS);
    }

    // Cleanup (this code is never reached in the current implementation)
    ESP_LOGE(TAG, "Unexpected exit from main loop");
    ESP_ERROR_CHECK(rotary_encoder_uninit(&info));
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(CONN_TAG, "Advertising data set, status %d", param->adv_data_raw_cmpl.status);
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(CONN_TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
            break;
        }
        ESP_LOGI(CONN_TAG, "Advertising start successfully");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(CONN_TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
        }
        ESP_LOGI(CONN_TAG, "Advertising stop successfully");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(CONN_TAG, "Connection params update, status %d, conn_int %d, latency %d, timeout %d",
                    param->update_conn_params.status,
                    param->update_conn_params.conn_int,
                    param->update_conn_params.latency,
                    param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    static uint16_t gatt_service_uuid = GATTS_SERVICE_UUID;
    static uint16_t gatt_char_uuid    = GATTS_CHAR_UUID;

    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(CONN_TAG, "GATT server register, status %d, app_id %d", param->reg.status, param->reg.app_id);
        
        // Create attribute table
        esp_gatts_attr_db_t gatt_db[GATTS_NUM_HANDLE] = {
            // Service Declaration
            [0] = {
                {ESP_GATT_AUTO_RSP},
                {ESP_UUID_LEN_16, (uint8_t*)&primary_service_uuid, ESP_GATT_PERM_READ,
                sizeof(uint16_t), sizeof(gatt_service_uuid), (uint8_t*)&gatt_service_uuid}
            },
            // Characteristic Declaration
            [1] = {
                {ESP_GATT_AUTO_RSP},
                {ESP_UUID_LEN_16, (uint8_t*)&character_declaration_uuid, ESP_GATT_PERM_READ,
                sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&char_prop_read_notify}
            },
            // Characteristic Value - FIXED
            [2] = {
                {ESP_GATT_RSP_BY_APP},
                {ESP_UUID_LEN_16, (uint8_t*)&gatt_char_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                CHAR_VALUE_MAX_LEN, sizeof(char_value_buffer), char_value_buffer}
            },
            // Client Characteristic Configuration Descriptor (CCCD)
            [3] = {
                {ESP_GATT_AUTO_RSP},
                {ESP_UUID_LEN_16, (uint8_t*)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                sizeof(uint16_t), sizeof(cccd), (uint8_t*)cccd}
            }
        };
        
        // Create the attribute table
        esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, GATTS_NUM_HANDLE, 0);
        if (create_attr_ret) {
            ESP_LOGE(CONN_TAG, "create attr table failed, error code = %x", create_attr_ret);
        }
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(CONN_TAG, "create attribute table failed, error code=0x%x", param->add_attr_tab.status);
            break;
        }
        
        if (param->add_attr_tab.num_handle != GATTS_NUM_HANDLE) {
            ESP_LOGE(CONN_TAG, "create attribute table abnormally, num_handle (%d) doesn't equal to GATTS_NUM_HANDLE(%d)", 
                    param->add_attr_tab.num_handle, GATTS_NUM_HANDLE);
            break;
        }
        
        ESP_LOGI(CONN_TAG, "create attribute table successfully, the number handle = %d", param->add_attr_tab.num_handle);
        memcpy(gatt_handle_table, param->add_attr_tab.handles, sizeof(gatt_handle_table));
        
        // Start the service
        esp_ble_gatts_start_service(gatt_handle_table[0]);
        break;

    case ESP_GATTS_READ_EVT:
        ESP_LOGI(CONN_TAG, "GATT read request, handle = %d", param->read.handle);
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = 1;
        rsp.attr_value.value[0] = 0x00;  // Default value
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(CONN_TAG, "Service start successfully, status %d, service_handle %d", 
                param->start.status, param->start.service_handle);
        ble_service_started = true;  // ADD THIS LINE
        break;
        
    case ESP_GATTS_CONNECT_EVT:
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20;
        conn_params.min_int = 0x10;
        conn_params.timeout = 400;
        ESP_LOGI(CONN_TAG, "Connected, conn_id %u, remote "ESP_BD_ADDR_STR"",
                param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        esp_ble_gap_update_conn_params(&conn_params);
        notify_conn_id = param->connect.conn_id;
        notify_gatts_if = gatts_if;
        connection_established = true;
        break;
        
    case ESP_GATTS_WRITE_EVT:
        ESP_LOGI(CONN_TAG, "GATT write request, handle = %d, value len = %d", 
                param->write.handle, param->write.len);
        
        // Add bounds checking for write operations
        if (param->write.len > CHAR_VALUE_MAX_LEN) {
            ESP_LOGE(CONN_TAG, "Write length %d exceeds maximum %d", param->write.len, CHAR_VALUE_MAX_LEN);
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_INVALID_ATTR_LEN, NULL);
            }
            break;
        }
        
        // Check if this is a CCCD write (handle 3 is our CCCD)
        if (param->write.handle == gatt_handle_table[3] && param->write.len == 2) {
            uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
            if (descr_value == 0x0001) {
                ESP_LOGI(CONN_TAG, "Notifications enabled");
                notifications_enabled = true;
            } else if (descr_value == 0x0000) {
                ESP_LOGI(CONN_TAG, "Notifications disabled");
                notifications_enabled = false;
            }
        }
    
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;
        
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(CONN_TAG, "Disconnected, remote "ESP_BD_ADDR_STR", reason 0x%02x",
                ESP_BD_ADDR_HEX(param->disconnect.remote_bda), param->disconnect.reason);
        connection_established = false;
        notifications_enabled = false;
        notify_conn_id = 0;
        notify_gatts_if = 0;
        esp_ble_gap_start_advertising(&adv_params);
        break;
        
    default:
        break;
    }
}
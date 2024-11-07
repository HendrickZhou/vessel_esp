/*
 *  
 *
 *
 *
 *  main deal with two major tasks, or channels: BLE & BT
 *
 */
/*
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_system.h"
#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_bt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h" */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_gatt_common_api.h"

#include "mic_input.c"

/* log tag */
#define BT_BLE_COEX_TAG             "BT_BLE_COEX"
#define GATTS_TAG                   "BT_GATTS"

/* device info */
#define DEVICE_NAME                 "VESSEL"
#define BLE_ADV_NAME                "COEX_VESSEL"

/* service related */
#define SERVICE_UUID_SPEECH             0x00FF
#define CHAR_UUID_SPEECH                0xFF01

/* app id */
#define APP_SPEECH_ID                   1



// #define DUMMY_DATA

/* protocol parameter */
uint8_t NOTIFY_INTERVAL_MS = 10; 
uint16_t MTU_SIZE = 512;

// todo: make a array
static uint16_t speech_gatt_if = 0;
static uint16_t cur_conn_id = 0;
static bool is_connected = false;
static uint16_t char_handle = 0;
static esp_gatt_srvc_id_t speech_service_id;
static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x060,
    .adv_int_max        = 0x060,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_RPA_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/*
 *
 * declaration
 */
static void gap_cb_entry(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
static void gatts_cb_entry(esp_gatts_cb_event_t event, esp_gatt_if_t gif, esp_ble_gatts_cb_param_t * param);
static void gatts_speech_app_entry(esp_gatts_cb_event_t event, esp_gatt_if_t gif , esp_ble_gatts_cb_param_t * param);
static void ble_channel_start();
static void advertise(const char * name);
static void audioGateway(); 
 
 

/*
 * entry for handling all the event related to gap
 */
static void gap_cb_entry(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t * param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: // triggered after `esp_ble_gap_config_adv_data_raw, which get the adv data ready in the stack to send
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        // esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        //advertising start complete event to indicate advertising start successfully or failed
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(BT_BLE_COEX_TAG, "Advertising start failed");
        }else {
            ESP_LOGI(BT_BLE_COEX_TAG, "Start adv successfully");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(BT_BLE_COEX_TAG, "Advertising stop failed");
        }
        else {
            ESP_LOGI(BT_BLE_COEX_TAG, "Stop adv successfully");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(BT_BLE_COEX_TAG, "update connection params status = %d, conn_int = %d, latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

/*
 * entry for handling all the event related to gatts
 */
static void gatts_cb_entry(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t * param) {
    // if it's register action
     if (event == ESP_GATTS_REG_EVT) {
        if(param->reg.status == ESP_GATT_OK) {
            speech_gatt_if = gatts_if;
        } else {
            ESP_LOGI(BT_BLE_COEX_TAG, "Reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
     }

     // deal with speech app rn
     // it's a server-side thing, need to start the service
     do {
        if(gatts_if == speech_gatt_if){
            gatts_speech_app_entry(event, gatts_if, param);
        }
     } while(0); // one-off, this paradiam make the code a block structure
     
}

static void gatts_speech_app_entry(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT: {
            ESP_LOGI(BT_BLE_COEX_TAG, "REGISTER_APP_EVT, status %d, app_id %d", param->reg.status, param->reg.app_id);
            esp_ble_gap_config_local_privacy(true);
            // and create service
            speech_service_id.is_primary = true;
            speech_service_id.id.inst_id = 0x00;
            speech_service_id.id.uuid.len = ESP_UUID_LEN_16;
            speech_service_id.id.uuid.uuid.uuid16 = SERVICE_UUID_SPEECH;
            esp_ble_gatts_create_service(gatts_if, &speech_service_id, 1);
            break;
        }
        case ESP_GATTS_CREATE_EVT: {
            // in ESP_GATTS_REG_EVT it only start the creation process, but here it finishs the creation, so we can actually start the service

            // Add speech characteristic
            esp_bt_uuid_t char_uuid;
            char_uuid.len = ESP_UUID_LEN_16;
            char_uuid.uuid.uuid16 = CHAR_UUID_SPEECH;
            esp_ble_gatts_add_char(param->create.service_handle, &char_uuid,
                                    ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                    ESP_GATT_CHAR_PROP_BIT_NOTIFY, NULL, NULL);
            esp_ble_gatts_start_service(param->create.service_handle);
            break;
        }
        case ESP_GATTS_ADD_CHAR_EVT:{
            ESP_LOGI(GATTS_TAG, "ADD_CHAR_EVT, attr_handle %d, service_handle %d",
                 param->add_char.attr_handle, param->add_char.service_handle);
            char_handle = param->add_char.attr_handle;
            break;
        }   
        case ESP_GATTS_READ_EVT: break; // react to "read request" from client
        case ESP_GATTS_WRITE_EVT: break; // react to "write request" from client
        case ESP_GATTS_EXEC_WRITE_EVT: break; // react to "prepare write request"
        case ESP_GATTS_MTU_EVT: break;
        case ESP_GATTS_CONNECT_EVT: {
            ESP_LOGI(GATTS_TAG, "ESP_GATTS_CONNECT_EVT, conn_id %d", param->connect.conn_id);
            cur_conn_id = param->connect.conn_id;
            is_connected = true;
            xSemaphoreGive(condition_connected);
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT: {
            ESP_LOGI(GATTS_TAG, "ESP_GATTS_DISCONNECT_EVT, reason 0x%x", param->disconnect.reason);
            is_connected = false;
            esp_ble_gap_start_advertising(&adv_params);// Restart advertising
            break;
        }
        default:
            break;
    }
}


/*
 * This is the root of dispatching tasks
 * start the channel of ble service, by registering all events to callbacks
 * Events include:
 *  gap related: device discovery, match and connect
 *  gatt related: streaming data
 *  
 *
 * support 2 application: 1 for audio, 1 for map data
 */
static void ble_channel_start(void) {
    esp_err_t ret;
    ret = esp_ble_gap_register_callback(gap_cb_entry);
    if(ret) {
        ESP_LOGE(BT_BLE_COEX_TAG, "gatts register error, error code = 0x%x", ret);
        return;
    }
    
    // this is the entry to all event that could happen to gatts
    // but not include sending the data since event means passively react to something
    // idf will have event-handling loop underneath
    ret = esp_ble_gatts_register_callback(gatts_cb_entry);
    if(ret){
        ESP_LOGE(BT_BLE_COEX_TAG, "gap register error, error code = 0x%x", ret);
        return ;
    }
    
    // register app with a GATT interface(gatts_if) identifier
    // so the callback will know how to route the event to which app
    // this call will trigger ESP_GATTS_REG_EVT
    ret = esp_ble_gatts_app_register(APP_SPEECH_ID);
    if(ret){
        ESP_LOGE(BT_BLE_COEX_TAG, "gatts app register error, error code = 0x%x", ret);
        return;
    }
    /*
     * more application
     */
    // if(ret = esp_ble_gatts_app_register()) {
    // }

    // advertise it self
    advertise(BLE_ADV_NAME);
    // advertise complete and connection establishment will be handled by event handler entry

    // start the speech transmission worker thread
    xTaskCreate(audioGateway, "audio_gate_way", 2048+512, NULL, 5, NULL);
}

static void advertise(const char *name){
    // start to advertise ourself
    // gap profile config adv data, and the response to scan, and trigger two gap events, only one event need to deal with the advertising
    int len = strlen(name);
    uint8_t raw_adv_data[len+5];
    //flag
    raw_adv_data[0] = 2;
    raw_adv_data[1] = ESP_BT_EIR_TYPE_FLAGS;
    raw_adv_data[2] = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    //adv name
    raw_adv_data[3] = len + 1;
    raw_adv_data[4] = ESP_BLE_AD_TYPE_NAME_CMPL;
    for (int i = 0;i < len;i++)
    {
        raw_adv_data[i+5] = *(name++);
    }
    //The length of adv data must be less than 31 bytes
    esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
    if (raw_adv_ret){
        ESP_LOGE(BT_BLE_COEX_TAG, "config raw adv data failed, error code = 0x%x ", raw_adv_ret);
    }
    esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_adv_data, sizeof(raw_adv_data));
    if (raw_scan_ret){
        ESP_LOGE(BT_BLE_COEX_TAG, "config raw scan rsp data failed, error code = 0x%x", raw_scan_ret);
    }
}

static void audioGateway() {
#ifdef DUMMY_DATA
    uint8_t notify_data[4] = {0x00, 0x00, 0x00, 0x00};
    while(1) {
        if(is_connected) {
            esp_ble_gatts_send_indicate(speech_gatt_if, cur_conn_id, char_handle, sizeof(notify_data), notify_data, false); // without ack
            ESP_LOGI(GATTS_TAG, "Sent notification: 0000");
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_INTERVAL_MS));
    }
#else 

#ifdef STACK_MONITOR
    UBaseType_t stack_1 = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK", "1Stack left now: %u words", stack_1);
#endif

    size_t item_size;
    while(1) {
        uint8_t *item = get_next_packet(&item_size);
        // if get null, nothing need to be done
        // if not null, connected or not, free item

#ifdef STACK_MONITOR
        UBaseType_t stack_2 = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI("STACK", "2Stack left now: %u words", stack_2);
#endif

        if(item != NULL) {
            if(is_connected) {
                ESP_LOGI("QUEUE", "get item from queue2, sending it to connected device");
                    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI("HEAP", "Free heap size: %u bytes", free_heap);
    //size_t free_long = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    //ESP_LOGI("HEAP", "largest free heap size: %u bytes", free_long);
    ESP_LOGI("HEAP", "need space: %d", item_size);
                esp_err_t ret = esp_ble_gatts_send_indicate(speech_gatt_if, cur_conn_id, char_handle, item_size, item, false); // without ack 
                if(ret != ESP_OK) {
                    ESP_LOGE(GATTS_TAG, "Failed to send indication, error code: %d", ret);
                }
                free(item);
                // interval of transimission
                vTaskDelay(pdMS_TO_TICKS(NOTIFY_INTERVAL_MS));
            } else {
                free(item);
            }
        }
#ifdef STACK_MONITOR
        UBaseType_t stack_3 = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI("STACK", "3Stack left now: %u words", stack_3);
#endif
    }
#endif 
}

void app_main(void) {
    // init nvs
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);


    ESP_LOGE(BT_BLE_COEX_TAG, "nvs success");
    /* **************************
     * init BLE GAP & GATT service
     * with bluedorid stack
     * **************************/
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(BT_BLE_COEX_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bt_controller_enable(ESP_BT_MODE_BTDM)) != ESP_OK) {
        ESP_LOGE(BT_BLE_COEX_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_A2DP_SINK_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((err = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_BLE_COEX_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(BT_BLE_COEX_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    // init Classic BT service
    //
    //


    // start media streaimg thread
    //

    condition_connected = xSemaphoreCreateBinary();
    // start the data stream
    setup_mic_input();
    // order very important, ohterwise buffer will not be ready for use

    // start BLE 
    ble_channel_start();

    }

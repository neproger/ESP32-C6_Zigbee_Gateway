/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * Zigbee Gateway Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include <fcntl.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_coexist.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_zigbee_gateway.h"
#include "esp_zigbee_cluster.h"
#include "zb_config_platform.h"

#include "gw_wifi.h"
#include "gw_zigbee/gw_zigbee.h"
#include "gw_core/event_bus.h"
#include "gw_core/device_registry.h"
#include "gw_core/automation_store.h"
#include "gw_core/sensor_store.h"
#include "gw_core/state_store.h"
#include "gw_core/zb_model.h"
#include "gw_http/gw_http.h"

#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"

static const char *TAG = "ESP_ZB_GATEWAY";

static esp_err_t zb_core_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id == ESP_ZB_CORE_REPORT_ATTR_CB_ID) {
        const esp_zb_zcl_report_attr_message_t *m = (const esp_zb_zcl_report_attr_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        uint16_t src_short = 0;
        if (m->src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
            src_short = m->src_address.u.short_addr;
        }

        gw_device_uid_t uid = {0};
        if (!gw_zb_model_find_uid_by_short(src_short, &uid) && src_short != 0) {
            (void)gw_zigbee_discover_by_short(src_short);
        }

        // Persist interesting sensor values for UI/debugging.
        const uint16_t cluster_id = m->cluster;
        const uint16_t attr_id = m->attribute.id;
        if (uid.uid[0] != '\0' && m->attribute.data.value != NULL) {
            gw_sensor_value_t v = {0};
            v.uid = uid;
            v.short_addr = src_short;
            v.endpoint = m->src_endpoint;
            v.cluster_id = cluster_id;
            v.attr_id = attr_id;
            v.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);

            if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
                m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 && m->attribute.data.size >= 2) {
                v.value_type = GW_SENSOR_VALUE_I32;
                v.value_i32 = *((const int16_t *)m->attribute.data.value);
                (void)gw_sensor_store_upsert(&v);

                // Normalized state key for automations.
                (void)gw_state_store_set_f32(&uid, "temperature_c", ((float)v.value_i32) / 100.0f, v.ts_ms);
            } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && attr_id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID &&
                       m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && m->attribute.data.size >= 2) {
                v.value_type = GW_SENSOR_VALUE_U32;
                v.value_u32 = *((const uint16_t *)m->attribute.data.value);
                (void)gw_sensor_store_upsert(&v);

                (void)gw_state_store_set_f32(&uid, "humidity_pct", ((float)v.value_u32) / 100.0f, v.ts_ms);
            } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG && attr_id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID &&
                       m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && m->attribute.data.size >= 1) {
                v.value_type = GW_SENSOR_VALUE_U32;
                v.value_u32 = *((const uint8_t *)m->attribute.data.value);
                (void)gw_sensor_store_upsert(&v);

                // Battery percentage is 0.5% units. Normalize to integer percent.
                (void)gw_state_store_set_u32(&uid, "battery_pct", (uint32_t)(v.value_u32 / 2u), v.ts_ms);
            } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
                       (m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL || m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8) &&
                       m->attribute.data.size >= 1) {
                uint8_t onoff = *((const uint8_t *)m->attribute.data.value);
                (void)gw_state_store_set_bool(&uid, "onoff", onoff != 0, v.ts_ms);
            }

            // Keep last seen fresh on any attribute report.
            (void)gw_state_store_set_u64(&uid, "last_seen_ms", v.ts_ms, v.ts_ms);
        }

        // Normalized event: zigbee.attr_report (msg + structured payload)
        {
            char msg[96];
            (void)snprintf(msg,
                           sizeof(msg),
                           "report cluster=0x%04x attr=0x%04x ep=%u type=0x%02x size=%u",
                           (unsigned)cluster_id,
                           (unsigned)attr_id,
                           (unsigned)m->src_endpoint,
                           (unsigned)m->attribute.data.type,
                           (unsigned)m->attribute.data.size);

            char payload[160];
            if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
                m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 && m->attribute.data.size >= 2 && m->attribute.data.value != NULL) {
                const int16_t v = *((const int16_t *)m->attribute.data.value);
                (void)snprintf(payload,
                               sizeof(payload),
                               "{\"endpoint\":%u,\"cluster\":\"0x%04x\",\"attr\":\"0x%04x\",\"value\":%d,\"unit\":\"cC\"}",
                               (unsigned)m->src_endpoint,
                               (unsigned)cluster_id,
                               (unsigned)attr_id,
                               (int)v);
            } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && attr_id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID &&
                       m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && m->attribute.data.size >= 2 && m->attribute.data.value != NULL) {
                const uint16_t v = *((const uint16_t *)m->attribute.data.value);
                (void)snprintf(payload,
                               sizeof(payload),
                               "{\"endpoint\":%u,\"cluster\":\"0x%04x\",\"attr\":\"0x%04x\",\"value\":%u,\"unit\":\"cP\"}",
                               (unsigned)m->src_endpoint,
                               (unsigned)cluster_id,
                               (unsigned)attr_id,
                               (unsigned)v);
            } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                       attr_id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID &&
                       m->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && m->attribute.data.size >= 1 && m->attribute.data.value != NULL) {
                const uint8_t v = *((const uint8_t *)m->attribute.data.value);
                (void)snprintf(payload,
                               sizeof(payload),
                               "{\"endpoint\":%u,\"cluster\":\"0x%04x\",\"attr\":\"0x%04x\",\"value\":%u,\"unit\":\"half_pct\"}",
                               (unsigned)m->src_endpoint,
                               (unsigned)cluster_id,
                               (unsigned)attr_id,
                               (unsigned)v);
            } else {
                (void)snprintf(payload,
                               sizeof(payload),
                               "{\"endpoint\":%u,\"cluster\":\"0x%04x\",\"attr\":\"0x%04x\",\"zcl_type\":\"0x%02x\",\"size\":%u}",
                               (unsigned)m->src_endpoint,
                               (unsigned)cluster_id,
                               (unsigned)attr_id,
                               (unsigned)m->attribute.data.type,
                               (unsigned)m->attribute.data.size);
            }
            gw_event_bus_publish_ex("zigbee.attr_report", "zigbee", uid.uid, src_short, msg, payload);
        }

        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *m = (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        uint16_t src_short = 0;
        if (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
            src_short = m->info.src_address.u.short_addr;
        }

        gw_device_uid_t uid = {0};
        if (!gw_zb_model_find_uid_by_short(src_short, &uid) && src_short != 0) {
            (void)gw_zigbee_discover_by_short(src_short);
        }

        for (esp_zb_zcl_read_attr_resp_variable_t *it = m->variables; it != NULL; it = it->next) {
            const uint16_t cluster_id = m->info.cluster;
            const uint16_t attr_id = it->attribute.id;

            if (uid.uid[0] != '\0' && it->status == ESP_ZB_ZCL_STATUS_SUCCESS && it->attribute.data.value != NULL) {
                gw_sensor_value_t v = {0};
                v.uid = uid;
                v.short_addr = src_short;
                v.endpoint = m->info.src_endpoint;
                v.cluster_id = cluster_id;
                v.attr_id = attr_id;
                v.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);

                if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
                    it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16 && it->attribute.data.size >= 2) {
                    v.value_type = GW_SENSOR_VALUE_I32;
                    v.value_i32 = *((const int16_t *)it->attribute.data.value);
                    (void)gw_sensor_store_upsert(&v);
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT &&
                           attr_id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID && it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
                           it->attribute.data.size >= 2) {
                    v.value_type = GW_SENSOR_VALUE_U32;
                    v.value_u32 = *((const uint16_t *)it->attribute.data.value);
                    (void)gw_sensor_store_upsert(&v);
                } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                           attr_id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID && it->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 &&
                           it->attribute.data.size >= 1) {
                    v.value_type = GW_SENSOR_VALUE_U32;
                    v.value_u32 = *((const uint8_t *)it->attribute.data.value);
                    (void)gw_sensor_store_upsert(&v);
                }
            }
        }

        gw_event_bus_publish("zigbee_read_attr_resp", "zigbee", uid.uid, src_short, "read attr response received");
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        const esp_zb_zcl_set_attr_value_message_t *m = (const esp_zb_zcl_set_attr_value_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            uint8_t onoff = 0;
            if (m->attribute.data.value != NULL && m->attribute.data.size >= 1) {
                onoff = *((const uint8_t *)m->attribute.data.value);
            }

            char msg[80];
            (void)snprintf(msg, sizeof(msg), "onoff=%u dst_ep=%u", (unsigned)onoff, (unsigned)m->info.dst_endpoint);
            gw_event_bus_publish("zigbee_onoff_attr", "zigbee", "", 0, msg);
        }

        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID) {
        const esp_zb_zcl_privilege_command_message_t *m = (const esp_zb_zcl_privilege_command_message_t *)message;
        if (m == NULL) {
            return ESP_OK;
        }

        if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && m->info.command.id == ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID) {
            uint16_t src_short = 0;
            if (m->info.src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
                src_short = m->info.src_address.u.short_addr;
            }

            gw_device_uid_t uid = {0};
            if (!gw_zb_model_find_uid_by_short(src_short, &uid) && src_short != 0) {
                (void)gw_zigbee_discover_by_short(src_short);
            }

            char msg[128];
            (void)snprintf(msg,
                           sizeof(msg),
                           "on_off toggle (src=0x%04x ep=%u rssi=%d)",
                           (unsigned)src_short,
                           (unsigned)m->info.src_endpoint,
                           (int)m->info.header.rssi);

            {
                char payload[160];
                (void)snprintf(payload,
                               sizeof(payload),
                               "{\"cmd\":\"toggle\",\"endpoint\":%u,\"cluster\":\"0x%04x\",\"rssi\":%d}",
                               (unsigned)m->info.src_endpoint,
                               (unsigned)ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                               (int)m->info.header.rssi);
                gw_event_bus_publish_ex("zigbee.command", "zigbee", uid.uid, src_short, msg, payload);
            }
        }
    }

    return ESP_OK;
}

/* Note: Please select the correct console output port based on the development board in menuconfig */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
esp_err_t esp_zb_gateway_console_init(void)
{
    esp_err_t ret = ESP_OK;
    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);

    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ret = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    usb_serial_jtag_vfs_use_driver();
    uart_vfs_dev_register();
    return ret;
}
#endif

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee bdb commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    esp_zb_zdo_signal_device_annce_params_t *dev_annce_params = NULL;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network formation");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                esp_zb_bdb_open_network(180);
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ieee_address;
            esp_zb_get_long_address(ieee_address);
            ESP_LOGI(TAG, "Formed network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4],
                     ieee_address[3], ieee_address[2], ieee_address[1], ieee_address[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGI(TAG, "Restart network formation (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Network steering started");
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", dev_annce_params->device_short_addr);
        gw_zigbee_on_device_annce(dev_annce_params->ieee_addr, dev_annce_params->device_short_addr, dev_annce_params->capability);
        break;
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            if (*(uint8_t *)esp_zb_app_signal_get_params(p_sg_p)) {
                ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", esp_zb_get_pan_id(), *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p));
            } else {
                ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", esp_zb_get_pan_id());
            }
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
        ESP_LOGI(TAG, "Production configuration is %s", err_status == ESP_OK ? "ready" : "not present");
        esp_zb_set_node_descriptor_manufacturer_code(ESP_MANUFACTURER_CODE);
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = ESP_ZB_GATEWAY_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_REMOTE_CONTROL_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_attribute_list_t *basic_cluser = esp_zb_basic_cluster_create(NULL);
    esp_zb_basic_cluster_add_attr(basic_cluser, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_cluser, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, ESP_MODEL_IDENTIFIER);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluser, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Groups server enables receiving group-addressed commands (and helps with interoperability).
    esp_zb_groups_cluster_cfg_t groups_cfg = {.groups_name_support_id = 0};
    esp_zb_cluster_list_add_groups_cluster(cluster_list, esp_zb_groups_cluster_create(&groups_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Expose an On/Off server so HA switches can bind to the gateway and we can observe toggle commands.
    esp_zb_on_off_cluster_cfg_t on_off_cfg = {.on_off = false};
    esp_zb_cluster_list_add_on_off_cluster(cluster_list, esp_zb_on_off_cluster_create(&on_off_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_ep_list_add_gateway_ep(ep_list, cluster_list, endpoint_config);
    esp_zb_device_register(ep_list);

    // Allow the application to observe On/Off Toggle as a "privilege command" callback.
    esp_zb_core_action_handler_register(zb_core_action_handler);
    (void)esp_zb_zcl_add_privilege_command(ESP_ZB_GATEWAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID);
    gw_event_bus_publish("zigbee_ready", "zigbee", "", 0, "registered privilege handler for on_off toggle");

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gw_event_bus_init());
    ESP_ERROR_CHECK(gw_zb_model_init());
    ESP_ERROR_CHECK(gw_sensor_store_init());
    ESP_ERROR_CHECK(gw_state_store_init());
    gw_event_bus_publish("boot", "system", "", 0, "app_main started");

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_wifi_i154_enable();
#endif /* CONFIG_ESP_COEX_SW_COEXIST_ENABLE */

    esp_err_t wifi_err = gw_wifi_connect_multi();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi not connected (%s)", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    }

    ESP_ERROR_CHECK(gw_device_registry_init());
    ESP_ERROR_CHECK(gw_automation_store_init());
    ESP_ERROR_CHECK(gw_http_start());
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    ESP_ERROR_CHECK(esp_zb_gateway_console_init());
#endif
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}

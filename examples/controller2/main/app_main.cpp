/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_ota.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_cluster.h>
#include <esp_matter_attribute.h>
#include <common_macros.h>

#include <app/server/Server.h>
#include <credentials/FabricTable.h>

#include <esp_matter_bridge.h>
#include "app_priv.h"
#include <app_reset.h>

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace esp_matter::cluster;
using namespace chip::app::Clusters;

static const char *TAG = "app_main";
uint16_t aggregator_endpoint_id = 0;
esp_matter_bridge::device_t *virtual_light = nullptr;
uint16_t light_endpoint_id = 0;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
            event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
        }
        break;
    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == attribute::PRE_UPDATE) {
        /* Driver update */
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

// Bridge device type callback
static esp_err_t bridge_device_type_cb(esp_matter::endpoint_t *ep, uint32_t device_type_id, void *priv_data)
{
    esp_err_t err = ESP_OK;0

    switch (device_type_id) {
        case ESP_MATTER_ON_OFF_LIGHT_DEVICE_TYPE_ID: {
            on_off_light::config_t on_off_light_conf;
            err = on_off_light::add(ep, &on_off_light_conf);
            break;
        }
    }

    return err;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#if CONFIG_ESP_MATTER_CONTROLLER_ENABLE
    esp_matter::console::controller_register_commands();
#endif // CONFIG_ESP_MATTER_CONTROLLER_ENABLE
#endif // CONFIG_ENABLE_CHIP_SHELL

    /* Initialize driver */
    app_driver_handle_t light_handle = app_driver_light_init();
    app_driver_handle_t button_handle = app_driver_button_init();
    app_reset_button_register(button_handle);

    // Create Matter node with aggregator and bridged light
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, NULL);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    // // Create aggregator with bridged light
    // err = create_aggregator_and_bridged_light(node);
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to create aggregator and bridged light");
    //     return;
    // }

    endpoint_t *aggregator_endpoint = aggregator::create(node, NULL, ENDPOINT_FLAG_NONE, NULL);

    if (!aggregator_endpoint) {
        ESP_LOGE(TAG, "Failed to create aggregator endpoint");
        return;
    }

    aggregator_endpoint_id = endpoint::get_id(aggregator_endpoint);

     // Initialize bridge
    esp_matter_bridge::initialize(node, bridge_device_type_cb);
    
    // Create virtual light
    virtual_light = esp_matter_bridge::create_device(
        node,
        aggregator_endpoint_id,
        ESP_MATTER_ON_OFF_LIGHT_DEVICE_TYPE_ID,
        nullptr
    );
    
    if (virtual_light) {
        endpoint::enable(virtual_light->endpoint);
        ESP_LOGI(TAG, "Virtual light created on endpoint %d", 
                 endpoint::get_id(virtual_light->endpoint));
    }

    light_endpoint_id = endpoint::get_id(virtual_light->endpoint);

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

#if CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
    esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
    esp_matter::lock::chip_stack_unlock();
#endif // CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
}

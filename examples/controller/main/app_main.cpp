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
#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include <esp_openthread_border_router.h>
#include <esp_openthread_lock.h>
#include <esp_ot_config.h>
#include <esp_spiffs.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER
#include <common_macros.h>

#include <app/server/Server.h>
#include <credentials/FabricTable.h>

#include "globals.h"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

bool commissioner_enabled = false;

static const char *TAG = "app_main";
uint16_t switch_endpoint_id = 0;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
            event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
            static bool sThreadBRInitialized = false;
            if (!sThreadBRInitialized) {
                esp_openthread_set_backbone_netif(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
                esp_openthread_lock_acquire(portMAX_DELAY);
                esp_openthread_border_router_init();
                esp_openthread_lock_release();
                sThreadBRInitialized = true;
            }
#endif
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        // MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        // MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        {
            ESP_LOGI(TAG, "Fabric removed successfully");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
            {
                // chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
                // constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
                // if (!commissionMgr.IsCommissioningWindowOpen())
                // {
                //     /* After removing last fabric, this example does not remove the Wi-Fi credentials
                //      * and still has IP connectivity so, only advertising on DNS-SD.
                //      */
                //     CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                //                                     chip::CommissioningWindowAdvertisement::kDnssdOnly);
                //     if (err != CHIP_NO_ERROR)
                //     {
                //         ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                //     }
                // }
            }
        break;
        }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        // MEMORY_PROFILER_DUMP_HEAP_STAT("BLE deinitialized");
        break;       
    default:
        break;
    }
}

#include <esp_console.h>
#include <app/util/attribute-storage.h>

static int commissioner_setup_command(int argc, char **argv)
{
    commissioner_enabled = true;
    ESP_LOGI(TAG, "Setting up commissioner...");
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
    #if CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
    esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
    #endif
    esp_matter::lock::chip_stack_unlock();
    ESP_LOGI(TAG, "Commissioner setup complete");
    return 0;
}

static void commissioner_disable()
{
    commissioner_enabled = false;
}

static void commissioner_enable()
{
    commissioner_enabled = true;
}

static int commissioner_enable_command(int argc, char **argv)
{
    if (argc < 2) {
        ESP_LOGI(TAG, "Usage: commissioner_enable <0|1>");
        return 1;
    }
    int enable = atoi(argv[1]);
    commissioner_enabled = (enable != 0);

    if (commissioner_enabled)
    {
        commissioner_enable();
    }
    else 
    {
        commissioner_disable();
    }

    return 0;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    esp_log_level_set("chip*", ESP_LOG_DEBUG);
    esp_log_level_set("DMG", ESP_LOG_DEBUG);
    esp_log_level_set("esp_matter_endpoint", ESP_LOG_DEBUG);
    esp_log_level_set("esp_matter", ESP_LOG_DEBUG);

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
#ifdef CONFIG_OPENTHREAD_BORDER_ROUTER
    esp_matter::console::otcli_register_commands();
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER
#endif // CONFIG_ENABLE_CHIP_SHELL
#ifdef CONFIG_OPENTHREAD_BORDER_ROUTER
#ifdef CONFIG_AUTO_UPDATE_RCP
    esp_vfs_spiffs_conf_t rcp_fw_conf = {
        .base_path = "/rcp_fw", .partition_label = "rcp_fw", .max_files = 10, .format_if_mount_failed = false};
    if (ESP_OK != esp_vfs_spiffs_register(&rcp_fw_conf)) {
        ESP_LOGE(TAG, "Failed to mount rcp firmware storage");
        return;
    }
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();
    openthread_init_br_rcp(&rcp_update_config);
#endif
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER

    // if (commissioner_enabled)
    // {
        esp_console_cmd_t commissioner_cmd = {
            .command = "commissioner_setup",
            .help = "Setup the Matter commissioner",
            .hint = NULL,
            .func = &commissioner_setup_command,
            .argtable = NULL
        };
        esp_console_cmd_register(&commissioner_cmd);
    // }

    esp_console_cmd_t commissioner_cmd2 = {
        .command = "commissioner_enable",
        .help = "Enable the Matter commissioner",
        .hint = NULL,
        .func = &commissioner_enable_command,
        .argtable = NULL
    };
    esp_console_cmd_register(&commissioner_cmd2);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;

    ESP_LOGI(TAG, "Creating Matter node and endpoint");
    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, NULL, NULL);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    extended_color_light::config_t light_config;

    // endpoint handles can be used to add/modify clusters.
    endpoint_t *endpoint = extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create extended color light endpoint"));

    endpoint::get_id(endpoint);
    // ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

// #if CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
//     esp_matter::lock::chip_stack_lock(portMAX_DELAY);
//     esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
//     esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
//     esp_matter::lock::chip_stack_unlock();
// #endif // CONFIG_ESP_MATTER_COMMISSIONER_ENABLE

    if (node) {
        endpoint_t *root = endpoint::get(node, 0);
        if (root) {
            cluster_t *c = cluster::get_first(root);
            while (c) {
                ESP_LOGI(TAG, "Root endpoint cluster: 0x%04X", cluster::get_id(c));
                c = cluster::get_next(c);
            }
        }
    }
}

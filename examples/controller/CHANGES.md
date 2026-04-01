# Controller Example — Custom Changes

This document describes the modifications made to the stock `controller` example to support **switch → light binding** with a physical LED output on an ESP32-S3.

---

## Overview

The controller runs simultaneously as:
- A **Matter commissionee** — hosts a light endpoint via `chip::Server` on port 5540
- A **Matter commissioner** — can commission and control other Matter devices

The goal is to commission an external light switch onto the controller's fabric, write a Binding cluster entry to the switch pointing at the local light endpoint, and have the switch drive the controller's OnOff attribute (and physical LED) directly over CASE.

---

## Files Changed

### `components/esp_matter_controller/core/esp_matter_controller_client.cpp`

**Combined commissioner + commissionee mode** — shares the server's FabricTable and GroupDataProvider with the factory so that:

1. The server's CASEServer can find the commissioner's fabric entry (NOC + NodeId) when a switch initiates CASE on port 5540.
2. The commissioner's IPK is written into the server's GroupDataProvider, making it visible to the server's CASEServer for Sigma1 destinationId lookup.

Key changes:
- `#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE` block added to `init()`:
  - `factory_init_params.fabricTable = &chip::Server::GetInstance().GetFabricTable()` — shared fabric table
  - `factory_init_params.groupDataProvider = chip::Server::GetInstance().GetGroupDataProvider()` — shared group data provider
  - `factory_init_params.enableServerInteractions = false` — avoids a redundant CASEServer on port 5580
  - `factory_init_params.dataModelProvider = chip::app::InteractionModelEngine::GetInstance()->GetDataModelProvider()` — passes the existing server data model provider back to avoid overwriting the server's light endpoint registration
- `GroupTesting::InitData` call removed — it was filling the 3-slot keyset limit in the server's GroupDataProvider before the commissioner's IPK could be written, causing `CHIP_ERROR_INVALID_LIST_LENGTH`
- `setup_commissioner()` IPK write updated to use `DeviceControllerFactory::GetInstance().GetSystemState()->GetGroupDataProvider()` so the IPK is always written to whichever provider the factory was initialized with

### `examples/controller/main/app_main.cpp`

- Added `#include <access/AccessControl.h>` and `#include <led_strip.h>`
- Added WS2812 LED initialisation (GPIO 38, RMT backend) at startup
- Added `app_attribute_update_cb` — drives the LED when the OnOff attribute changes:
  - ON: `led_strip_set_pixel` + `led_strip_refresh` (white, 16/16/16)
  - OFF: `led_strip_clear`
- Passed `app_attribute_update_cb` to `node::create()` (was `NULL`)
- Added `static uint16_t light_endpoint_id` and captured it after endpoint creation
- Added `on_switch_commissioned` callback (inside `#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE`):
  - Writes a Binding cluster entry to the commissioned switch pointing at the local light endpoint and OnOff cluster (cluster 6)
  - JSON format: `{"0:ARR-OBJ":[{"1:U64":<controller_node_id>,"3:U16":<light_ep>,"4:U32":6}]}` — required by the esp-matter json_to_tlv codec
  - Does **not** call `chip_stack_lock`/`chip_stack_unlock` — the callback already runs on the CHIP task with the stack held
- Added `commissioner_setup_command` console command that:
  1. Calls `matter_controller_client::get_instance().init(112233, 1, 5580)`
  2. Registers the `on_switch_commissioned` callback
  3. Calls `setup_commissioner()`
  4. Adds an Operate ACL entry to the local server for the commissioner's fabric → light endpoint → OnOff cluster, with a wildcard subject (all CASE-authenticated nodes on that fabric). This persists to NVS so the switch can invoke commands after a reboot without recommissioning.

### `examples/controller/main/matter_project_config.h`

- Removed explicit `#define CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE 1` — this caused a redefinition error because `CHIPDevicePlatformConfig.h` already defines it as `CONFIG_ENABLE_ESP32_BLE_CONTROLLER`

### `examples/controller/sdkconfig.defaults`

- Added `CONFIG_ENABLE_ESP32_BLE_CONTROLLER=y` — this is the Kconfig flag that enables combined commissioner + commissionee mode on ESP32

### `examples/controller/main/idf_component.yml` (new file)

- Added `espressif/led_strip` managed component dependency for the WS2812 RMT driver

---

## How It Works

```
Boot
 └─ LED initialised (GPIO 38, WS2812 via RMT)
 └─ Light endpoint created (extended_color_light, endpoint 1)
 └─ Matter server starts (port 5540)

Console: commissioner_setup
 └─ Factory initialised with shared FabricTable + GroupDataProvider
 └─ Commissioner fabric created (NodeId 112233)
 └─ IPK written to server's GroupDataProvider
 └─ ACL entry added: all CASE nodes on commissioner's fabric
    can Operate OnOff on endpoint 1  →  persisted to NVS

Console: matter esp controller pairing onnetwork <node_id> <pin>
 └─ Switch commissioned onto commissioner's fabric
 └─ on_switch_commissioned fires:
     └─ Writes Binding entry to switch → {node:112233, ep:1, cluster:OnOff}

Switch button press
 └─ Switch reads binding → initiates CASE to node 112233 on port 5540
 └─ Server authenticates: finds fabric in shared FabricTable ✓
                          finds IPK in server's GroupDataProvider ✓
 └─ CASE session established
 └─ OnOff Toggle command arrives at server's IM engine
 └─ app_attribute_update_cb fires → LED toggles
```

---

## Usage

```bash
# Build for ESP32-S3
idf.py set-target esp32s3 build
idf.py -p <PORT> erase-flash flash monitor

# On first boot (or after factory reset):
matter esp wifi connect <ssid> <password>

# Set up the commissioner (run once; ACL persists to NVS)
commissioner_setup

# Commission the switch
matter esp controller pairing onnetwork <node_id> 20202021

# The switch button now controls the LED directly
```

---

## Notes

- Factory reset the controller (`matter esp factoryreset`) if you hit IPK errors after flashing — stale NVS keyset data from previous runs can fill the 3-slot limit.
- The ACL entry added by `commissioner_setup` is append-only. If you run `commissioner_setup` multiple times without a factory reset, duplicate entries accumulate in NVS (harmless but wasteful).
- The binding JSON written to the switch uses esp-matter's json_to_tlv key format (`"<tag>:<type>"`), not human-readable field names.

// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sdkconfig.h>

#include <esp_check.h>
#include <esp_err.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_credentials_issuer.h>
#include <esp_matter_controller_pairing_command.h>

#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
#include <app/server/Server.h>
#endif

#include <app/InteractionModelEngine.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/OperationalCredentialsDelegate.h>
#include <controller_data_model_provider.h>
#include <credentials/GroupDataProvider.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/ScopedBuffer.h>
#include <lib/support/Span.h>
#include <lib/support/TestGroupData.h>
#include <stdint.h>

#include "esp_matter_data_model_provider.h"

#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
#include <esp_matter_attestation_trust_store.h>
#endif

#if CONFIG_ENABLE_ESP32_BLE_CONTROLLER
#include <platform/internal/BLEManager.h>
#endif

#if CHIP_DEVICE_CONFIG_DYNAMIC_SERVER
#include <app/clusters/ota-provider/ota-provider-cluster.h>
#include <app/dynamic_server/AccessControl.h>
#include <esp_matter_ota_provider.h>
#endif

#define TAG "MatterController"

using chip::Platform::ScopedMemoryBufferWithSize;

namespace esp_matter {
namespace controller {

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY
ESPCommissionerCallback commissioner_callback;
#endif

esp_err_t matter_controller_client::init(NodeId node_id, FabricId fabric_id, uint16_t listen_port)
{
    chip::Controller::FactoryInitParams factory_init_params;

#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
    // Combined commissioner+commissionee mode:
    // - Share the server's fabric table so the server's CASEServer can authenticate switches
    //   commissioned onto the commissioner's fabric (FindLocalNodeFromDestinationId iterates it).
    // - Use the server's GroupDataProvider as the factory's provider. setup_commissioner() will
    //   write the commissioner's IPK into it, making it visible to the server's CASEServer
    //   (CASEServer::ListenForSessionEstablishment was called with mGroupsProvider = server's provider).
    // - opCertStore and operationalKeystore are NOT provided: FabricTable::Init() is skipped when
    //   fabricTable is supplied externally (the server's table is already fully initialized).
    // - Do NOT call SetGroupDataProvider: Server::Init() already set the global to mGroupsProvider.
    factory_init_params.fabricTable = &chip::Server::GetInstance().GetFabricTable();
    factory_init_params.groupDataProvider = chip::Server::GetInstance().GetGroupDataProvider();

    ESP_RETURN_ON_FALSE(m_icd_client_storage.Init(&m_default_storage, &m_session_key_store) == CHIP_NO_ERROR, ESP_FAIL,
                        TAG, "Failed to initialize ICD client store");
#else
    ESP_RETURN_ON_FALSE(m_operational_keystore.Init(&m_default_storage) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to initialize operational keystore");
    ESP_RETURN_ON_FALSE(m_operational_cert_store.Init(&m_default_storage) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to initialize operational cert store");
    ESP_RETURN_ON_FALSE(m_icd_client_storage.Init(&m_default_storage, &m_session_key_store) == CHIP_NO_ERROR, ESP_FAIL,
                        TAG, "Failed to initialize ICD client store");
    factory_init_params.operationalKeystore = &m_operational_keystore;
    factory_init_params.opCertStore = &m_operational_cert_store;

    m_group_data_provider.SetStorageDelegate(&m_default_storage);
    m_group_data_provider.SetSessionKeystore(&m_session_key_store);
    m_group_data_provider.SetListener(&m_group_data_provider_listener);
    ESP_RETURN_ON_FALSE(m_group_data_provider.Init() == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to initialize group data provider");
    factory_init_params.groupDataProvider =
        reinterpret_cast<chip::Credentials::GroupDataProvider *>(&m_group_data_provider);
    chip::Credentials::SetGroupDataProvider(factory_init_params.groupDataProvider);
#endif // CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE

    factory_init_params.listenPort = listen_port;
    factory_init_params.fabricIndependentStorage = &m_default_storage;
    factory_init_params.sessionKeystore = &m_session_key_store;
#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
    // In combined mode, switches connect to port 5540 (server) so the factory does not need its
    // own CASEServer. Disable server interactions to skip creating a second CASEServer on port 5580.
    // InitSystemState still calls InteractionModelEngine::SetDataModelProvider unconditionally, so
    // pass the provider that is already set on the singleton (the server's) to make it a no-op and
    // avoid hiding the server's light endpoints from external commissioners (Apple Home etc.).
    factory_init_params.enableServerInteractions = false;
    factory_init_params.dataModelProvider = chip::app::InteractionModelEngine::GetInstance()->GetDataModelProvider();
#else
    factory_init_params.enableServerInteractions = m_operational_advertising;
    factory_init_params.dataModelProvider = &data_model::provider::get_instance();
#endif
    m_controller_node_id = node_id;
    m_controller_fabric_id = fabric_id;

    ESP_RETURN_ON_FALSE(chip::Controller::DeviceControllerFactory::GetInstance().Init(factory_init_params) ==
                            CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to initialize DeviceControllerFactory");
    
    auto *system_state = chip::Controller::DeviceControllerFactory::GetInstance().GetSystemState();
    m_group_data_provider_listener.Init(system_state);
    
    auto engine = chip::app::InteractionModelEngine::GetInstance();
    ESP_RETURN_ON_FALSE(engine, ESP_ERR_INVALID_STATE, TAG, "No interaction model engine");
    ESP_RETURN_ON_FALSE(m_icd_check_in_delegate.Init(&m_icd_client_storage, engine) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to initialize check in delegate");
    ESP_RETURN_ON_FALSE(m_check_in_handler.Init(system_state->ExchangeMgr(), &m_icd_client_storage,
                                                &m_icd_check_in_delegate, engine) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to initialize Check In handler");
#if CHIP_DEVICE_CONFIG_DYNAMIC_SERVER
    auto &ota_provider = ota_provider::EspOtaProvider::GetInstance();
    ESP_RETURN_ON_ERROR(
        ota_provider.Init(true, system_state->SystemLayer(), system_state->ExchangeMgr(), system_state->Fabrics()), TAG,
        "Failed to initialize OTA provider");
    data_model::provider::get_instance().set_ota_provider_delegate(&ota_provider);
    chip::app::dynamic_server::InitAccessControl();
#endif
    return ESP_OK;
}

#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
esp_err_t matter_controller_client::setup_commissioner()
{
#if CONFIG_ENABLE_ESP32_BLE_CONTROLLER
    CHIP_ERROR err = chip::DeviceLayer::Internal::BLEMgr().Init();
    // This function will return CHIP_ERROR_INCORRECT_STATE if BLE Manager is already initialized.
    ESP_RETURN_ON_FALSE(err == CHIP_NO_ERROR || err == CHIP_ERROR_INCORRECT_STATE, ESP_FAIL, TAG,
                        "Failed to initialize the BLE manager");
    ESP_RETURN_ON_FALSE(chip::DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(0, true) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to configure BLEManager");
#endif
    chip::Controller::SetupParams commissioner_params;
    const chip::Credentials::AttestationTrustStore *trust_store = chip::Credentials::get_attestation_trust_store();
    chip::Credentials::DeviceAttestationVerifier *dac_verifier = chip::Credentials::GetDefaultDACVerifier(trust_store);
    chip::Credentials::SetDeviceAttestationVerifier(dac_verifier);
    commissioner_params.deviceAttestationVerifier = dac_verifier;
    m_credentials_issuer = get_credentials_issuer();
    ESP_RETURN_ON_FALSE(m_credentials_issuer, ESP_FAIL, TAG,
                        "Please set the custom credentials_issuer before calling setup_commissioner");
    ESP_RETURN_ON_ERROR(m_credentials_issuer->initialize_credentials_issuer(m_default_storage), TAG,
                        "Failed to initialize credentials_issuer");
    commissioner_params.operationalCredentialsDelegate = m_credentials_issuer->get_delegate();
    commissioner_params.controllerVendorId = chip::VendorId((uint16_t)CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID);

    // Commissioner NOC chain
    ScopedMemoryBufferWithSize<uint8_t> noc;
    noc.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(noc.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for noc");
    chip::MutableByteSpan noc_span(noc.Get(), chip::Controller::kMaxCHIPDERCertLength);
    ScopedMemoryBufferWithSize<uint8_t> icac;
    icac.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(icac.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for icac");
    chip::MutableByteSpan icac_span(icac.Get(), chip::Controller::kMaxCHIPDERCertLength);
    ScopedMemoryBufferWithSize<uint8_t> rcac;
    rcac.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(rcac.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for rcac");
    chip::MutableByteSpan rcac_span(rcac.Get(), chip::Controller::kMaxCHIPDERCertLength);
    // NOC Keypair
    chip::Crypto::P256Keypair ephemeral_key;
    ESP_RETURN_ON_FALSE(ephemeral_key.Initialize(chip::Crypto::ECPKeyTarget::ECDSA) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to initialize ephemeral_key pair");
    ESP_RETURN_ON_ERROR(m_credentials_issuer->generate_controller_noc_chain(m_controller_node_id,
                                                                            m_controller_fabric_id, ephemeral_key,
                                                                            rcac_span, icac_span, noc_span),
                        TAG, "Failed to generate NOC chain");
    commissioner_params.operationalKeypair = &ephemeral_key;
    commissioner_params.controllerRCAC = rcac_span;
    commissioner_params.controllerICAC = icac_span;
    commissioner_params.controllerNOC = noc_span;
    commissioner_params.defaultCommissioner = &m_auto_commissioner;
    commissioner_params.enableServerInteractions = m_operational_advertising;
    auto &factory = chip::Controller::DeviceControllerFactory::GetInstance();
    ESP_RETURN_ON_FALSE(factory.SetupCommissioner(commissioner_params, m_device_commissioner) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to setup commissioner");

    // Initialize Group Data, including IPK
    chip::FabricIndex fabric_index = m_device_commissioner.GetFabricIndex();
    ESP_RETURN_ON_FALSE(fabric_index != chip::kUndefinedFabricIndex, ESP_FAIL, TAG, "Invalid Fabric Index");
    m_icd_client_storage.UpdateFabricList(fabric_index);
    uint8_t compressed_fabric_id[sizeof(uint64_t)] = {0};
    chip::MutableByteSpan compressed_fabric_id_span(compressed_fabric_id);
    ESP_RETURN_ON_FALSE(m_device_commissioner.GetCompressedFabricIdBytes(compressed_fabric_id_span) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to get compressed_fabric_id");
    // Write the IPK for the commissioner's fabric into whichever GroupDataProvider the
    // factory was initialized with. In combined mode this is the server's GroupDataProvider,
    // making the IPK visible to the server's CASEServer when a switch initiates CASE on
    // port 5540. In standalone mode this is m_group_data_provider as before.
    chip::Credentials::GroupDataProvider *group_data_provider =
        chip::Controller::DeviceControllerFactory::GetInstance().GetSystemState()->GetGroupDataProvider();
    chip::ByteSpan default_ipk = chip::GroupTesting::DefaultIpkValue::GetDefaultIpk();
    {
        CHIP_ERROR ipk_err = chip::Credentials::SetSingleIpkEpochKey(group_data_provider, fabric_index, default_ipk,
                                                                     compressed_fabric_id_span);
        ESP_RETURN_ON_FALSE(ipk_err == CHIP_NO_ERROR, ESP_FAIL, TAG,
                            "Failed to set ipk for commissioner fabric: %" CHIP_ERROR_FORMAT, ipk_err.Format());
    }

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY
    get_discovery_controller()->SetUserDirectedCommissioningServer(
        get_commissioner()->GetUserDirectedCommissioningServer());
    get_discovery_controller()->SetCommissionerCallback(&commissioner_callback);
#endif

    m_device_commissioner.RegisterPairingDelegate(nullptr);

    return ESP_OK;
}

#else
esp_err_t matter_controller_client::setup_controller(chip::MutableByteSpan &ipk)
{
    chip::Controller::SetupParams controller_params;
    // Controller doesn't need to verify device attestation. Set deviceAttestationVerifier to null.
    controller_params.deviceAttestationVerifier = nullptr;
    m_credentials_issuer = get_credentials_issuer();
    ESP_RETURN_ON_FALSE(m_credentials_issuer, ESP_FAIL, TAG,
                        "Please set the custom credentials_issuer before calling setup_controller");
    ESP_RETURN_ON_ERROR(m_credentials_issuer->initialize_credentials_issuer(m_default_storage), TAG,
                        "Failed to initialize credentials_issuer");
    controller_params.operationalCredentialsDelegate = m_credentials_issuer->get_delegate();
    controller_params.controllerVendorId = chip::VendorId((uint16_t)CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID);
    // Commissioner NOC chain
    ScopedMemoryBufferWithSize<uint8_t> noc;
    noc.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(noc.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for noc");
    chip::MutableByteSpan noc_span(noc.Get(), chip::Controller::kMaxCHIPDERCertLength);
    ScopedMemoryBufferWithSize<uint8_t> icac;
    icac.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(icac.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for icac");
    chip::MutableByteSpan icac_span(icac.Get(), chip::Controller::kMaxCHIPDERCertLength);
    ScopedMemoryBufferWithSize<uint8_t> rcac;
    rcac.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(rcac.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for rcac");
    chip::MutableByteSpan rcac_span(rcac.Get(), chip::Controller::kMaxCHIPDERCertLength);
    chip::Crypto::P256Keypair ephemeral_key;
    ESP_RETURN_ON_ERROR(m_credentials_issuer->generate_controller_noc_chain(m_controller_node_id,
                                                                            m_controller_fabric_id, ephemeral_key,
                                                                            rcac_span, icac_span, noc_span),
                        TAG, "Failed to generate NOC chain");
    // Check whether the keypair is initialized in generate_controller_noc_chain
    bool is_keypair_initialized = false;
    {
        chip::Crypto::P256ECDSASignature signature;
        is_keypair_initialized = ephemeral_key.ECDSA_sign_msg(NULL, 0, signature) != CHIP_ERROR_UNINITIALIZED;
    }
    // If not initialized, use an empty keypair.
    controller_params.operationalKeypair = is_keypair_initialized ? &ephemeral_key : nullptr;
    controller_params.controllerRCAC = rcac_span;
    controller_params.controllerICAC = icac_span;
    controller_params.controllerNOC = noc_span;
    controller_params.defaultCommissioner = nullptr;
    controller_params.enableServerInteractions = m_operational_advertising;
    controller_params.permitMultiControllerFabrics = false;
    auto &factory = chip::Controller::DeviceControllerFactory::GetInstance();
    ESP_RETURN_ON_FALSE(factory.SetupController(controller_params, m_device_controller) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to setup controller");

    chip::FabricIndex fabric_index = m_device_controller.GetFabricIndex();
    if (fabric_index != chip::kUndefinedFabricIndex) {
        m_icd_client_storage.UpdateFabricList(fabric_index);
    }
    if (fabric_index != chip::kUndefinedFabricIndex && !ipk.empty()) {
        // If we have created fabric in SetupController and IPK input is not empty, initialize Group Data with IPK.
        // Otherwise we will initialize Group Data with IPK later.
        uint8_t compressed_fabric_id[sizeof(uint64_t)] = {0};
        chip::MutableByteSpan compressed_fabric_id_span(compressed_fabric_id);
        ESP_RETURN_ON_FALSE(m_device_controller.GetCompressedFabricIdBytes(compressed_fabric_id_span) == CHIP_NO_ERROR,
                            ESP_FAIL, TAG, "Failed to get compressed_fabric_id");
        chip::Credentials::GroupDataProvider *group_data_provider =
            reinterpret_cast<chip::Credentials::GroupDataProvider *>(&m_group_data_provider);
        chip::Credentials::GroupDataProvider::KeySet keyset;
        keyset.keyset_id = chip::Credentials::GroupDataProvider::kIdentityProtectionKeySetId;
        keyset.policy = chip::app::Clusters::GroupKeyManagement::GroupKeySecurityPolicyEnum::kTrustFirst;
        keyset.num_keys_used = 1;
        keyset.epoch_keys[0].start_time = 0;
        memcpy(keyset.epoch_keys[0].key, ipk.data(), chip::Crypto::CHIP_CRYPTO_SYMMETRIC_KEY_LENGTH_BYTES);
        ESP_RETURN_ON_FALSE(group_data_provider->SetKeySet(fabric_index, compressed_fabric_id_span, keyset) ==
                                CHIP_NO_ERROR,
                            ESP_FAIL, TAG, "Failed to set ipk for commissioner fabric");
    }

    return ESP_OK;
}
#endif // CONFIG_ESP_MATTER_COMMISSIONER_ENABLE

} // namespace controller
} // namespace esp_matter

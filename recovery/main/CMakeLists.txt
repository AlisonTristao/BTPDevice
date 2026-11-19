# The portable pieces come straight from the library's sources, so the
# recovery and the application agree on the contract by construction.
set(_src "${CMAKE_CURRENT_LIST_DIR}/../../src")
idf_component_register(
    SRCS "main.cpp"
         "${_src}/security/KeyStore.cpp"
         "${_src}/security/Sha256.cpp"
         "${_src}/transport/WifiNetworks.cpp"
         "${_src}/update/OtaAuth.cpp"
    INCLUDE_DIRS "." "${_src}"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server app_update esp_partition
                  bootloader_support esp_timer esp_app_format BTP)

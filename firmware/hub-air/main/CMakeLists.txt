idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto kvm_hublink kvm_webui
             nvs_flash esp_wifi esp_event esp_netif
)

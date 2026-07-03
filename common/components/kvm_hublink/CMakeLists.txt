idf_component_register(
    SRCS "link.c" "store.c"
    INCLUDE_DIRS "include"
    REQUIRES kvm_proto kvm_crypto nvs_flash esp_wifi esp_event esp_netif esp_timer
)

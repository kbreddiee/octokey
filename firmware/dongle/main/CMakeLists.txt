idf_component_register(
    SRCS "main.c" "store.c" "usb_dev.c" "link.c"
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto nvs_flash esp_wifi esp_event esp_timer driver
)

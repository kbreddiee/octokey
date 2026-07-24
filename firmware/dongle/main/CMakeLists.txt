set(srcs "main.c" "store.c" "link.c")

if(CONFIG_ESPKVM_LCD)
    list(APPEND srcs "lcd.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto kvm_font kvm_usbdev
             nvs_flash esp_wifi esp_event esp_timer driver
)

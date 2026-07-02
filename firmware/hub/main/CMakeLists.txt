idf_component_register(
    SRCS "main.c" "store.c" "link.c" "usb_kbd.c" "input.c" "ui.c" "oled.c"
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto kvm_hidparse kvm_font
             nvs_flash esp_wifi esp_event esp_timer driver usb
)

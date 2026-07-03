idf_component_register(
    SRCS "main.c" "kbd_i2c.c" "trackball.c" "disp.c"
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto kvm_hublink kvm_font
             nvs_flash esp_wifi esp_event esp_timer driver
)

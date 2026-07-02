set(srcs "main.c" "store.c" "link.c" "usb_kbd.c" "input.c" "ui.c")

# One display backend, same API (oled.h): SSD1306 for the DevKit build,
# ST7789 for the T-Embed build.
if(CONFIG_ESPKVM_HUB_BOARD_TEMBED)
    list(APPEND srcs "disp_st7789.c")
else()
    list(APPEND srcs "oled.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto kvm_hidparse kvm_font
             nvs_flash esp_wifi esp_event esp_timer driver usb
)

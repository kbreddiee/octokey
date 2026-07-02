set(srcs "main.c" "store.c" "link.c" "usb_kbd.c" "input.c")

# One UI per board, same interface (ui.h). Display boards share ui.c with
# a display backend behind the oled.h API; the custom PCB is button+LEDs.
if(CONFIG_ESPKVM_HUB_BOARD_HEADLESS)
    list(APPEND srcs "ui_headless.c")
elseif(CONFIG_ESPKVM_HUB_BOARD_TEMBED)
    list(APPEND srcs "ui.c" "disp_st7789.c")
else()
    list(APPEND srcs "ui.c" "oled.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES kvm_proto kvm_crypto kvm_hidparse kvm_font
             nvs_flash esp_wifi esp_event esp_timer driver usb
)

# espressif__esp_tinyusb is the managed component fetched via this
# directory's idf_component.yml; named explicitly because the projects
# trim their build to `set(COMPONENTS main)`, which drops the component
# manager's auto-injected requirement.
idf_component_register(
    SRCS "usb_dev.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer espressif__esp_tinyusb
)

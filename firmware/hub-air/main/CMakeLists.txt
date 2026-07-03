idf_component_register(
    SRCS "main.c" "webui.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES kvm_proto kvm_crypto kvm_hublink
             nvs_flash esp_wifi esp_event esp_netif esp_http_server json
)

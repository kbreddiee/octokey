idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html"
    EMBED_FILES "web/logo.png"
    REQUIRES kvm_proto kvm_hublink esp_http_server json
)

idf_component_register(
    SRCS "kvm_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES kvm_proto mbedtls esp_hw_support log
)

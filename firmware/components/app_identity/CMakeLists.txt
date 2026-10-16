idf_component_register(
    SRCS "app_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES app_store
    PRIV_REQUIRES esp_hw_support mbedtls
)

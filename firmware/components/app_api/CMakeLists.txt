idf_component_register(
    SRCS "app_api.c"
    INCLUDE_DIRS "include"
    REQUIRES app_identity
    PRIV_REQUIRES esp_http_client esp-tls
)

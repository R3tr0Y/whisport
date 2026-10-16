idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_store app_identity app_net app_api app_ui
    PRIV_REQUIRES esp_event nvs_flash
)

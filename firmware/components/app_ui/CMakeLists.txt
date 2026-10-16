idf_component_register(
    SRCS "app_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES console
    PRIV_REQUIRES esp_timer
)

idf_component_register(
    SRCS "main.c" "ui.c" "net_prov.c"
    INCLUDE_DIRS "."
    REQUIRES bsp app_store app_identity app_net app_api
    PRIV_REQUIRES esp_event esp_timer nvs_flash console lvgl esp_wifi json esp_http_server
)

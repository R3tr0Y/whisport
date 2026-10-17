idf_component_register(
    SRCS "app_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash app_store app_identity esp_timer
)

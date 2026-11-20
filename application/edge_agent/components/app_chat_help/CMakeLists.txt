idf_component_register(
    SRCS "app_chat_help.c" "device_status.c" "token_usage.c"
         "app_cloud_media.c" "cloud_media_store.c" "cloud_media_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES claw_event_router
    PRIV_REQUIRES app_claw claw_cap claw_core esp_http_client esp_system esp_timer heap mbedtls nvs_flash
)

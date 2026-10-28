idf_component_register(
    SRCS
        "main.c"
        "ota_web.c"

    INCLUDE_DIRS
        "."

    PRIV_REQUIRES
        driver
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        app_update
        esp_partition
        nvs_flash
)
idf_component_register(
    SRCS
        "main.c"
        "web_control.c"
    INCLUDE_DIRS "."
    REQUIRES
        BSP
        Middlewares
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
)

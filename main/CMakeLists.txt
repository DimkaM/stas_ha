idf_component_register(
    SRCS "main.cpp" "ads1115.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash mqtt esp_driver_i2c esp_timer
)

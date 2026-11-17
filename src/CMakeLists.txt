# The dashboard: every source under src/.
file(GLOB_RECURSE app_sources
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
)

# What the application, the board support in lib/boardstuff and NightMareNetwork use between them. PlatformIO builds
# the libraries with this component's include paths, so a driver one of them needs has to be named here.
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_adc esp_driver_ledc esp_driver_i2c esp_driver_spi esp_driver_gpio
             esp_timer esp_wifi esp_netif esp_event nvs_flash spiffs lwip esp_ringbuf esp_pm app_update esp_app_format mbedtls
)

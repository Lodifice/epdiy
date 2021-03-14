set(app_sources "main.c" "server.c" "wifi.c")

idf_component_register(SRCS ${app_sources} REQUIRES epd_driver nvs_flash)

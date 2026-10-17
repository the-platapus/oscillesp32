idf_component_register(
    SRCS "main.cpp" "settings.cpp" "acquisition.cpp" "measure.cpp" "siggen.cpp"
         "commands.cpp" "web.cpp" "display.cpp" "ui.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc_cal esp_wifi esp_event esp_netif nvs_flash
             esp_http_server mdns lwip esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-missing-field-initializers)

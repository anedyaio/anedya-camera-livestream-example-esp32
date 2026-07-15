FILE(GLOB AnedyaSDKSource         ./core/src/*.c
     GLOB AnedyaInterfaceQuectel  ./interfaces/esp32-quectel/*.c
     GLOB AnedyaInterfaceWifi     ./interfaces/esp32-wifi/*.c
     GLOB AnedyaInterfaceWifiQuectel  ./interfaces/esp32-wifi-quectel/*.c)
idf_component_register(SRCS ${AnedyaSDKSource} ${AnedyaInterfaceQuectel} ${AnedyaInterfaceWifi} ${AnedyaInterfaceWifiQuectel}
                    INCLUDE_DIRS "./core/include" "./interfaces/include"
                    PRIV_REQUIRES esp_event esp_netif esp-tls espressif__mqtt driver esp_http_client)

# Vendored SDK is not const-clean and has unused vars; GCC 15 in IDF 6 treats
# these as errors by default. Don't fail the build on the vendor's warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=discarded-qualifiers -Wno-error=unused-variable)

# Connection method flags are handled by anedya_sdk_config.h via CONFIG_CONN_ANEDYA_* Kconfig values.
# Do NOT add -DANEDYA_CONNECTION_METHOD_* here; the header derives them from sdkconfig.

# ── Interface compile-time flags + embedded certificates ──────────────────────
if(CONFIG_AN_INTERFACE_ESP32_WIFI)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ANEDYA_EMBED_DER)
endif()

if(CONFIG_AN_INTERFACE_ESP32_QUETEL)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ANEDYA_EMBED_PEM)
endif()

if(CONFIG_AN_INTERFACE_ESP32_WIFI_QUETEL)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ANEDYA_EMBED_PEM)
endif()
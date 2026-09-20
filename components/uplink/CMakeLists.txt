# Exactly one implementation is compiled: the HaLow transceiver when
# CONFIG_HALOW_UPLINK is set, the native Wi-Fi STA otherwise. Both files also
# guard their whole body on that symbol, so a stale object never links.
set(uplink_srcs uplink_wifi.c)

if(CONFIG_HALOW_UPLINK)
    set(uplink_srcs uplink_halow.c)
endif()

# morsemicro__halow has to be required unconditionally. ESP-IDF resolves
# component requirements in an early expansion pass that runs each component's
# CMakeLists before any CONFIG_* value is defined, so a requirement added
# inside if(CONFIG_HALOW_UPLINK) is silently dropped -- the source list above
# still switches correctly on the second pass, but the include paths never
# arrive and mmhalow.h fails to resolve. The dependency is declared for every
# target anyway (main/idf_component.yml), and the native build simply never
# references it.
idf_component_register(SRCS ${uplink_srcs}
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event esp_driver_gpio morsemicro__halow)

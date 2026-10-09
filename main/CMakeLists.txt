# Wi-Fi details live in wifi_config.h, which is not in git (it holds your
# password). On a fresh checkout, create it from the template.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/wifi_config.h")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/wifi_config.example.h"
                   "${CMAKE_CURRENT_LIST_DIR}/wifi_config.h" COPYONLY)
    message(WARNING "Created main/wifi_config.h from the template: "
                    "put your Wi-Fi name and password in it, then build again.")
endif()

idf_component_register(
    SRCS "main.cpp" "opcua_server.c" "wifi_net.c" "ns0_fixups.c"
    INCLUDE_DIRS "."
    REQUIRES open62541 esp_wifi esp_netif esp_event esp_timer nvs_flash lwip esp_pm
)

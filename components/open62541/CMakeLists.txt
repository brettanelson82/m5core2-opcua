# open62541 v1.5.8 (OPC UA stack), pre-generated as a single .c/.h pair
# for ESP32 (FreeRTOS + lwIP). See components/open62541/README.md.
idf_component_register(
    SRCS "open62541.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_timer freertos
)
# Third-party code: don't let its warnings stop the build
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

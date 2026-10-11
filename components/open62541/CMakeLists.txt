# open62541 v1.5.8 (OPC UA stack), pre-generated as a single .c/.h pair
# for ESP32 (FreeRTOS + lwIP). See components/open62541/README.md.
idf_component_register(
    SRCS "open62541.c"
    INCLUDE_DIRS "include"
    # mbedtls: encryption (security policies, certificate creation and checks)
    # esp_hw_support: heap_caps_* used by ua_psram_alloc.h
    REQUIRES lwip esp_timer freertos mbedtls esp_hw_support heap
)
# Third-party code: don't let its warnings stop the build
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
# Allocate from PSRAM first (see include/ua_psram_alloc.h). PUBLIC so code
# using open62541 (main/) agrees on the allocator.
target_compile_options(${COMPONENT_LIB} PUBLIC
    "-include" "${CMAKE_CURRENT_LIST_DIR}/include/ua_psram_alloc.h")

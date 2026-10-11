/*
 * ua_psram_alloc.h - make open62541 allocate from PSRAM first.
 *
 * The ESP32 has ~300 kB of internal RAM, which Wi-Fi and lwIP need, and the
 * Core2 adds 8 MB of PSRAM. By default every allocation up to 1 kB goes to
 * internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and the OPC UA address
 * space is thousands of such small allocations: about 100 kB of internal RAM,
 * enough that the server could not open its socket once security was on.
 *
 * open62541.h uses UA_malloc & co. if they are already defined, so this file
 * is force-included (compiler option -include, see CMakeLists.txt) into the
 * library and into everything that uses it. Everything allocated and freed
 * by open62541 then goes through these, falling back to internal RAM if
 * PSRAM is ever full. free() releases memory from either.
 */
#pragma once

#include <stdlib.h>
#include "esp_heap_caps.h"

#define UA_malloc(size) \
    heap_caps_malloc_prefer((size), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#define UA_calloc(num, size) \
    heap_caps_calloc_prefer((num), (size), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#define UA_realloc(ptr, size) \
    heap_caps_realloc_prefer((ptr), (size), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#define UA_free(ptr) free(ptr)

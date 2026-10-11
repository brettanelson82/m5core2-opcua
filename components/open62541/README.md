# open62541 (pre-generated)

**License:** Mozilla Public License 2.0, see [LICENSE](LICENSE) (copied from
open62541 v1.5.8). Upstream source: <https://github.com/open62541/open62541>.
`open62541.c` here is a modified copy (the clock patch below); MPL-2.0 requires
that the source of these files stays available to anyone who receives
firmware built from them.

`open62541.c` / `include/open62541.h` are the single-file ("amalgamated") build of
**open62541 v1.5.8** with **mbedTLS encryption**, generated for ESP32 so you don't
have to run the generator.

To regenerate, run [`tools/gen_open62541.ps1`](../../tools/gen_open62541.ps1) (see the
top of that file). The settings are:

```
-DCMAKE_BUILD_TYPE=MinSizeRel
-DUA_ARCHITECTURE=freertos-lwip -DUA_LWIP_EXTERNAL=ON
-DUA_ENABLE_AMALGAMATION=ON -DUA_NAMESPACE_ZERO=REDUCED
-DUA_ENABLE_ENCRYPTION=MBEDTLS  (headers from ESP-IDF's components/mbedtls)
-DUA_ENABLE_PUBSUB=OFF -DUA_ENABLE_HISTORIZING=OFF
-DUA_ENABLE_JSON_ENCODING=OFF -DUA_ENABLE_XML_ENCODING=OFF
-DUA_ENABLE_DISCOVERY_MULTICAST=OFF -DUA_ENABLE_DIAGNOSTICS=OFF
-DUA_ENABLE_SUBSCRIPTIONS=ON -DUA_ENABLE_METHODCALLS=ON
-DUA_ENABLE_NODEMANAGEMENT=ON -DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF
-DUA_MULTITHREADING=0 -DUA_LOGLEVEL=300
then: python tools/patch_amalgamation.py open62541.c
```

Keep `MinSizeRel` (or `Release`): open62541 makes a debug build when no build
type is given, which adds `UA_DEBUG` checks. XML encoding stays off to save
flash; `main/ns0_fixups.c` fills in the standard values that this leaves empty.

**Allocation:** `include/ua_psram_alloc.h` is force-included (see `CMakeLists.txt`)
so open62541 allocates from PSRAM first. This is a build setting, not a change
to the library's files.

**One patch is applied** (`tools/patch_amalgamation.py`). The stock FreeRTOS clock
counts ticks since boot, which would timestamp everything as 1970. The patched
version uses the ESP32's real clock (`gettimeofday`, set from NTP/RTC) and
`esp_timer` for the monotonic clock.

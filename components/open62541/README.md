# open62541 (pre-generated)

**License:** Mozilla Public License 2.0, see [LICENSE](LICENSE) (copied from
open62541 v1.5.8). Upstream source: <https://github.com/open62541/open62541>.
`open62541.c` here is a modified copy (the clock patch below); MPL-2.0 requires
that the source of these files stays available to anyone who receives
firmware built from them.

`open62541.c` / `include/open62541.h` are the single-file ("amalgamated") build of
**open62541 v1.5.8**, generated for ESP32 so you don't have to run the generator.

Generated with:

```
cmake .. -DUA_ARCHITECTURE=freertos-lwip -DUA_LWIP_EXTERNAL=ON \
  -DUA_ENABLE_AMALGAMATION=ON -DUA_NAMESPACE_ZERO=REDUCED \
  -DUA_ENABLE_ENCRYPTION=OFF -DUA_ENABLE_PUBSUB=OFF -DUA_ENABLE_HISTORIZING=OFF \
  -DUA_ENABLE_JSON_ENCODING=OFF -DUA_ENABLE_XML_ENCODING=OFF \
  -DUA_ENABLE_DISCOVERY_MULTICAST=OFF -DUA_ENABLE_DIAGNOSTICS=OFF \
  -DUA_ENABLE_SUBSCRIPTIONS=ON -DUA_ENABLE_METHODCALLS=ON \
  -DUA_ENABLE_NODEMANAGEMENT=ON -DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF \
  -DUA_MULTITHREADING=0 -DUA_LOGLEVEL=300
make open62541-amalgamation
python3 tools/patch_amalgamation.py open62541.c
```

(The FreeRTOS kernel path only has to exist for CMake to configure. Nothing from
it ends up in the output.)

**One patch is applied** (`tools/patch_amalgamation.py`). The stock FreeRTOS clock
counts ticks since boot, which would timestamp everything as 1970. The patched
version uses the ESP32's real clock (`gettimeofday`, set from NTP/RTC) and
`esp_timer` for the monotonic clock.

To turn on certificate security later, regenerate with
`-DUA_ENABLE_ENCRYPTION=MBEDTLS` and add `mbedtls` to `REQUIRES` in `CMakeLists.txt`.

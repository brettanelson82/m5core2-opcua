#!/usr/bin/env python3
"""Patch the open62541 v1.5 amalgamation (freertos-lwip) for ESP-IDF.

The stock FreeRTOS clock counts ticks since boot, which makes every OPC UA
timestamp show up as 1970. On ESP-IDF we have a real wall clock (set from SNTP
or the Core2 RTC), so we use gettimeofday() / esp_timer instead.

Usage: python3 patch_amalgamation.py path/to/open62541.c
"""
import sys

OLD_START = "#ifdef UA_ARCHITECTURE_FREERTOS\n\n#include <FreeRTOS.h>\n#include <task.h>\n"
OLD_END = "#endif /* UA_ARCHITECTURE_FREERTOS */"

NEW = """#ifdef UA_ARCHITECTURE_FREERTOS

/* ---- Patched for ESP-IDF (M5Stack Core2 OPC UA project) ---- */
#include <sys/time.h>
#include <time.h>
#include "esp_timer.h"

/* The current time in UTC (system clock, set from SNTP or the RTC) */
UA_DateTime UA_DateTime_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((UA_DateTime)tv.tv_sec * UA_DATETIME_SEC) +
           ((UA_DateTime)tv.tv_usec * UA_DATETIME_USEC) + UA_DATETIME_UNIX_EPOCH;
}

/* The device keeps its clock in UTC */
UA_Int64 UA_DateTime_localTimeUtcOffset(void) {
    return 0;
}

/* Monotonic clock for timers (microseconds since boot) */
UA_DateTime UA_DateTime_nowMonotonic(void) {
    return ((UA_DateTime)esp_timer_get_time() * UA_DATETIME_USEC) + UA_DATETIME_UNIX_EPOCH;
}

#endif /* UA_ARCHITECTURE_FREERTOS */"""

path = sys.argv[1]
src = open(path, encoding="utf-8").read()
if "Patched for ESP-IDF" in src:
    print("already patched"); sys.exit(0)
a = src.index(OLD_START)
b = src.index(OLD_END, a) + len(OLD_END)
src = src[:a] + NEW + src[b:]
open(path, "w", encoding="utf-8").write(src)
print("patched clock functions")

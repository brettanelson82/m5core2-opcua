/*
 * ============================================================
 *   TEMPLATE  -  copied to wifi_config.h on the first build
 * ============================================================
 *
 * Put your Wi-Fi details in main/wifi_config.h (not in this file).
 * The first build creates wifi_config.h from this template if it is
 * missing. wifi_config.h is listed in .gitignore, so your password
 * never ends up in git.
 *
 * The Core2 only supports 2.4 GHz Wi-Fi (not 5 GHz).
 */
#pragma once

#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

/* Time zone used for the clock on the screen (POSIX TZ format).
 * OPC UA timestamps are always UTC, this only affects the display.
 *   US Eastern : "EST5EDT,M3.2.0,M11.1.0"
 *   US Central : "CST6CDT,M3.2.0,M11.1.0"
 *   US Mountain: "MST7MDT,M3.2.0,M11.1.0"
 *   US Pacific : "PST8PDT,M3.2.0,M11.1.0"
 *   UK         : "GMT0BST,M3.5.0/1,M10.5.0"
 */
#define LOCAL_TIMEZONE  "EST5EDT,M3.2.0,M11.1.0"

/* Time server used to set the clock (needs internet access) */
#define NTP_SERVER      "pool.ntp.org"

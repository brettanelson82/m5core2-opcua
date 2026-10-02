/*
 * wifi_net.h - Wi-Fi connection and clock sync (see wifi_net.c).
 * Network name / password / NTP server are set in wifi_config.h.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_start(void);          /* start connecting in the background */
void wifi_wait_connected(void); /* block until we have an IP address   */
int  wifi_rssi(void);           /* signal strength in dBm, 0 if unknown */
bool time_sync(void);           /* set the system clock from NTP        */

#ifdef __cplusplus
}
#endif

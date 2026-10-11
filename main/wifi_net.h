/*
 * wifi_net.h - Wi-Fi connection and clock sync (see wifi_net.c).
 * Network name / password / NTP server are set in wifi_config.h.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Device ID: the last 3 bytes of the Wi-Fi MAC as 6 lowercase hex digits
 * (e.g. "a1b2c3"), read from eFuse, so it works before Wi-Fi starts. out
 * needs 7 bytes. Unique per chip: the first 3 MAC bytes are Espressif's. */
void wifi_device_id(char *out);

/* Network name of the device, "m5core2-<device id>" (DHCP hostname; also in
 * the server certificate). Valid after wifi_start(). */
const char *wifi_hostname(void);

void wifi_start(void);          /* start connecting in the background */
void wifi_wait_connected(void); /* block until we have an IP address   */
int  wifi_rssi(void);           /* signal strength in dBm, 0 if unknown */
bool time_sync(void);           /* set the system clock from NTP        */

#ifdef __cplusplus
}
#endif

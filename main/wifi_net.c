/*
 * wifi_net.c - connect to Wi-Fi (and keep reconnecting), then sync the clock.
 *
 * The network name and password come from wifi_config.h. Connection state is
 * shared two ways:
 *   - g_state.ip_address holds the current IP ("" while disconnected); the
 *     screen and the OPC UA server read it from there
 *   - CONNECTED_BIT in s_events lets wifi_wait_connected() block until an
 *     IP address has been assigned
 * Disconnect reasons are logged as "disconnected (reason N, rssi ...)". N is
 * an ESP-IDF wifi_err_reason_t value, e.g. 2 = auth expired, 4 = inactivity,
 * 8 = left the network, 201 = network not found, 204 = handshake timeout.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi_net.h"
#include "wifi_config.h"
#include "device_state.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events; /* holds CONNECTED_BIT */
static char s_hostname[24];         /* "m5core2-<device id>", set in wifi_start */

void wifi_device_id(char *out) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 7, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

const char *wifi_hostname(void) { return s_hostname; }
#define CONNECTED_BIT BIT0          /* set while we have an IP address */

/* ESP-IDF event handler (runs on the system event task):
 *   STA_START         -> start the first connection attempt
 *   STA_DISCONNECTED  -> clear the IP, log the reason, try again immediately
 *                        (the driver paces its own retries, so no loop here)
 *   STA_GOT_IP        -> store the IP and wake anyone in wifi_wait_connected() */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        state_lock();
        g_state.ip_address[0] = '\0';
        state_unlock();
        ESP_LOGW(TAG, "disconnected (reason %d, rssi %d) - retrying", e->reason, e->rssi);
        esp_wifi_connect();
    } else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        state_lock();
        snprintf(g_state.ip_address, sizeof(g_state.ip_address), IPSTR, IP2STR(&e->ip_info.ip));
        state_unlock();
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

/* Initialise NVS (the Wi-Fi driver stores calibration data there), the
 * network stack and the Wi-Fi driver, then start connecting in station mode.
 * Returns straight away; the connection happens in the background through
 * on_event(). Any setup failure is fatal (ESP_ERROR_CHECK restarts the chip). */
void wifi_start(void) {
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    char id[7];
    wifi_device_id(id);
    snprintf(s_hostname, sizeof(s_hostname), "m5core2-%s", id);
    esp_netif_set_hostname(sta, s_hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Mesh networks have several access points with the same name: scan them
     * all and join the strongest instead of the first one heard */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.rssi = -80;
    /* Use 802.11r (fast roaming) when the router offers it. Some mesh routers
     * never finish the plain WPA2 handshake once they advertise FT-PSK. */
    wc.sta.ft_enabled = 1;
    wc.sta.pmf_cfg.capable = true;
    /* With power saving (below) the radio only wakes every 5th router beacon
     * (~0.5 s) to collect waiting traffic: replies stay well under 1 s. */
    wc.sta.listen_interval = 5;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep: the radio naps between wake-ups (listen_interval above).
     * Replies can take up to ~0.5 s longer, but it saves a lot of battery. */
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    ESP_LOGI(TAG, "connecting to \"%s\"", WIFI_SSID);
}

/* Block the calling task until Wi-Fi has an IP address. Returns at once if it
 * is already connected. Waits forever if the network is never found. */
void wifi_wait_connected(void) {
    xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

/* Signal strength of the current access point in dBm (about -30 = excellent,
 * -70 = weak, -80 = barely usable); 0 when not connected. */
int wifi_rssi(void) {
    wifi_ap_record_t ap;
    if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return 0;
}

/* Start SNTP against NTP_SERVER and wait up to 15 s for the first answer.
 * Returns true if the system clock was set. SNTP keeps running afterwards and
 * re-syncs periodically on its own (ESP-IDF default: hourly). Call it once,
 * after wifi_wait_connected(). */
bool time_sync(void) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&cfg);
    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK;
    ESP_LOGI(TAG, "time sync %s", ok ? "OK" : "failed (using RTC time)");
    return ok;
}

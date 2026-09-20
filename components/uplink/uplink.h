/* Upstream link abstraction.
 *
 * The repeater's upstream side is either the native 2.4 GHz Wi-Fi STA or a
 * Morse Micro Wi-Fi HaLow transceiver (CONFIG_HALOW_UPLINK). Both present a
 * single esp_netif registered under the "WIFI_STA_DEF" ifkey, so the L2 bridge
 * (netif_hooks.c, repeater_forward.c) attaches to either one unchanged.
 *
 * With HaLow selected the native radio runs AP-only: the two sides no longer
 * share a radio, so the AP is free of the channel-lock constraint and there is
 * no WIFI_EVENT_STA_* traffic. The HaLow link state is surfaced instead as
 * UPLINK_EVENT, posted from the Morse status callback onto the default event
 * loop so the router's reconnect backoff can stay in one place.
 */

#ifndef UPLINK_H
#define UPLINK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(UPLINK_EVENT);

/* Longest SSID (32) plus terminator. */
#define UPLINK_SSID_MAXLEN 33

/* Upper bound on cached scan results, matching what the web UI renders. */
#define UPLINK_SCAN_MAX 20

/* One scan hit, or the currently associated AP. Radio-neutral: the native STA
 * and the HaLow transceiver report overlapping but not identical information,
 * so fields that do not apply are left zeroed. */
typedef struct {
    char ssid[UPLINK_SSID_MAXLEN];
    uint8_t bssid[6];
    int8_t rssi;              /* dBm */
    uint16_t channel;         /* 2.4 GHz channel, or S1G channel number */
    uint32_t freq_khz;        /* HaLow centre frequency; 0 on the native STA */
    uint8_t bw_mhz;           /* HaLow operating bandwidth; 0 on the native STA */
    bool secure;              /* true when the BSS advertises any encryption */
    wifi_auth_mode_t authmode; /* native only; WIFI_AUTH_MAX when unknown */
} uplink_ap_record_t;

enum {
    UPLINK_EVENT_CONNECTED,      /* association up; DHCP is starting */
    UPLINK_EVENT_DISCONNECTED,   /* association lost or never established */
};

/* True when the upstream link is a HaLow transceiver rather than the native
 * Wi-Fi STA. Compile-time constant; callers may branch on it freely. */
static inline bool uplink_is_halow(void)
{
#if CONFIG_HALOW_UPLINK
    return true;
#else
    return false;
#endif
}

/* --- Radio-neutral API ---------------------------------------------------
 *
 * Implemented twice, once per uplink radio. Callers outside main/ (web UI,
 * console, OLED, MQTT) use these rather than esp_wifi_* so they keep working
 * when the upstream side is not the native STA.
 */

/* Scan the upstream band. With blocking set, returns once the scan has
 * finished and the results are readable; otherwise returns immediately and the
 * results appear some seconds later. A scan takes the radio off channel, so on
 * the HaLow side it drops the association -- only call it from the scan/config
 * paths, which already suppress reconnects. */
esp_err_t uplink_scan_start(bool blocking);

/* Copy up to max cached results, strongest first. Returns the number written. */
int uplink_scan_get_results(uplink_ap_record_t *out, int max);

/* MAC address of the upstream interface. False when the radio is not ready. */
bool uplink_get_mac(uint8_t mac[6]);

/* Describe the currently associated upstream AP. False when not associated. */
bool uplink_get_ap_info(uplink_ap_record_t *out);

/* Start (or restart) an association attempt using the stored credentials. */
esp_err_t uplink_connect(void);

esp_err_t uplink_disconnect(void);

#if CONFIG_HALOW_UPLINK

/* Boot the transceiver and create its esp_netif under the "WIFI_STA_DEF"
 * ifkey. Must be called instead of esp_netif_create_default_wifi_sta(), and
 * after esp_netif_init() / esp_event_loop_create_default(). Returns NULL on
 * failure. */
esp_netif_t *uplink_halow_init(void);

/* Stage the association parameters. Security is derived from passwd: empty
 * means open, otherwise WPA3-SAE. WPA2-PSK and WPA2-Enterprise do not exist on
 * the HaLow uplink. Safe to call repeatedly; takes effect on the next connect. */
esp_err_t uplink_halow_set_config(const char *ssid, const char *passwd);

/* Begin (or retry) association. The result arrives asynchronously as
 * UPLINK_EVENT_CONNECTED / UPLINK_EVENT_DISCONNECTED. */
esp_err_t uplink_halow_connect(void);

esp_err_t uplink_halow_disconnect(void);

/* Associated and the controlled port is open. */
bool uplink_halow_is_connected(void);

/* Last known RSSI in dBm, or 0 when not associated. */
int uplink_halow_get_rssi(void);

/* MAC address of the HaLow interface. Returns false before the transceiver has
 * booted. */
bool uplink_halow_get_mac(uint8_t mac[6]);

#endif /* CONFIG_HALOW_UPLINK */

#ifdef __cplusplus
}
#endif

#endif /* UPLINK_H */

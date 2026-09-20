/* Native 2.4 GHz Wi-Fi STA upstream link (the default when CONFIG_HALOW_UPLINK
 * is off). A thin adapter over esp_wifi_* so callers outside main/ can use the
 * radio-neutral API in uplink.h. See uplink_halow.c for the HaLow side.
 */

#include "sdkconfig.h"

#if !CONFIG_HALOW_UPLINK

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "uplink.h"

static const char *TAG = "uplink_wifi";

ESP_EVENT_DEFINE_BASE(UPLINK_EVENT);

static void record_from_ap(uplink_ap_record_t *dst, const wifi_ap_record_t *src)
{
    memset(dst, 0, sizeof(*dst));
    strlcpy(dst->ssid, (const char *)src->ssid, sizeof(dst->ssid));
    memcpy(dst->bssid, src->bssid, 6);
    dst->rssi = src->rssi;
    dst->channel = src->primary;
    dst->authmode = src->authmode;
    dst->secure = (src->authmode != WIFI_AUTH_OPEN);
}

esp_err_t uplink_scan_start(bool blocking)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, blocking);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
    }
    return err;
}

int uplink_scan_get_results(uplink_ap_record_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        return 0;
    }
    if (ap_count > max) {
        ap_count = (uint16_t)max;
    }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (records == NULL) {
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        free(records);
        return 0;
    }

    for (int i = 0; i < ap_count; i++) {
        record_from_ap(&out[i], &records[i]);
    }
    free(records);
    return ap_count;
}

bool uplink_get_mac(uint8_t mac[6])
{
    return esp_wifi_get_mac(ESP_IF_WIFI_STA, mac) == ESP_OK;
}

bool uplink_get_ap_info(uplink_ap_record_t *out)
{
    wifi_ap_record_t ap;
    if (out == NULL || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    record_from_ap(out, &ap);
    return true;
}

esp_err_t uplink_connect(void)
{
    return esp_wifi_connect();
}

esp_err_t uplink_disconnect(void)
{
    return esp_wifi_disconnect();
}

#endif /* !CONFIG_HALOW_UPLINK */

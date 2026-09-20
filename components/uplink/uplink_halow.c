/* Wi-Fi HaLow upstream link (CONFIG_HALOW_UPLINK).
 *
 * Wraps the morsemicro/halow component so the rest of the firmware can drive
 * the upstream side without knowing which radio is behind it. See uplink.h.
 */

#include "sdkconfig.h"

#if CONFIG_HALOW_UPLINK

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "uplink.h"
#include "mmhalow.h"

static const char *TAG = "uplink_halow";

ESP_EVENT_DEFINE_BASE(UPLINK_EVENT);

/* Upper bound on a blocking scan; a full S1G sweep is well under this. */
#define HALOW_SCAN_TIMEOUT_MS 15000

static esp_netif_t *halow_netif = NULL;

/* Scan results are delivered one probe response at a time from the Morse scan
 * task and are only valid for the duration of the callback, so they are copied
 * into this table. Guarded by a mutex because the HTTP handler reads it from
 * its own task while the scan is still running. */
static uplink_ap_record_t scan_cache[UPLINK_SCAN_MAX];
static int scan_cache_count = 0;
static volatile bool scan_running = false;
static SemaphoreHandle_t scan_lock = NULL;

static char configured_ssid[UPLINK_SSID_MAXLEN] = {0};
static bool configured_secure = false;

/* mmwlan reports link state from its own task. Posting to the default event
 * loop moves the reconnect decision back onto the event task, where the
 * router's backoff timer and the rest of the WIFI_EVENT handling already run. */
static void halow_status_cb(enum mmwlan_sta_state state)
{
    switch (state) {
        case MMWLAN_STA_CONNECTED:
            ESP_LOGI(TAG, "HaLow associated");
            esp_event_post(UPLINK_EVENT, UPLINK_EVENT_CONNECTED, NULL, 0, 0);
            break;
        case MMWLAN_STA_CONNECTING:
            ESP_LOGI(TAG, "HaLow associating");
            break;
        case MMWLAN_STA_DISABLED:
        default:
            ESP_LOGI(TAG, "HaLow link down");
            esp_event_post(UPLINK_EVENT, UPLINK_EVENT_DISCONNECTED, NULL, 0, 0);
            break;
    }
}

static void scan_rx_cb(const struct mmwlan_scan_result *result, void *arg)
{
    (void)arg;
    if (result == NULL || result->ssid == NULL || result->ssid_len == 0) {
        return;  /* hidden or malformed; nothing useful to show */
    }

    if (xSemaphoreTake(scan_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    /* Several probe responses per BSS are normal; keep the strongest. */
    int slot = -1;
    for (int i = 0; i < scan_cache_count; i++) {
        if (result->bssid != NULL && memcmp(scan_cache[i].bssid, result->bssid, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (scan_cache_count >= UPLINK_SCAN_MAX) {
            xSemaphoreGive(scan_lock);
            return;
        }
        slot = scan_cache_count++;
    } else if (result->rssi <= scan_cache[slot].rssi) {
        xSemaphoreGive(scan_lock);
        return;
    }

    uplink_ap_record_t *rec = &scan_cache[slot];
    memset(rec, 0, sizeof(*rec));

    size_t ssid_len = result->ssid_len;
    if (ssid_len > UPLINK_SSID_MAXLEN - 1) {
        ssid_len = UPLINK_SSID_MAXLEN - 1;
    }
    memcpy(rec->ssid, result->ssid, ssid_len);
    rec->ssid[ssid_len] = '\0';

    if (result->bssid != NULL) {
        memcpy(rec->bssid, result->bssid, 6);
    }
    rec->rssi = (int8_t)result->rssi;
    rec->freq_khz = result->channel_freq_hz / 1000;
    rec->bw_mhz = result->op_bw_mhz ? result->op_bw_mhz : result->bw_mhz;
    /* Privacy bit (b4) of the Capability Information field. */
    rec->secure = (result->capability_info & 0x0010) != 0;
    rec->authmode = WIFI_AUTH_MAX;  /* SAE/OWE/open is not distinguishable here */

    xSemaphoreGive(scan_lock);
}

static void scan_complete_cb(enum mmwlan_scan_state state, void *arg)
{
    (void)arg;
    (void)state;
    scan_running = false;
    ESP_LOGI(TAG, "HaLow scan complete, %d network(s)", scan_cache_count);
}

esp_err_t uplink_scan_start(bool blocking)
{
    /* uplink_halow_init() allocates scan_lock; a failed init leaves it NULL and
     * the callbacks below would dereference it from the Morse scan task. */
    if (scan_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scan_running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(scan_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        scan_cache_count = 0;
        xSemaphoreGive(scan_lock);
    }

    /* A scan takes the radio off the operating channel, so drop the
     * association first rather than letting it time out mid-scan. The
     * router's backoff timer brings it back afterwards. */
    if (mmwlan_get_sta_state() != MMWLAN_STA_DISABLED) {
        mmhalow_disconnect();
    }

    struct mmhalow_scan_args args = {
        .rx_cb = scan_rx_cb,
        .complete_cb = scan_complete_cb,
        .cb_arg = NULL,
    };

    scan_running = true;
    esp_err_t err = mmhalow_scan(&args);
    if (err != ESP_OK) {
        scan_running = false;
        ESP_LOGW(TAG, "HaLow scan request failed: %s", esp_err_to_name(err));
        return err;
    }

    if (blocking) {
        /* mmwlan has no blocking scan, so poll the completion callback's flag.
         * The timeout only bounds a scan that never reports back; a normal
         * sweep of the S1G channel list finishes well inside it. */
        const int poll_ms = 100;
        int waited_ms = 0;
        while (scan_running && waited_ms < HALOW_SCAN_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            waited_ms += poll_ms;
        }
        if (scan_running) {
            ESP_LOGW(TAG, "HaLow scan did not complete within %d ms", HALOW_SCAN_TIMEOUT_MS);
            scan_running = false;
        }
    }
    return ESP_OK;
}

int uplink_scan_get_results(uplink_ap_record_t *out, int max)
{
    if (out == NULL || max <= 0 || scan_lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(scan_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    int count = scan_cache_count < max ? scan_cache_count : max;
    memcpy(out, scan_cache, count * sizeof(uplink_ap_record_t));
    xSemaphoreGive(scan_lock);

    /* Strongest first, as the native scan already returns them. */
    for (int i = 1; i < count; i++) {
        uplink_ap_record_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

bool uplink_get_mac(uint8_t mac[6])
{
    return uplink_halow_get_mac(mac);
}

bool uplink_get_ap_info(uplink_ap_record_t *out)
{
    if (out == NULL || !uplink_halow_is_connected()) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->ssid, configured_ssid, sizeof(out->ssid));
    out->secure = configured_secure;
    out->authmode = WIFI_AUTH_MAX;
    out->rssi = (int8_t)uplink_halow_get_rssi();

    (void)mmwlan_get_bssid(out->bssid);

    /* channel/bw_mhz are deliberately left zero. The obvious source,
     * mmwlan_get_vif_channel_info(), is compiled against umac_ap_get_channel_info(),
     * which only exists when CONFIG_HALOW_AP_MODE is enabled -- and HaLow AP
     * mode is off here because the uplink is a station. Calling it fails the
     * link rather than returning an error at runtime. */
    return true;
}

esp_err_t uplink_connect(void)
{
    return uplink_halow_connect();
}

esp_err_t uplink_disconnect(void)
{
    return uplink_halow_disconnect();
}

/* Put the transceiver and its interrupt pins into a known-quiet state before
 * the Morse stack touches them.
 *
 * Neither a software restart (esp_restart(), which is what "save and reboot"
 * in the web UI does) nor the ESP32's own reset clears the GPIO peripheral's
 * per-pin interrupt type, and neither resets the MM6108 -- RESET_N is only
 * driven low once mmhal_init() runs. So a warm boot starts with the
 * transceiver still running from the previous session, still holding its
 * active-low IRQ line asserted, and with that pin's interrupt type still
 * latched at GPIO_INTR_LOW_LEVEL from the last mmhal_wlan_set_spi_irq_enabled().
 *
 * mmhal_init() then calls gpio_install_isr_service(), which enables the GPIO
 * interrupt before any per-pin handler has been registered. gpio_isr_loop()
 * only auto-clears the status bit for edge-triggered pins, so the level
 * interrupt re-fires immediately with gpio_isr_func[].fn == NULL and CPU0
 * spins in the ISR until the interrupt watchdog panics -- which reboots into
 * exactly the same state, hence the loop.
 *
 * Clearing the latched interrupt type here makes a warm boot look like a cold
 * one, and holding RESET_N low gives the chip time to release the line. */
static void halow_quiesce_transceiver(void)
{
    gpio_config_t reset_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CONFIG_MM_RESET_N,
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&reset_conf);
    gpio_set_level(CONFIG_MM_RESET_N, 0);

    /* Both are plain register writes; neither needs the ISR service, which is
     * the point -- this has to happen before gpio_install_isr_service(). */
    gpio_set_intr_type(CONFIG_MM_SPI_IRQ, GPIO_INTR_DISABLE);
    gpio_intr_disable(CONFIG_MM_SPI_IRQ);
#if CONFIG_HALOW_PS_MODE
    gpio_set_intr_type(CONFIG_MM_BUSY, GPIO_INTR_DISABLE);
    gpio_intr_disable(CONFIG_MM_BUSY);
#endif

    /* Hold the chip in reset long enough for it to let go of the IRQ line. */
    vTaskDelay(pdMS_TO_TICKS(20));
}

esp_netif_t *uplink_halow_init(void)
{
    halow_quiesce_transceiver();

    if (scan_lock == NULL) {
        scan_lock = xSemaphoreCreateMutex();
        if (scan_lock == NULL) {
            ESP_LOGE(TAG, "failed to allocate scan mutex");
            return NULL;
        }
    }

    /* mmhalow_init() boots the transceiver, creates the esp_netif under the
     * "WIFI_STA_DEF" ifkey and starts it. Its wifi_init_config_t argument is
     * ignored by the component but the signature demands one. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = mmhalow_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    halow_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (halow_netif == NULL) {
        ESP_LOGE(TAG, "HaLow netif not registered under WIFI_STA_DEF");
        return NULL;
    }

    ESP_LOGI(TAG, "HaLow uplink ready (country %s)", CONFIG_HALOW_COUNTRY_CODE);
    return halow_netif;
}

esp_err_t uplink_halow_set_config(const char *ssid, const char *passwd)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    if (ssid_len > MMWLAN_SSID_MAXLEN) {
        ESP_LOGE(TAG, "SSID too long for HaLow (%u > %u)",
                 (unsigned)ssid_len, (unsigned)MMWLAN_SSID_MAXLEN);
        return ESP_ERR_INVALID_ARG;
    }

    mmhalow_wifi_config_t conf;
    const struct mmwlan_sta_args defaults = MMWLAN_STA_ARGS_INIT;
    memcpy(&conf.sta, &defaults, sizeof(conf.sta));

    memcpy(conf.sta.ssid, ssid, ssid_len);
    conf.sta.ssid_len = (uint16_t)ssid_len;

    /* The HaLow stack offers open, OWE and WPA3-SAE only; there is no
     * WPA2-PSK. A configured passphrase therefore always means SAE. */
    if (passwd != NULL && passwd[0] != '\0') {
        size_t pass_len = strlen(passwd);
        if (pass_len > MMWLAN_PASSPHRASE_MAXLEN) {
            ESP_LOGE(TAG, "Passphrase too long for HaLow (%u > %u)",
                     (unsigned)pass_len, (unsigned)MMWLAN_PASSPHRASE_MAXLEN);
            return ESP_ERR_INVALID_ARG;
        }
        conf.sta.security_type = MMWLAN_SAE;
        memcpy(conf.sta.passphrase, passwd, pass_len);
        conf.sta.passphrase[pass_len] = '\0';
        conf.sta.passphrase_len = (uint16_t)pass_len;
        conf.sta.pmf_mode = MMWLAN_PMF_REQUIRED;
    } else {
        conf.sta.security_type = MMWLAN_OPEN;
        conf.sta.pmf_mode = MMWLAN_PMF_DISABLED;
    }

    strlcpy(configured_ssid, ssid, sizeof(configured_ssid));
    configured_secure = (conf.sta.security_type != MMWLAN_OPEN);

    ESP_LOGI(TAG, "HaLow uplink SSID '%s' (%s)", ssid,
             conf.sta.security_type == MMWLAN_SAE ? "WPA3-SAE" : "open");

    return mmhalow_set_config(WIFI_IF_STA, &conf);
}

esp_err_t uplink_halow_connect(void)
{
    return mmhalow_connect(halow_status_cb);
}

esp_err_t uplink_halow_disconnect(void)
{
    return mmhalow_disconnect();
}

bool uplink_halow_is_connected(void)
{
    /* mmhalow_status() is declared as returning enum mmwlan_status but in fact
     * forwards mmwlan_get_sta_state(); call the latter directly so the compared
     * enum types line up. */
    return mmwlan_get_sta_state() == MMWLAN_STA_CONNECTED;
}

int uplink_halow_get_rssi(void)
{
    if (!uplink_halow_is_connected()) {
        return 0;
    }
    int32_t rssi = mmwlan_get_rssi();
    return (rssi == INT32_MIN) ? 0 : (int)rssi;
}

bool uplink_halow_get_mac(uint8_t mac[6])
{
    return mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS;
}

#endif /* CONFIG_HALOW_UPLINK */

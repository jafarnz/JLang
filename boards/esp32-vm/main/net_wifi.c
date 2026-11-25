/*
 * net_wifi.c - lightweight Wi-Fi + HTTP/HTTPS GET built-ins for esp32-vm:
 *
 *   wifi_connect(ssid, pass)  join a 2.4 GHz Wi-Fi network (returns bool)
 *   wifi_ip()                 local IPv4 address as a heap str (or nil)
 *   http_get(url)             HTTP or HTTPS GET returning a heap str (or nil)
 *
 * Credentials are never hardcoded in firmware; save them on the device with
 * kv_set("wifi_ssid", "...") and kv_set("wifi_pass", "...").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "hardware.h"
#include "jlang.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     4
#define HTTP_MAX_BODY      4096

static bool wifi_inited = false;
static bool wifi_connected = false;
static int wifi_retries = 0;
static EventGroupHandle_t wifi_eg = NULL;
static char wifi_cur_ssid[33];
static char wifi_ip_str[20];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        wifi_ip_str[0] = '\0';
        if (wifi_retries < WIFI_MAX_RETRY) {
            wifi_retries++;
            esp_wifi_connect();
        } else if (wifi_eg) {
            xEventGroupSetBits(wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(wifi_ip_str, sizeof wifi_ip_str, IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_connected = true;
        wifi_retries = 0;
        if (wifi_eg) xEventGroupSetBits(wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_ensure_init(void) {
    if (wifi_inited) return true;
#if CONFIG_IDF_TARGET_ESP32C6
    /* XIAO ESP32-C6 RF switch: GPIO 3 LOW enables switch, GPIO 14 LOW picks ceramic antenna. */
    gpio_reset_pin(3);
    gpio_set_direction(3, GPIO_MODE_OUTPUT);
    gpio_set_level(3, 0);
    gpio_reset_pin(14);
    gpio_set_direction(14, GPIO_MODE_OUTPUT);
    gpio_set_level(14, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    wifi_eg = xEventGroupCreate();
    if (!wifi_eg) return false;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
    wifi_inited = true;
    return true;
}

static bool b_wifi_connect(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *ssid, *pass;
    uint32_t sn, pn;
    if (!jl_get_string(vm, args[0], &ssid, &sn) || !jl_get_string(vm, args[1], &pass, &pn))
        return jl_error(vm, "wifi_connect(): expected (ssid, password) strings");
    if (sn == 0 || sn > 32)
        return jl_error(vm, "wifi_connect(): ssid must be 1 to 32 bytes");
    if (pn > 64)
        return jl_error(vm, "wifi_connect(): password must be at most 64 bytes");

    /* Already connected to this network: return immediately. */
    if (wifi_connected && strlen(wifi_cur_ssid) == sn && memcmp(wifi_cur_ssid, ssid, sn) == 0) {
        *out = bool_val(true);
        return true;
    }

    if (!wifi_ensure_init()) {
        *out = bool_val(false);
        return true;
    }

    esp_wifi_stop();
    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof wcfg);
    memcpy(wcfg.sta.ssid, ssid, sn);
    memcpy(wcfg.sta.password, pass, pn);
    wcfg.sta.threshold.authmode = pn > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    memcpy(wifi_cur_ssid, ssid, sn);
    wifi_cur_ssid[sn] = '\0';
    wifi_connected = false;
    wifi_retries = 0;
    wifi_ip_str[0] = '\0';
    xEventGroupClearBits(wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wcfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        *out = bool_val(false);
        return true;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    *out = bool_val((bits & WIFI_CONNECTED_BIT) != 0);
    return true;
}

static bool b_wifi_ip(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    if (!wifi_connected || wifi_ip_str[0] == '\0') {
        *out = nil_val();
        return true;
    }
    return jl_new_string(vm, wifi_ip_str, (uint32_t)strlen(wifi_ip_str), out);
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} HttpBuf;

static esp_err_t http_ev_handler(esp_http_client_event_t *evt) {
    HttpBuf *hb = (HttpBuf *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && hb && evt->data && evt->data_len > 0) {
        size_t space = hb->cap - hb->len;
        size_t take = (size_t)evt->data_len < space ? (size_t)evt->data_len : space;
        if (take > 0) {
            memcpy(hb->buf + hb->len, evt->data, take);
            hb->len += take;
        }
    }
    return ESP_OK;
}

static bool b_http_get(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *url_p;
    uint32_t un;
    if (!jl_get_string(vm, args[0], &url_p, &un))
        return jl_error(vm, "http_get(): url must be a string");
    if (un == 0 || un > 512)
        return jl_error(vm, "http_get(): url must be 1 to 512 bytes");
    if (!wifi_connected) {
        *out = nil_val();
        return true;
    }

    char url[513];
    memcpy(url, url_p, un);
    url[un] = '\0';

    char *body = (char *)malloc(HTTP_MAX_BODY);
    if (!body) {
        *out = nil_val();
        return true;
    }
    HttpBuf hb = {body, 0, HTTP_MAX_BODY};

    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.event_handler = http_ev_handler;
    cfg.user_data = &hb;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        *out = nil_val();
        return true;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        free(body);
        *out = nil_val();
        return true;
    }

    bool ok = jl_new_string(vm, body, (uint32_t)hb.len, out);
    free(body);
    return ok;
}

void net_wifi_register(JlVM *vm) {
    jl_register_native(vm, "wifi_connect", b_wifi_connect, 2);
    jl_register_native(vm, "wifi_ip", b_wifi_ip, 0);
    jl_register_native(vm, "http_get", b_http_get, 1);
}

/*
 * wifi_captive_portal.c - see wifi_captive_portal.h.
 *
 * SoftAP + DNS-redirect + HTTP config page, same shape as esp-idf's own
 * examples/protocols/http_server/captive_portal example (this project
 * vendors that example's dns_server component verbatim - see
 * components/dns_server/). Unlike the network_provisioning component
 * (protocomm-based, meant for Espressif's companion app/CLI, not a
 * browser), this answers every DNS query from a connected phone/laptop
 * with the SoftAP's own IP and redirects every unmatched HTTP path back
 * to "/", which is what makes iOS/Android/Windows auto-pop the config
 * page the moment someone joins the AP - no app required.
 */
#include "wifi_captive_portal.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h" /* must precede dns_server.h - that header uses esp_ip4_addr_t without including its own definition */
#include "dns_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "status_led.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_captive_portal";

#define WIFI_PROV_NVS_NAMESPACE "wifi_cfg"
#define WIFI_PROV_NVS_DONE_KEY  "done"

static const char s_formPageTemplate[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>LOCK_HYBRID_AI WiFi Setup</title>"
    "<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}"
    "h1{font-size:1.2em}label{display:block;margin-top:12px;font-weight:bold}"
    "input{width:100%%;box-sizing:border-box;padding:8px;margin-top:4px;font-size:1em}"
    "button{margin-top:20px;width:100%%;padding:10px;font-size:1em}"
    "p.err{color:#b00020}p.ok{color:#0a7d00}</style></head><body>"
    "<h1>LOCK_HYBRID_AI - WiFi Setup</h1>"
    "%s"
    "<form method=\"POST\" action=\"/save\">"
    "<label>WiFi SSID</label><input name=\"ssid\" maxlength=\"32\" required>"
    "<label>WiFi Password</label><input name=\"password\" type=\"password\" maxlength=\"64\">"
    "<label>Backend Server URL (optional)</label>"
    "<input name=\"backend_url\" maxlength=\"90\" placeholder=\"e.g. http://192.168.1.8:8000 - leave blank to keep current\">"
    "<label>Device API Key (optional)</label>"
    "<input name=\"backend_key\" maxlength=\"70\" placeholder=\"leave blank to keep current\">"
    "<button type=\"submit\">Save &amp; Connect</button>"
    "</form></body></html>";

static char s_formPageBuf[sizeof(s_formPageTemplate) + 256];
static httpd_handle_t s_httpServer = NULL;
static dns_server_handle_t s_dnsServer = NULL;

static void RenderFormPage(const char *statusHtml)
{
    snprintf(s_formPageBuf, sizeof(s_formPageBuf), s_formPageTemplate, statusHtml);
}

/* Minimal application/x-www-form-urlencoded decoder, in place, NUL-terminated input -> NUL-terminated output. */
static void UrlDecode(char *s)
{
    char *out = s;
    while (*s != '\0')
    {
        if (*s == '+')
        {
            *out++ = ' ';
            s++;
        }
        else if (*s == '%' && s[1] != '\0' && s[2] != '\0')
        {
            char hex[3] = {s[1], s[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        }
        else
        {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

/* Finds `key=` in a urlencoded body and copies/decodes its value into
 * `outVal` (size outValSize). Returns true if the key was present. */
static bool ExtractFormField(const char *body, const char *key, char *outVal, size_t outValSize)
{
    size_t keyLen = strlen(key);
    const char *p = body;
    while (p != NULL)
    {
        if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=')
        {
            const char *valStart = p + keyLen + 1;
            const char *valEnd = strchr(valStart, '&');
            size_t valLen = (valEnd != NULL) ? (size_t)(valEnd - valStart) : strlen(valStart);
            if (valLen >= outValSize)
            {
                valLen = outValSize - 1;
            }
            memcpy(outVal, valStart, valLen);
            outVal[valLen] = '\0';
            UrlDecode(outVal);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL)
        {
            p++;
        }
    }
    return false;
}

static esp_err_t RootGetHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET / (serving WiFi setup form)");
    RenderFormPage("<p>Enter the site's WiFi details below.</p>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, s_formPageBuf);
    return ESP_OK;
}

static esp_err_t SavePostHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /save (content_len=%d)", (int)req->content_len);
    char body[512];
    size_t toRead = (req->content_len < sizeof(body) - 1) ? req->content_len : sizeof(body) - 1;
    /* A single httpd_req_recv() call isn't guaranteed to return the whole
     * body - loop until toRead bytes are in, same as any other streamed
     * POST read (the SSID-only version of this handler got away with one
     * call because that body was always under a TCP segment; the two new
     * optional fields make that assumption less safe). */
    size_t gotTotal = 0U;
    while (gotTotal < toRead)
    {
        int n = httpd_req_recv(req, body + gotTotal, toRead - gotTotal);
        if (n <= 0)
        {
            ESP_LOGE(TAG, "POST /save: httpd_req_recv() failed/returned %d", n);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        gotTotal += (size_t)n;
    }
    body[gotTotal] = '\0';

    char ssid[33]       = {0};
    char password[65]   = {0};
    char backendUrl[91] = {0};
    char backendKey[71] = {0};
    bool haveSsid = ExtractFormField(body, "ssid", ssid, sizeof(ssid));
    (void)ExtractFormField(body, "password", password, sizeof(password));
    bool haveBackendUrl = ExtractFormField(body, "backend_url", backendUrl, sizeof(backendUrl)) && backendUrl[0] != '\0';
    bool haveBackendKey = ExtractFormField(body, "backend_key", backendKey, sizeof(backendKey)) && backendKey[0] != '\0';

    if (!haveSsid || ssid[0] == '\0')
    {
        RenderFormPage("<p class=\"err\">SSID can't be empty - try again.</p>");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, s_formPageBuf);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "received WiFi credentials for SSID '%s', saving and rebooting...", ssid);

    /* The portal runs in plain WIFI_MODE_AP (set in WIFI_CAPTIVE_PORTAL_RunForever())
     * - no STA interface exists yet, so esp_wifi_set_config(WIFI_IF_STA, ...)
     * fails with ESP_ERR_WIFI_MODE (confirmed on real hardware: this
     * ESP_ERROR_CHECK used to abort/reboot right here, every time,
     * silently discarding whatever the user just submitted). Switching to
     * APSTA first gives the STA interface config to write into; the
     * esp_restart() below moments later boots clean into STA-only mode
     * anyway, so this dual-mode state is never actually used for
     * anything. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wifiConfig = {0};
    strlcpy((char *)wifiConfig.sta.ssid, ssid, sizeof(wifiConfig.sta.ssid));
    strlcpy((char *)wifiConfig.sta.password, password, sizeof(wifiConfig.sta.password));
    /* esp_wifi persists this to NVS itself (CONFIG_ESP_WIFI_NVS_ENABLED=y) - no manual NVS write needed for the credentials themselves. */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));

    nvs_handle_t nvsHandle;
    if (nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvsHandle) == ESP_OK)
    {
        (void)nvs_set_u8(nvsHandle, WIFI_PROV_NVS_DONE_KEY, 1U);
        /* Left blank == "keep whatever's already there" (the compiled
         * Kconfig default on a first-ever setup, or a previously-saved
         * value on a re-provision) - only write when the field was
         * actually filled in, never overwrite with empty. */
        if (haveBackendUrl)
        {
            ESP_LOGI(TAG, "backend URL overridden: %s", backendUrl);
            (void)nvs_set_str(nvsHandle, "backend_url", backendUrl);
        }
        if (haveBackendKey)
        {
            ESP_LOGI(TAG, "backend API key overridden");
            (void)nvs_set_str(nvsHandle, "backend_key", backendKey);
        }
        (void)nvs_commit(nvsHandle);
        nvs_close(nvsHandle);
    }

    static const char successPage[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>Saved</title></head><body style=\"font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px\">"
        "<h1>Saved</h1><p>WiFi credentials saved. The device is rebooting and will "
        "connect to your network - this setup AP will disappear shortly.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, successPage);

    /* Give the HTTP response a moment to actually go out over the air
     * before the AP interface (and everything else) disappears under it. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK; /* unreachable */
}

/* Redirects every unmatched path back to "/" - combined with the DNS
 * server answering every lookup with our own IP, this is what makes
 * phones/laptops detect a captive portal and auto-open it. iOS/macOS
 * specifically require actual body content on the redirect response to
 * recognize it as a captive portal, not just the 30x + Location header. */
static esp_err_t NotFoundRedirectHandler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    ESP_LOGI(TAG, "404 on '%s' -> redirecting to /", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirecting to the WiFi setup page", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t s_rootUri = {.uri = "/", .method = HTTP_GET, .handler = RootGetHandler};
static const httpd_uri_t s_saveUri = {.uri = "/save", .method = HTTP_POST, .handler = SavePostHandler};

static void StartHttpServer(void)
{
    /* Captive-portal probes from every OS generate a flood of requests
     * to paths this server doesn't recognize - that's expected, not a
     * real error, so quiet the httpd component's own logging the same
     * way esp-idf's own captive_portal example does. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* esp-idf's own captive_portal example uses 13, but that assumes a
     * higher CONFIG_LWIP_MAX_SOCKETS than this project's sdkconfig has -
     * confirmed on real hardware: httpd_start() refused 13 ("max allowed
     * 7, 3 sockets used by HTTP server internally"). 4 is comfortably
     * under that for a single-device setup flow. */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    if (httpd_start(&s_httpServer, &config) == ESP_OK)
    {
        httpd_register_uri_handler(s_httpServer, &s_rootUri);
        httpd_register_uri_handler(s_httpServer, &s_saveUri);
        httpd_register_err_handler(s_httpServer, HTTPD_404_NOT_FOUND, NotFoundRedirectHandler);
    }
    else
    {
        ESP_LOGE(TAG, "httpd_start() failed - WiFi setup page will not be reachable");
    }
}

static void WifiEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT)
    {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "client " MACSTR " joined the setup AP", MAC2STR(event->mac));
    }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "client " MACSTR " left the setup AP", MAC2STR(event->mac));
    }
}

void WIFI_CAPTIVE_PORTAL_RunForever(void)
{
    STATUS_LED_Init();
    STATUS_LED_SetConfigMode(); /* solid blue for as long as the portal is up - device.noinit/esp_restart() on save is what turns it off (reboot into STA mode never re-lights it) */

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, NULL));

    wifi_config_t apConfig = {
        .ap = {
            .ssid_len      = strlen(CONFIG_LOCK_HYBRID_WIFI_PROV_SERVICE_NAME),
            .max_connection = 4,
            .authmode      = WIFI_AUTH_OPEN, /* captive portals are conventionally open APs - PoP/password only protects the SoftAP transport, not this browser-facing one */
        },
    };
    strlcpy((char *)apConfig.ap.ssid, CONFIG_LOCK_HYBRID_WIFI_PROV_SERVICE_NAME, sizeof(apConfig.ap.ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ipInfo;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ipInfo);
    char ipStr[16];
    inet_ntoa_r(ipInfo.ip.addr, ipStr, sizeof(ipStr));
    ESP_LOGI(TAG, "setup AP '%s' up at %s - connect and a WiFi setup page should open automatically",
             CONFIG_LOCK_HYBRID_WIFI_PROV_SERVICE_NAME, ipStr);

    StartHttpServer();

    dns_server_config_t dnsConfig = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dnsServer = start_dns_server(&dnsConfig);

    /* Nothing else to do here - SavePostHandler() reboots the device once
     * real credentials arrive, which is this function's only exit. */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

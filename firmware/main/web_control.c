#include "web_control.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "custom_sim.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "lwip/lwip_napt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "panel_config.h"
#include "rgb.h"
#include "sim_manager.h"

#define AP_SSID "LED-Matrix"
#define AP_PASS "12345678"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4
#define WIFI_NVS_NS "wifi_sta"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"
#define CHAT_API_URL "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#define CHAT_MAX_REQUEST_BODY 32768
#define CHAT_MAX_API_KEY_LEN 192

static const char* TAG = "web_control";

static httpd_handle_t s_server = NULL;
static bool s_sta_connected = false;
static char s_sta_ssid[33] = {0};
static char s_sta_ip[16] = {0};
static char s_sta_pass[65] = {0};
static bool s_wifi_only_mode = false;
static sim_mode_t s_resume_mode = SIM_MODE_WATER;
static esp_netif_t* s_ap_netif = NULL;
static esp_netif_t* s_sta_netif = NULL;
static bool s_ap_dhcp_dns_offer_ready = false;

static void apply_wifi_only_mode(bool enable) {
    if (enable == s_wifi_only_mode) {
        return;
    }

    if (enable) {
        sim_mode_t cur = sim_manager_current();
        if (cur != SIM_MODE_NONE) {
            s_resume_mode = cur;
        }

        esp_err_t err = sim_manager_stop();
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "sim_manager_stop failed: %s", esp_err_to_name(err));
        }

        rgb_clear();
        rgb_show();
        s_wifi_only_mode = true;
        ESP_LOGI(TAG, "wifi-only mode enabled");
        return;
    }

    s_wifi_only_mode = false;
    if (sim_manager_current() == SIM_MODE_NONE) {
        sim_mode_t resume = (s_resume_mode == SIM_MODE_NONE) ? SIM_MODE_WATER : s_resume_mode;
        esp_err_t err = sim_manager_start(resume);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "resume sim mode failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "wifi-only mode disabled");
}

static void set_nat_enabled(bool enabled) {
#if CONFIG_LWIP_IPV4_NAPT
    if (!s_ap_netif) {
        return;
    }

    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) != ESP_OK) {
        return;
    }

    ip_napt_enable(ap_ip.ip.addr, enabled ? 1 : 0);
    ESP_LOGI(TAG, "NAT %s on AP ip: " IPSTR, enabled ? "enabled" : "disabled", IP2STR(&ap_ip.ip));
#else
    (void)enabled;
    ESP_LOGW(TAG, "CONFIG_LWIP_IPV4_NAPT disabled, forwarding unavailable");
#endif
}

static void ensure_ap_dhcp_offers_dns(void) {
    if (!s_ap_netif || s_ap_dhcp_dns_offer_ready) {
        return;
    }

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "stop AP DHCP server failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t offer_dns = 1;
    err = esp_netif_dhcps_option(
        s_ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_DOMAIN_NAME_SERVER,
        &offer_dns,
        sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enable AP DHCP DNS offer failed: %s", esp_err_to_name(err));
    }

    esp_err_t err2 = esp_netif_dhcps_start(s_ap_netif);
    if (err2 != ESP_OK && err2 != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "start AP DHCP server failed: %s", esp_err_to_name(err2));
        return;
    }

    if (err == ESP_OK) {
        s_ap_dhcp_dns_offer_ready = true;
    }
}

static void sync_ap_dns_from_sta(void) {
    if (!s_ap_netif || !s_sta_netif) {
        return;
    }

    esp_netif_dns_info_t dns_main = {0};
    esp_err_t err = esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    if (err != ESP_OK || dns_main.ip.type != ESP_IPADDR_TYPE_V4 || dns_main.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "STA DNS unavailable, skip AP DHCP DNS sync");
        return;
    }

    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set AP DNS failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "AP DHCP DNS synced to: " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));
}

static esp_err_t save_sta_credentials(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t load_sta_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len, bool* has_data) {
    if (!ssid || !pass || !has_data) {
        return ESP_ERR_INVALID_ARG;
    }

    *has_data = false;
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, WIFI_NVS_KEY_PASS, pass, &pass_size);
    }

    nvs_close(handle);

    if (err == ESP_OK && ssid[0] != '\0') {
        *has_data = true;
    }
    return err;
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

static void url_decode_inplace(char* s) {
    char* src = s;
    char* dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = hex_to_int(src[1]);
            int lo = hex_to_int(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool parse_form_value(const char* body, const char* key, char* out, size_t out_len) {
    size_t key_len = strlen(key);
    const char* p = body;

    while (p && *p) {
        const char* amp = strchr(p, '&');
        size_t token_len = amp ? (size_t)(amp - p) : strlen(p);

        if (token_len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t value_len = token_len - key_len - 1;
            if (value_len >= out_len) {
                value_len = out_len - 1;
            }
            memcpy(out, p + key_len + 1, value_len);
            out[value_len] = '\0';
            url_decode_inplace(out);
            return true;
        }

        p = amp ? (amp + 1) : NULL;
    }

    return false;
}

static bool parse_form_bool(const char* body, const char* key, bool* out) {
    if (!body || !key || !out) {
        return false;
    }

    char raw[16] = {0};
    if (!parse_form_value(body, key, raw, sizeof(raw))) {
        return false;
    }

    for (char* p = raw; *p; ++p) {
        *p = (char)tolower((unsigned char)*p);
    }

    if (strcmp(raw, "1") == 0 || strcmp(raw, "true") == 0 || strcmp(raw, "on") == 0) {
        *out = true;
    } else {
        *out = false;
    }
    return true;
}

static void trim_ascii_spaces(char* s) {
    if (!s) {
        return;
    }

    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && isspace((unsigned char)s[start])) {
        start++;
    }
    size_t end = n;
    while (end > start && isspace((unsigned char)s[end - 1])) {
        end--;
    }

    if (start > 0) {
        memmove(s, s + start, end - start);
    }
    s[end - start] = '\0';
}

static bool read_header_value(httpd_req_t* req, const char* name, char* out, size_t out_len) {
    if (!req || !name || !out || out_len == 0) {
        return false;
    }

    size_t value_len = httpd_req_get_hdr_value_len(req, name);
    if (value_len == 0 || value_len >= out_len) {
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, name, out, out_len) != ESP_OK) {
        return false;
    }

    trim_ascii_spaces(out);
    return out[0] != '\0';
}

static esp_err_t read_request_body(httpd_req_t* req, size_t max_len, char** out_body, size_t* out_len) {
    if (!req || !out_body || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    *out_len = 0;

    int content_len = req->content_len;
    if (content_len <= 0 || (size_t)content_len > max_len) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "payload too large");
        return ESP_ERR_INVALID_SIZE;
    }

    char* body = (char*)malloc((size_t)content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_ERR_NO_MEM;
    }

    int offset = 0;
    while (offset < content_len) {
        int got = httpd_req_recv(req, body + offset, content_len - offset);
        if (got <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        offset += got;
    }

    body[content_len] = '\0';
    *out_body = body;
    *out_len = (size_t)content_len;
    return ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t* e = (const wifi_event_sta_disconnected_t*)event_data;
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        set_nat_enabled(false);
        ESP_LOGW(TAG, "STA disconnected, reason=%d", e ? (int)e->reason : -1);
        if (s_sta_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
    }
}

static esp_err_t chat_post_handler(httpd_req_t* req) {
    char api_key[CHAT_MAX_API_KEY_LEN] = {0};
    if (!read_header_value(req, "X-Zhipu-Api-Key", api_key, sizeof(api_key))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing api key");
        return ESP_OK;
    }

    char* body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_request_body(req, CHAT_MAX_REQUEST_BODY, &body, &body_len);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "chat request: %.*s", (int)((body_len > 256) ? 256 : body_len), body);

    esp_http_client_config_t config = {
        .url = CHAT_API_URL,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 480000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "client init failed");
        return ESP_OK;
    }

    char auth[CHAT_MAX_API_KEY_LEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);

    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chat open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream open failed");
        return ESP_OK;
    }

    int written = 0;
    while (written < (int)body_len) {
        int w = esp_http_client_write(client, body + written, (int)body_len - written);
        if (w < 0) {
            ESP_LOGW(TAG, "chat write failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream write failed");
            return ESP_OK;
        }
        written += w;
    }

    int header_len = esp_http_client_fetch_headers(client);
    if (header_len < 0) {
        ESP_LOGW(TAG, "chat fetch headers failed, continue read body: %s",
                 esp_err_to_name((esp_err_t)header_len));
    }

    int status = esp_http_client_get_status_code(client);
    httpd_resp_set_type(req, "application/json");
    bool sent_any = false;
    char upstream_preview[384] = {0};
    size_t upstream_preview_len = 0;

    if (status > 0 && (status < 200 || status >= 300)) {
        char resp[160];
        if (status == 429) {
            ESP_LOGW(TAG, "chat upstream rate limited (429)");
            snprintf(resp, sizeof(resp),
                     "{\"error\":\"rate_limited\",\"message\":\"upstream status 429\"}");
            httpd_resp_set_status(req, "429 Too Many Requests");
        } else {
            snprintf(resp, sizeof(resp), "{\"error\":\"upstream status %d\"}", status);
            httpd_resp_set_status(req, "502 Bad Gateway");
        }
        httpd_resp_sendstr(req, resp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(body);
        return ESP_OK;
    }

    char chunk[1024];
    while (1) {
        int read = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read < 0) {
            ESP_LOGW(TAG, "chat read failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(body);
            if (!sent_any) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream read failed");
            }
            return ESP_OK;
        }
        if (read == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, read) != ESP_OK) {
            ESP_LOGW(TAG, "chat send chunk failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            free(body);
            return ESP_OK;
        }
        if (upstream_preview_len < sizeof(upstream_preview) - 1) {
            size_t cap = (sizeof(upstream_preview) - 1) - upstream_preview_len;
            size_t cp = ((size_t)read < cap) ? (size_t)read : cap;
            memcpy(upstream_preview + upstream_preview_len, chunk, cp);
            upstream_preview_len += cp;
            upstream_preview[upstream_preview_len] = '\0';
        }
        sent_any = true;
    }

    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "chat upstream status=%d preview=%s", status, upstream_preview);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return ESP_OK;
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    (void)arg;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t* event = (const ip_event_got_ip_t*)event_data;
        s_sta_connected = true;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        sync_ap_dns_from_sta();
        set_nat_enabled(true);
        if (s_sta_ssid[0] != '\0') {
            esp_err_t err = save_sta_credentials(s_sta_ssid, s_sta_pass);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "save_sta_credentials failed: %s", esp_err_to_name(err));
            }
        }
        ESP_LOGI(TAG, "STA got ip: %s", s_sta_ip);
    }
}

static const char s_index_html[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LED Matrix Control</title>"
    "<style>"
    ":root{--bg:#f4efe4;--ink:#1f2a31;--card:#fffdf8;--line:#d6cab8;--active:#e66f3c;--btn:#2f5d50;}"
    "body{margin:0;font-family:'Trebuchet MS','Segoe UI',sans-serif;background:radial-gradient(circle at 20% 10%,#fff8e8,transparent 45%),linear-gradient(135deg,#f4efe4,#efe4d0);color:var(--ink);}"
    "main{max-width:760px;margin:20px auto;padding:16px;}"
    "h1{margin:0 0 12px;font-size:26px;letter-spacing:.4px;}"
    ".card{background:var(--card);border:2px solid var(--line);border-radius:14px;padding:14px;box-shadow:0 8px 24px rgba(59,45,25,.12);}"
    ".modes{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:12px;}"
    "button{border:0;border-radius:10px;padding:10px 14px;background:var(--btn);color:#fff;font-weight:700;cursor:pointer;}"
    "button.alt{background:#8a6348;}"
    "button.active{background:var(--active);}" 
    "#grid{display:grid;grid-template-columns:repeat(8,1fr);gap:7px;margin:10px 0 12px;}"
    ".cell{aspect-ratio:1/1;border-radius:6px;border:1px solid #cab69e;background:#f8f2e8;transition:.1s;}"
    ".cell.on{background:linear-gradient(135deg,#ffcf70,#ff7a3f);box-shadow:0 0 12px rgba(255,122,63,.6);}" 
    ".color-wrap{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:end;margin-bottom:12px;}"
    ".row{display:flex;align-items:center;gap:8px;margin:6px 0;}"
    ".row label{width:26px;font-weight:700;}"
    ".row input[type='text'],.row input[type='password'],.row input[type='color']{width:100%;padding:7px 8px;border:1px solid #cdbca4;border-radius:8px;background:#fffaf1;}"
    ".checkline{display:flex;align-items:center;gap:8px;font-weight:700;color:#364148;}"
    ".checkline input{width:auto;}"
    ".chat-card{margin-top:14px;padding-top:14px;border-top:1px solid #ded2c1;display:grid;gap:10px;}"
    ".chat-head{display:flex;justify-content:space-between;align-items:center;gap:8px;font-weight:700;font-size:15px;}"
    ".chat-meta{font-size:12px;color:#6b706b;}"
    ".chat-actions{display:flex;gap:8px;flex-wrap:wrap;}"
    ".chat-log{height:260px;overflow:auto;border:1px solid #cdbca4;border-radius:12px;padding:12px;background:#fffaf1;display:flex;flex-direction:column;gap:10px;}"
    ".bubble{max-width:86%;padding:10px 12px;border-radius:14px;line-height:1.45;white-space:pre-wrap;word-break:break-word;}"
    ".bubble.user{align-self:flex-end;background:linear-gradient(135deg,#2f5d50,#4c8b73);color:#fff;}"
    ".bubble.assistant{align-self:flex-start;background:#fff;border:1px solid #e0d3c0;}"
    ".bubble.system{align-self:center;background:#eef0ed;color:#52615a;font-size:12px;}"
    ".chat-input{min-height:92px;resize:vertical;padding:10px 12px;border:1px solid #cdbca4;border-radius:12px;background:#fffaf1;font:inherit;line-height:1.45;}"
    ".chat-send{background:var(--active);}"
    ".chat-tip{font-size:12px;color:#68737b;line-height:1.4;}"
    "#preview{height:82px;border-radius:10px;border:2px solid #d8c8b0;background:#fff;box-shadow:inset 0 0 0 1px rgba(255,255,255,.6);}"
    "#msg{min-height:22px;font-size:14px;color:#4f5b63;}"
    "</style></head><body><main><h1>ESP32 LED Matrix</h1><div class='card'>"
    "<div class='modes'>"
    "<button id='mode-fire' class='alt'>Fire</button>"
    "<button id='mode-water' class='alt'>Fluid</button>"
    "<button id='mode-custom' class='alt'>Custom</button>"
    "</div>"
    "<div class='color-wrap'>"
    "<div>"
    "<div class='row'><label style='width:68px'>SSID</label><input id='wifi-ssid' type='text' placeholder='router ssid'></div>"
    "<div class='row'><label style='width:68px'>PASS</label><input id='wifi-pass' type='password' placeholder='router password'></div>"
    "<div class='row'><label style='width:68px'>Mode</label><label class='checkline'><input id='wifi-only' type='checkbox'>WiFi only</label></div>"
    "</div>"
    "<div style='display:flex;flex-direction:column;gap:8px'>"
    "<button id='wifi-connect' class='alt'>Connect STA</button>"
    "<div id='wifi-status' style='font-size:13px;color:#4f5b63;line-height:1.4'>STA: idle</div>"
    "</div>"
    "</div>"
    "<div class='color-wrap'>"
    "<div>"
    "<div class='row'><label style='width:68px'>Color</label><input id='color-picker' type='color' value='#ffffff' style='height:42px;padding:4px'></div>"
    "</div>"
    "<div id='preview'></div>"
    "</div>"
    "<button id='apply-color' class='alt'>Apply Color</button>"
    "<div id='grid'></div>"
    "<button id='apply'>Confirm Pattern</button>"
    "<div class='chat-card'>"
    "<div class='chat-head'><div>AI Chat</div><div class='chat-meta'>glm-4.7-flash via device proxy</div></div>"
    "<div class='row'><label style='width:68px'>Key</label><input id='chat-api-key' type='password' placeholder='your-api-key'></div>"
    "<div class='chat-actions'>"
    "<button id='chat-save-key' class='alt' type='button'>Save Key</button>"
    "<button id='chat-clear' class='alt' type='button'>Clear Chat</button>"
    "</div>"
    "<div id='chat-log' class='chat-log' aria-live='polite'></div>"
    "<textarea id='chat-input' class='chat-input' placeholder='输入消息，Enter 发送，Shift+Enter 换行'></textarea>"
    "<div class='chat-actions' style='justify-content:space-between;align-items:center'>"
    "<div class='chat-tip'>消息会先发送到设备，再由设备代理到智谱开放平台。</div>"
    "<button id='chat-send' class='chat-send' type='button'>发送</button>"
    "</div>"
    "</div>"
    "<div id='msg'></div></div></main>"
    "<script>"
    "const W=8,H=8;const state={mode:'water',grid:Array(W*H).fill(0),r8:255,g8:255,b8:255,color:'#ffffff',sta_connected:false,sta_ssid:'',sta_ip:'',wifi_only:false};"
    "const chatState={messages:[],busy:false};const chatStorageKey='glm_api_key';const chatModel='glm-4.7-flash';const chatMaxHistory=16;"
    "const chatSystemPrompt='你是ESP32灯板内置AI。仅当用户明确问你是谁/你的身份时，才回答“我是ESP32灯板内置AI”。你可以调整灯的颜色、切换模式（fire/water/custom）、使用8x8画板自动绘画。请不要输出markdown代码块。若需要驱动灯板，请输出严格JSON，格式为{\"reply\":\"给用户看的自然语言回答\",\"control\":{\"mode\":\"fire|water|custom\",\"color\":\"#RRGGBB\",\"bitmap\":\"64位0/1字符串\"}}。模式切换指令仅在用户明确提到颜色时再返回color。没有控制动作时control设为null。';"
    "const APPLE_BITMAP='0001100000111100011111101111111111111111011111100011110000011000';"
        "function setGridEnabled(enabled){document.querySelectorAll('#grid .cell').forEach(el=>{el.disabled=!enabled;});document.getElementById('grid').style.opacity=enabled?'1':'0.45';}"
        "function updateWifiOnlyUI(){const on=!!state.wifi_only;document.getElementById('wifi-only').checked=on;['mode-fire','mode-water','mode-custom','apply','apply-color'].forEach(id=>{const el=document.getElementById(id);if(el)el.disabled=on;});setGridEnabled(!on);}"
    "const msg=t=>document.getElementById('msg').textContent=t;"
    "const clamp8=v=>Math.max(0,Math.min(255,Number(v)||0));"
    "const toHex=v=>v.toString(16).padStart(2,'0');"
    "function syncColorHex(){state.color='#'+toHex(state.r8)+toHex(state.g8)+toHex(state.b8);}"
    "function hexToRgb(hex){const m=/^#?([a-fA-F0-9]{6})$/.exec(hex||'');if(!m)return null;const s=m[1];return {r:parseInt(s.slice(0,2),16),g:parseInt(s.slice(2,4),16),b:parseInt(s.slice(4,6),16)};}"
    "function updateWifiStatus(){const el=document.getElementById('wifi-status');if(state.sta_connected){el.textContent='STA: connected to '+(state.sta_ssid||'?')+' , IP='+ (state.sta_ip||'?');}else if(state.sta_ssid){el.textContent='STA: connecting/disconnected ('+state.sta_ssid+')';}else{el.textContent='STA: idle';}}"
    "function syncColorInputs(){syncColorHex();document.getElementById('color-picker').value=state.color;}"
    "function updatePreview(){const r=state.r8,g=state.g8,b=state.b8;document.getElementById('preview').style.background='linear-gradient(135deg, rgb('+r+','+g+','+b+'), rgb('+Math.min(255,r+30)+','+Math.min(255,g+30)+','+Math.min(255,b+30)+'))';}"
    "function readColorInputs(){const rgb=hexToRgb(document.getElementById('color-picker').value);if(!rgb)return;state.r8=clamp8(rgb.r);state.g8=clamp8(rgb.g);state.b8=clamp8(rgb.b);syncColorInputs();updatePreview();}"
    "function setModeBtn(){['fire','water','custom'].forEach(m=>{const b=document.getElementById('mode-'+m);b.classList.toggle('active',state.mode===m);});}"
    "function makeGrid(){const g=document.getElementById('grid');g.innerHTML='';for(let y=0;y<H;y++){for(let x=0;x<W;x++){const i=y*W+x;const d=document.createElement('button');d.type='button';d.className='cell'+(state.grid[i]?' on':'');d.onclick=()=>{if(state.wifi_only)return;state.grid[i]=state.grid[i]?0:1;d.className='cell'+(state.grid[i]?' on':'');};g.appendChild(d);}}setGridEnabled(!state.wifi_only);}"
    "function loadChatKey(){try{const saved=localStorage.getItem(chatStorageKey)||'';if(saved){document.getElementById('chat-api-key').value=saved;}}catch(e){}}"
    "function saveChatKey(){const key=document.getElementById('chat-api-key').value.trim();try{if(key){localStorage.setItem(chatStorageKey,key);}else{localStorage.removeItem(chatStorageKey);}}catch(e){}msg(key?'api key saved':'api key cleared');}"
    "function appendChatBubble(role,text){const log=document.getElementById('chat-log');const item=document.createElement('div');item.className='bubble '+role;item.textContent=text;log.appendChild(item);log.scrollTop=log.scrollHeight;return item;}"
    "function pruneChatHistory(){if(chatState.messages.length>chatMaxHistory){chatState.messages.splice(0,chatState.messages.length-chatMaxHistory);}}"
    "function initChatLog(){const log=document.getElementById('chat-log');log.innerHTML='';appendChatBubble('system','输入 API Key 后即可对话。');}"
    "function clearChat(){chatState.messages=[];initChatLog();msg('chat cleared');}"
    "function extractFirstJsonObject(text){let start=-1;let depth=0;let inStr=false;let esc=false;for(let i=0;i<text.length;i++){const ch=text[i];if(inStr){if(esc){esc=false;}else if(ch==='\\\\'){esc=true;}else if(ch==='\"'){inStr=false;}continue;}if(ch==='\"'){inStr=true;continue;}if(ch==='{'){if(depth===0)start=i;depth++;continue;}if(ch==='}'){if(depth>0){depth--;if(depth===0&&start>=0){return text.slice(start,i+1);}}}}return '';}"
    "function stripMarkdownJsonFences(text){return String(text||'').replace(/```json[\\s\\S]*?```/gi,'').replace(/```[\\s\\S]*?```/g,'').trim();}"
    "function parseAssistantReply(content){const text=(content||'').trim();if(!text){return {reply:'',control:null};}const jsonText=extractFirstJsonObject(text);if(jsonText){try{const data=JSON.parse(jsonText);if(data&&typeof data==='object'){const prefix=stripMarkdownJsonFences(text.replace(jsonText,'')).trim();const bodyReply=stripMarkdownJsonFences(typeof data.reply==='string'?data.reply:'').trim();const control=(data.control&&typeof data.control==='object')?data.control:null;const reply=[prefix,bodyReply].filter(Boolean).join(prefix&&bodyReply?'\\n':'');return {reply:reply||stripMarkdownJsonFences(text),control};}}catch(e){}}try{const data=JSON.parse(text);if(data&&typeof data==='object'){const reply=stripMarkdownJsonFences(typeof data.reply==='string'?data.reply:'').trim();const control=(data.control&&typeof data.control==='object')?data.control:null;return {reply:reply||stripMarkdownJsonFences(text),control};}}catch(e){}return {reply:stripMarkdownJsonFences(text),control:null};}"
    "function hexColorToRgb(hex){const m=/^#?([a-fA-F0-9]{6})$/.exec((hex||'').trim());if(!m)return null;const s=m[1];return {r:parseInt(s.slice(0,2),16),g:parseInt(s.slice(2,4),16),b:parseInt(s.slice(4,6),16)};}"
    "function normalizeBitmap(bitmap){const bits=String(bitmap||'').replace(/[^01]/g,'');if(bits.length>=W*H){return bits.slice(0,W*H);}if(bits.length===0){return '';}return (bits+'0'.repeat(W*H)).slice(0,W*H);}"
    "function bitmapToGrid(bitmap){const norm=normalizeBitmap(bitmap);if(!norm||norm.length!==W*H)return null;const grid=Array(W*H).fill(0);for(let i=0;i<grid.length;i++){grid[i]=(norm[i]==='1')?1:0;}return grid;}"
    "async function setMode(mode,fromAi){try{const headers=fromAi?{'X-Chat-Control':'1'}:{};const r=await fetch('/api/mode?value='+encodeURIComponent(mode),{method:'POST',headers});if(!r.ok)throw 0;state.mode=mode;setModeBtn();msg('mode -> '+mode);return true;}catch(e){msg('set mode failed');return false;}}"
    "async function applyColor(fromAi){readColorInputs();const headers=fromAi?{'X-Chat-Control':'1','Content-Type':'application/x-www-form-urlencoded'}:{'Content-Type':'application/x-www-form-urlencoded'};const body='color='+encodeURIComponent(state.color);try{const r=await fetch('/api/color',{method:'POST',headers,body});if(!r.ok)throw 0;msg('color applied to all modes');return true;}catch(e){msg('apply color failed');return false;}}"
    "async function applyPattern(fromAi){const bitmap=state.grid.map(v=>v?'1':'0').join('');const headers=fromAi?{'X-Chat-Control':'1','Content-Type':'application/x-www-form-urlencoded'}:{'Content-Type':'application/x-www-form-urlencoded'};try{const r=await fetch('/api/custom',{method:'POST',headers,body:'bitmap='+bitmap});if(!r.ok)throw 0;msg('pattern applied');return true;}catch(e){msg('apply failed');return false;}}"
    "function inferControlFromText(userText,assistantReply){const u=String(userText||'');const a=String(assistantReply||'');if(/苹果/.test(u)||/苹果/.test(a)){return {mode:'custom',color:'#FF2D2D',bitmap:APPLE_BITMAP};}if(/火焰|fire/i.test(u)||/火焰|fire/i.test(a)){return {mode:'fire'};}if(/流水|水流|water|fluid/i.test(u)||/流水|水流|water|fluid/i.test(a)){return {mode:'water'};}return null;}"
    "async function applyAiControl(control,userText){if(!control||typeof control!=='object'){return;}const requestedText=String(userText||'');const wantsColor=/(颜色|色彩|color|rgb|#)/i.test(requestedText);const hasBitmap=typeof control.bitmap==='string'&&control.bitmap.trim().length>0;if(control.mode){await setMode(String(control.mode).trim(),true);}else if(hasBitmap){await setMode('custom',true);}if(control.color&&(wantsColor||hasBitmap)){const rgb=hexColorToRgb(control.color);if(rgb){state.r8=clamp8(rgb.r);state.g8=clamp8(rgb.g);state.b8=clamp8(rgb.b);syncColorInputs();updatePreview();await applyColor(true);}}if(hasBitmap){const grid=bitmapToGrid(String(control.bitmap).trim());if(grid){state.grid=grid;makeGrid();await applyPattern(true);}else{appendChatBubble('system','AI位图无效，已忽略。');}}}"
    "async function sendChat(){if(chatState.busy)return;const input=document.getElementById('chat-input');const apiKey=document.getElementById('chat-api-key').value.trim();const text=input.value.trim();if(!apiKey){msg('api key required');return;}if(!text){msg('message required');return;}try{localStorage.setItem(chatStorageKey,apiKey);}catch(e){}chatState.busy=true;document.getElementById('chat-send').disabled=true;input.disabled=true;document.getElementById('chat-save-key').disabled=true;chatState.messages.push({role:'user',content:text});pruneChatHistory();appendChatBubble('user',text);input.value='';msg('sending...');try{const payload={model:chatModel,messages:[{role:'system',content:chatSystemPrompt},...chatState.messages],thinking:{type:'enabled'},max_tokens:65536,temperature:1.0};const r=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json','X-Zhipu-Api-Key':apiKey},body:JSON.stringify(payload)});const raw=await r.text();if(!r.ok)throw new Error(raw||('HTTP '+r.status));const j=JSON.parse(raw);const answer=j&&j.choices&&j.choices[0]&&j.choices[0].message&&j.choices[0].message.content?String(j.choices[0].message.content):'';if(!answer)throw new Error('empty assistant response');const parsed=parseAssistantReply(answer);chatState.messages.push({role:'assistant',content:parsed.reply});pruneChatHistory();appendChatBubble('assistant',parsed.reply);const inferred=inferControlFromText(text,parsed.reply);const ctl=parsed.control||inferred;if(ctl){await applyAiControl(ctl,text);}msg('done');}catch(e){appendChatBubble('system','请求失败：'+(e&&e.message?e.message:'unknown'));msg('chat failed');}finally{chatState.busy=false;document.getElementById('chat-send').disabled=false;input.disabled=false;document.getElementById('chat-save-key').disabled=false;input.focus();}}"
    "async function loadState(){try{const r=await fetch('/api/state');const j=await r.json();state.mode=j.mode||state.mode;if(j.r8!==undefined)state.r8=clamp8(j.r8);if(j.g8!==undefined)state.g8=clamp8(j.g8);if(j.b8!==undefined)state.b8=clamp8(j.b8);state.sta_connected=!!j.sta_connected;state.sta_ssid=j.sta_ssid||'';state.sta_ip=j.sta_ip||'';state.wifi_only=!!j.wifi_only;setModeBtn();syncColorInputs();updatePreview();updateWifiStatus();updateWifiOnlyUI();if(state.sta_ssid){document.getElementById('wifi-ssid').value=state.sta_ssid;}}catch(e){msg('state load failed');}}"
    "async function setWifiOnly(on){const body='wifi_only='+(on?'1':'0');try{const r=await fetch('/api/wifi/only',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw 0;state.wifi_only=on;updateWifiOnlyUI();msg(on?'wifi only enabled':'wifi only disabled');setTimeout(loadState,500);}catch(e){msg('set wifi only failed');document.getElementById('wifi-only').checked=state.wifi_only;}}"
    "async function connectWifi(){const ssid=document.getElementById('wifi-ssid').value.trim();const pass=document.getElementById('wifi-pass').value;const wifiOnly=document.getElementById('wifi-only').checked;if(!ssid){msg('ssid required');return;}const body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)+'&wifi_only='+(wifiOnly?'1':'0');try{const r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw 0;state.sta_ssid=ssid;state.sta_connected=false;state.sta_ip='';state.wifi_only=wifiOnly;updateWifiStatus();updateWifiOnlyUI();msg('wifi connecting...');setTimeout(loadState,2000);}catch(e){msg('connect failed');}}"
    "document.getElementById('mode-fire').onclick=()=>setMode('fire');"
    "document.getElementById('mode-water').onclick=()=>setMode('water');"
    "document.getElementById('mode-custom').onclick=()=>setMode('custom');"
    "document.getElementById('wifi-connect').onclick=connectWifi;"
    "document.getElementById('wifi-only').onchange=e=>setWifiOnly(!!e.target.checked);"
    "document.getElementById('chat-save-key').onclick=saveChatKey;"
    "document.getElementById('chat-clear').onclick=clearChat;"
    "document.getElementById('chat-send').onclick=sendChat;"
    "document.getElementById('chat-api-key').addEventListener('change',saveChatKey);"
    "document.getElementById('chat-input').addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendChat();}});"
    "document.getElementById('color-picker').addEventListener('input',()=>{readColorInputs();});"
    "document.getElementById('apply-color').onclick=applyColor;"
    "document.getElementById('apply').onclick=applyPattern;"
    "makeGrid();syncColorInputs();updatePreview();updateWifiStatus();loadChatKey();initChatLog();loadState();"
    "</script></body></html>";

static const char* mode_to_str(sim_mode_t mode) {
    switch (mode) {
        case SIM_MODE_FIRE:
            return "fire";
        case SIM_MODE_WATER:
            return "water";
        case SIM_MODE_CUSTOM:
            return "custom";
        default:
            return "none";
    }
}

static bool str_to_mode(const char* s, sim_mode_t* out_mode) {
    if (!s || !out_mode) {
        return false;
    }
    if (strcmp(s, "fire") == 0) {
        *out_mode = SIM_MODE_FIRE;
        return true;
    }
    if (strcmp(s, "water") == 0 || strcmp(s, "fluid") == 0) {
        *out_mode = SIM_MODE_WATER;
        return true;
    }
    if (strcmp(s, "custom") == 0) {
        *out_mode = SIM_MODE_CUSTOM;
        return true;
    }
    return false;
}

static esp_err_t root_get_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t state_get_handler(httpd_req_t* req) {
    char resp[224];
    const char* mode = mode_to_str(sim_manager_current());
    uint8_t r8 = 0;
    uint8_t g8 = 0;
    uint8_t b8 = 0;
    rgb_get_global_color8(&r8, &g8, &b8);
    snprintf(resp, sizeof(resp),
             "{\"mode\":\"%s\",\"r8\":%u,\"g8\":%u,\"b8\":%u,\"sta_connected\":%s,\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"wifi_only\":%s}",
             mode, (unsigned)r8, (unsigned)g8, (unsigned)b8,
             s_sta_connected ? "true" : "false", s_sta_ssid, s_sta_ip,
             s_wifi_only_mode ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t chat_post_handler(httpd_req_t* req);

static bool is_chat_control_request(httpd_req_t* req) {
    char value[8] = {0};
    return read_header_value(req, "X-Chat-Control", value, sizeof(value));
}

static esp_err_t mode_post_handler(httpd_req_t* req) {
    bool from_ai = is_chat_control_request(req);
    if (s_wifi_only_mode && !from_ai) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi-only enabled");
        return ESP_OK;
    }

    char query[64] = {0};
    char value[16] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing value");
        return ESP_OK;
    }

    sim_mode_t mode = SIM_MODE_NONE;
    if (!str_to_mode(value, &mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
        return ESP_OK;
    }
    if (from_ai) {
        ESP_LOGI(TAG, "ai control mode=%s", value);
    }

    esp_err_t err = sim_manager_switch(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "switch mode failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "switch failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t custom_post_handler(httpd_req_t* req) {
    bool from_ai = is_chat_control_request(req);
    if (s_wifi_only_mode && !from_ai) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi-only enabled");
        return ESP_OK;
    }

    char body[128] = {0};
    int remain = req->content_len;
    if (remain <= 0 || remain >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    int offset = 0;
    while (remain > 0) {
        int got = httpd_req_recv(req, body + offset, remain);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        remain -= got;
        offset += got;
    }
    body[offset] = '\0';

    const char* prefix = "bitmap=";
    if (strncmp(body, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing bitmap");
        return ESP_OK;
    }

    const char* bitmap_str = body + strlen(prefix);
    if (from_ai) {
        ESP_LOGI(TAG, "ai control bitmap len=%u body=%.*s", (unsigned)strlen(bitmap_str), 96, bitmap_str);
    }
    if ((int)strlen(bitmap_str) != PANEL_LED_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bitmap length");
        return ESP_OK;
    }

    uint8_t bitmap[PANEL_LED_COUNT];
    for (int i = 0; i < PANEL_LED_COUNT; i++) {
        char ch = bitmap_str[i];
        if (ch == '0') {
            bitmap[i] = 0;
        } else if (ch == '1') {
            bitmap[i] = 1;
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bitmap format");
            return ESP_OK;
        }
    }

    esp_err_t err = custom_sim_set_bitmap(bitmap, PANEL_LED_COUNT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "custom set bitmap failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static bool parse_hex_color(const char* hex, uint8_t* r8, uint8_t* g8, uint8_t* b8) {
    if (!hex || !r8 || !g8 || !b8) {
        return false;
    }

    if (hex[0] == '#') {
        hex++;
    }
    if (strlen(hex) != 6) {
        return false;
    }

    int h0 = hex_to_int(hex[0]);
    int h1 = hex_to_int(hex[1]);
    int h2 = hex_to_int(hex[2]);
    int h3 = hex_to_int(hex[3]);
    int h4 = hex_to_int(hex[4]);
    int h5 = hex_to_int(hex[5]);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0 || h5 < 0) {
        return false;
    }

    *r8 = (uint8_t)((h0 << 4) | h1);
    *g8 = (uint8_t)((h2 << 4) | h3);
    *b8 = (uint8_t)((h4 << 4) | h5);
    return true;
}

static esp_err_t color_post_handler(httpd_req_t* req) {
    bool from_ai = is_chat_control_request(req);
    if (s_wifi_only_mode && !from_ai) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi-only enabled");
        return ESP_OK;
    }

    char body[96] = {0};
    int remain = req->content_len;
    if (remain <= 0 || remain >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    int offset = 0;
    while (remain > 0) {
        int got = httpd_req_recv(req, body + offset, remain);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        remain -= got;
        offset += got;
    }
    body[offset] = '\0';

    char color[16] = {0};
    if (!parse_form_value(body, "color", color, sizeof(color))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing color");
        return ESP_OK;
    }
    if (from_ai) {
        ESP_LOGI(TAG, "ai control color=%s", color);
    }

    uint8_t r8 = 0;
    uint8_t g8 = 0;
    uint8_t b8 = 0;
    if (!parse_hex_color(color, &r8, &g8, &b8)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid color");
        return ESP_OK;
    }

    rgb_set_global_color8(r8, g8, b8);
    esp_err_t err2 = custom_sim_set_color8(r8, g8, b8);
    if (err2 != ESP_OK) {
        ESP_LOGW(TAG, "custom_sim_set_color8 failed: %s", esp_err_to_name(err2));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_connect_post_handler(httpd_req_t* req) {
    char body[256] = {0};
    int remain = req->content_len;
    if (remain <= 0 || remain >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    int offset = 0;
    while (remain > 0) {
        int got = httpd_req_recv(req, body + offset, remain);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        remain -= got;
        offset += got;
    }
    body[offset] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    bool wifi_only = s_wifi_only_mode;
    if (!parse_form_value(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_OK;
    }
    parse_form_value(body, "pass", pass, sizeof(pass));
    parse_form_bool(body, "wifi_only", &wifi_only);
    trim_ascii_spaces(ssid);

    wifi_config_t sta_cfg = {0};
    strlcpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.failure_retry_cnt = 5;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta connect failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "connect failed");
        return ESP_OK;
    }

    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    strlcpy(s_sta_pass, pass, sizeof(s_sta_pass));
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    apply_wifi_only_mode(wifi_only);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_only_post_handler(httpd_req_t* req) {
    char body[64] = {0};
    int remain = req->content_len;
    if (remain <= 0 || remain >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    int offset = 0;
    while (remain > 0) {
        int got = httpd_req_recv(req, body + offset, remain);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        remain -= got;
        offset += got;
    }
    body[offset] = '\0';

    bool wifi_only = false;
    if (!parse_form_bool(body, "wifi_only", &wifi_only)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing wifi_only");
        return ESP_OK;
    }

    apply_wifi_only_mode(wifi_only);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t start_softap(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_ap_netif || !s_sta_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };

    if (strlen(AP_PASS) < 8) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ap_config.ap.password[0] = '\0';
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    wifi_config_t sta_cfg = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ensure_ap_dhcp_offers_dns();

    bool has_sta = false;
    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};
    esp_err_t load_err = load_sta_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass), &has_sta);
    if (load_err == ESP_OK && has_sta) {
        wifi_config_t saved_sta_cfg = {0};
        strlcpy((char*)saved_sta_cfg.sta.ssid, saved_ssid, sizeof(saved_sta_cfg.sta.ssid));
        strlcpy((char*)saved_sta_cfg.sta.password, saved_pass, sizeof(saved_sta_cfg.sta.password));
        saved_sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        saved_sta_cfg.sta.failure_retry_cnt = 5;
        saved_sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        saved_sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        saved_sta_cfg.sta.pmf_cfg.capable = true;
        saved_sta_cfg.sta.pmf_cfg.required = false;

        if (esp_wifi_set_config(WIFI_IF_STA, &saved_sta_cfg) == ESP_OK && esp_wifi_connect() == ESP_OK) {
            strlcpy(s_sta_ssid, saved_ssid, sizeof(s_sta_ssid));
            strlcpy(s_sta_pass, saved_pass, sizeof(s_sta_pass));
            ESP_LOGI(TAG, "auto connect STA: %s", s_sta_ssid);
        } else {
            ESP_LOGW(TAG, "auto connect STA failed");
        }
    } else {
        ESP_LOGI(TAG, "no saved STA credentials");
    }

    ESP_LOGI(TAG, "APSTA started, AP ssid=%s pass=%s", AP_SSID, AP_PASS);
    return ESP_OK;
}

static esp_err_t start_http_server(void) {
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 16384;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t state_uri = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t mode_uri = {
        .uri = "/api/mode",
        .method = HTTP_POST,
        .handler = mode_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t custom_uri = {
        .uri = "/api/custom",
        .method = HTTP_POST,
        .handler = custom_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t color_uri = {
        .uri = "/api/color",
        .method = HTTP_POST,
        .handler = color_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t wifi_connect_uri = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_connect_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t wifi_only_uri = {
        .uri = "/api/wifi/only",
        .method = HTTP_POST,
        .handler = wifi_only_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t chat_uri = {
        .uri = "/api/chat",
        .method = HTTP_POST,
        .handler = chat_post_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &state_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &mode_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &custom_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &color_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wifi_connect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wifi_only_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &chat_uri));
    return ESP_OK;
}

esp_err_t web_control_start(void) {
    esp_err_t err = start_softap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_softap failed: %s", esp_err_to_name(err));
        return err;
    }

    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_http_server failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

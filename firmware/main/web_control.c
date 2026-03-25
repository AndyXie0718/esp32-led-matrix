#include "web_control.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "custom_sim.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
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
    "<div id='msg'></div></div></main>"
    "<script>"
    "const W=8,H=8;const state={mode:'water',grid:Array(W*H).fill(0),r8:255,g8:255,b8:255,color:'#ffffff',sta_connected:false,sta_ssid:'',sta_ip:'',wifi_only:false};"
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
    "async function loadState(){try{const r=await fetch('/api/state');const j=await r.json();state.mode=j.mode||state.mode;if(j.r8!==undefined)state.r8=clamp8(j.r8);if(j.g8!==undefined)state.g8=clamp8(j.g8);if(j.b8!==undefined)state.b8=clamp8(j.b8);state.sta_connected=!!j.sta_connected;state.sta_ssid=j.sta_ssid||'';state.sta_ip=j.sta_ip||'';state.wifi_only=!!j.wifi_only;setModeBtn();syncColorInputs();updatePreview();updateWifiStatus();updateWifiOnlyUI();if(state.sta_ssid){document.getElementById('wifi-ssid').value=state.sta_ssid;}}catch(e){msg('state load failed');}}"
    "async function setWifiOnly(on){const body='wifi_only='+(on?'1':'0');try{const r=await fetch('/api/wifi/only',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw 0;state.wifi_only=on;updateWifiOnlyUI();msg(on?'wifi only enabled':'wifi only disabled');setTimeout(loadState,500);}catch(e){msg('set wifi only failed');document.getElementById('wifi-only').checked=state.wifi_only;}}"
    "async function connectWifi(){const ssid=document.getElementById('wifi-ssid').value.trim();const pass=document.getElementById('wifi-pass').value;const wifiOnly=document.getElementById('wifi-only').checked;if(!ssid){msg('ssid required');return;}const body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)+'&wifi_only='+(wifiOnly?'1':'0');try{const r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw 0;state.sta_ssid=ssid;state.sta_connected=false;state.sta_ip='';state.wifi_only=wifiOnly;updateWifiStatus();updateWifiOnlyUI();msg('wifi connecting...');setTimeout(loadState,2000);}catch(e){msg('connect failed');}}"
    "async function setMode(mode){try{const r=await fetch('/api/mode?value='+mode,{method:'POST'});if(!r.ok)throw 0;state.mode=mode;setModeBtn();msg('mode -> '+mode);}catch(e){msg('set mode failed');}}"
    "async function applyColor(){readColorInputs();const body='color='+encodeURIComponent(state.color);try{const r=await fetch('/api/color',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw 0;msg('color applied to all modes');}catch(e){msg('apply color failed');}}"
    "async function applyPattern(){const bitmap=state.grid.map(v=>v?'1':'0').join('');try{const r=await fetch('/api/custom',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'bitmap='+bitmap});if(!r.ok)throw 0;msg('pattern applied');}catch(e){msg('apply failed');}}"
    "document.getElementById('mode-fire').onclick=()=>setMode('fire');"
    "document.getElementById('mode-water').onclick=()=>setMode('water');"
    "document.getElementById('mode-custom').onclick=()=>setMode('custom');"
    "document.getElementById('wifi-connect').onclick=connectWifi;"
    "document.getElementById('wifi-only').onchange=e=>setWifiOnly(!!e.target.checked);"
    "document.getElementById('color-picker').addEventListener('input',()=>{readColorInputs();});"
    "document.getElementById('apply-color').onclick=applyColor;"
    "document.getElementById('apply').onclick=applyPattern;"
    "makeGrid();syncColorInputs();updatePreview();updateWifiStatus();loadState();"
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

static esp_err_t mode_post_handler(httpd_req_t* req) {
    if (s_wifi_only_mode) {
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

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &state_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &mode_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &custom_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &color_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wifi_connect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wifi_only_uri));
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

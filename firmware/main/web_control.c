#include "web_control.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "custom_sim.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "panel_config.h"
#include "sim_manager.h"

#define AP_SSID "LED-Matrix"
#define AP_PASS "12345678"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4

static const char* TAG = "web_control";

static httpd_handle_t s_server = NULL;

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
    "#msg{min-height:22px;font-size:14px;color:#4f5b63;}"
    "</style></head><body><main><h1>ESP32 LED Matrix</h1><div class='card'>"
    "<div class='modes'>"
    "<button id='mode-fire' class='alt'>Fire</button>"
    "<button id='mode-water' class='alt'>Fluid</button>"
    "<button id='mode-custom' class='alt'>Custom</button>"
    "</div>"
    "<div id='grid'></div>"
    "<button id='apply'>Confirm Pattern</button>"
    "<div id='msg'></div></div></main>"
    "<script>"
    "const W=8,H=8;const state={mode:'water',grid:Array(W*H).fill(0)};"
    "const msg=t=>document.getElementById('msg').textContent=t;"
    "function setModeBtn(){['fire','water','custom'].forEach(m=>{const b=document.getElementById('mode-'+m);b.classList.toggle('active',state.mode===m);});}"
    "function makeGrid(){const g=document.getElementById('grid');g.innerHTML='';for(let y=0;y<H;y++){for(let x=0;x<W;x++){const i=y*W+x;const d=document.createElement('button');d.className='cell'+(state.grid[i]?' on':'');d.onclick=()=>{state.grid[i]=state.grid[i]?0:1;d.className='cell'+(state.grid[i]?' on':'');};g.appendChild(d);}}}"
    "async function loadState(){try{const r=await fetch('/api/state');const j=await r.json();state.mode=j.mode||state.mode;setModeBtn();}catch(e){msg('state load failed');}}"
    "async function setMode(mode){try{const r=await fetch('/api/mode?value='+mode,{method:'POST'});if(!r.ok)throw 0;state.mode=mode;setModeBtn();msg('mode -> '+mode);}catch(e){msg('set mode failed');}}"
    "async function applyPattern(){const bitmap=state.grid.map(v=>v?'1':'0').join('');try{const r=await fetch('/api/custom',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'bitmap='+bitmap});if(!r.ok)throw 0;msg('pattern applied');}catch(e){msg('apply failed');}}"
    "document.getElementById('mode-fire').onclick=()=>setMode('fire');"
    "document.getElementById('mode-water').onclick=()=>setMode('water');"
    "document.getElementById('mode-custom').onclick=()=>setMode('custom');"
    "document.getElementById('apply').onclick=applyPattern;"
    "makeGrid();loadState();"
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
    char resp[48];
    const char* mode = mode_to_str(sim_manager_current());
    snprintf(resp, sizeof(resp), "{\"mode\":\"%s\"}", mode);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t mode_post_handler(httpd_req_t* req) {
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

    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "softAP started ssid=%s pass=%s", AP_SSID, AP_PASS);
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

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &state_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &mode_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &custom_uri));
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

# 第 14 课：Wi-Fi AP/STA 混合模式

前面的课程我们已经把灯板的物理引擎讲得比较完整了。到了第 14 课，系统开始真正“出圈”：它不再只会在本地跑特效，而是要通过 Wi-Fi 让手机、电脑、甚至外网服务来控制灯板。

这一课的核心，是理解 **SoftAP + STA 混合模式**。在本项目中，ESP32 同时扮演两个角色：

1. **SoftAP（热点）**：自己开一个 Wi-Fi 热点，保证用户即使没有任何路由器，也能直接连上灯板。
2. **STA（客户端）**：连接到用户家里的路由器，让设备可以访问外网，并支持后续的 AI 接口调用。

这就是为什么项目不是简单地“连上一个 Wi-Fi”就结束，而是要做成一个既能本地控制、又能联网扩展的完整控制节点。

---

## 1. 先看系统启动链路

在 [main/main.c](main/main.c) 里，联网服务是在系统初始化后启动的：

```c
void app_main(void) {
    gravity_init();

    esp_err_t err = gravity_sensor_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gravity_sensor_start failed: %s", esp_err_to_name(err));
        return;
    }

    sim_runtime_config_t cfg = {
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
        .core_id = 1,
#else
        .core_id = 0,
#endif
        .stack_size = 8192,
        .priority = 5,
        .stop_timeout_ms = 1000,
    };

    err = sim_manager_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sim_manager_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = sim_manager_start(g_boot_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sim_manager_start failed: %s", esp_err_to_name(err));
        return;
    }

    err = web_control_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web_control_start failed: %s", esp_err_to_name(err));
    }
}
```

### 这段启动顺序说明了什么？

它说明联网服务并不是独立运行的，而是和整个灯板状态机绑定在一起：

- 先启动重力传感器，保证后续物理效果有输入。
- 再启动 `sim_manager`，让灯板至少先跑起来一个基础模式。
- 最后启动 `web_control_start()`，把 Wi-Fi 和 HTTP 服务拉起来。

这是一种很稳的嵌入式启动顺序：**先准备内部状态，再开放外部控制接口。**

---

## 2. AP/STA 混合模式是什么

混合模式最重要的价值，是让设备同时满足两个场景：

- **第一次上电、没有路由器密码**：用户仍然可以通过热点直接访问设备页面。
- **已经配置过家庭 Wi-Fi**：设备可以连外网，支持 NAT 转发和 AI 接口访问。

在代码里，这个能力由 `esp_wifi_set_mode(WIFI_MODE_APSTA)` 打开：

```c
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
```

### 这个模式的意义

`WIFI_MODE_APSTA` 不是简单地“开两个功能”，它意味着 ESP32 的 Wi-Fi 协议栈同时维护两套逻辑：

- AP 侧负责广播 SSID、分配 IP、接收手机连接。
- STA 侧负责连接外部路由器，获取上网地址。

这也是本项目能够做到“设备本地可控 + 设备还能访问外网”的根本原因。

---

## 3. SoftAP 初始化：让用户先连上来

看 [main/web_control.c](main/web_control.c) 中的 `start_softap()`：

```c
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

    ...
    return ESP_OK;
}
```

### 逐段解释

#### 1. 先初始化 NVS
`nvs_flash_init()` 是为了让设备有地方保存 Wi-Fi 账号密码。用户第一次配网后，SSID 和密码会写入 NVS，下次上电就能自动连接。

#### 2. 初始化 `esp_netif` 和事件循环
`esp_netif_init()` 提供网络接口层，`esp_event_loop_create_default()` 提供事件调度机制。没有它们，Wi-Fi 事件和 IP 事件就没有地方分发。

#### 3. 创建 AP 和 STA 的默认网络接口
`esp_netif_create_default_wifi_ap()` 和 `esp_netif_create_default_wifi_sta()` 分别创建两个角色的网络接口对象。这里必须同时创建，因为后面会在 AP 和 STA 之间同步 DNS、做 NAT。

#### 4. 组装 AP 配置
`AP_SSID`、`AP_PASS`、`AP_CHANNEL`、`AP_MAX_CONN` 定义了热点的名字、密码、信道和最大连接数。这里默认热点叫 `LED-Matrix`，用户拿手机一搜就能看到。

#### 5. 绑定事件回调
`wifi_event_handler()` 和 `ip_event_handler()` 分别处理 Wi-Fi 连接状态变化与 STA 获得 IP 的时刻。

#### 6. 真正开启 APSTA
调用 `esp_wifi_set_mode(WIFI_MODE_APSTA)` 之后，ESP32 就同时拥有热点和客户端身份了。

---

## 4. 事件驱动：Wi-Fi 状态不是轮询出来的

ESP-IDF 的 Wi-Fi 不是靠死循环不断查询“连上没”，而是靠事件回调通知状态变化。

### Wi-Fi 断开和连接事件

```c
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
```

### 这段代码的关键点

#### 1. STA 断开时要立刻清状态
` s_sta_connected = false; ` 和 `s_sta_ip[0] = '\0';` 用来把状态清空，避免界面还显示“已连接”但实际上已经断网。

#### 2. 断开后关闭 NAT
`set_nat_enabled(false);` 表示既然 STA 不在线了，那 AP 侧也不能再把流量转发到外网。

#### 3. 有保存过 SSID 就自动重连
如果设备之前已经保存过 `s_sta_ssid`，那么断线后会调用 `esp_wifi_connect()` 尝试自动重连。这能提升设备的可用性，避免一次掉线就要重新进网页配置。

---

## 5. STA 拿到 IP 后，才算真正联网成功

### IP 事件处理

```c
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
```

### 为什么“拿到 IP”比“连接成功”更重要？

因为 `STA_CONNECTED` 只说明 Wi-Fi 链路已经连上路由器，但还不代表真正能上网。只有拿到 IP，才意味着：

- DHCP 成功了
- 路由表配置好了
- DNS 也可能开始可用
- NAT 转发可以开启

所以项目里是等 `IP_EVENT_STA_GOT_IP` 才去 `set_nat_enabled(true)`。

---

## 6. NAT 转发：让 AP 侧设备借道上网

如果你让手机连上 ESP32 的热点，但 ESP32 本身又连着家里路由器，那么 AP 侧的手机理论上也可以通过 ESP32 转发访问外网。这个能力就是 NAT。

```c
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
```

### 这里的逻辑

- 先拿到 AP 的 IP 地址。
- 再调用 `ip_napt_enable()` 在这个地址上开启或关闭 NAT。

这就是把 ESP32 变成一个小型路由器的关键步骤。

---

## 7. AP 的 DNS 下发：让连上的手机能正确解析域名

AP 模式下，手机拿到的 DNS 服务器信息并不一定自动正确，所以项目里做了专门处理。

```c
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
```

### 这段代码的作用

当 STA 已经连上路由器并获得有效 DNS 后，AP 侧 DHCP 服务器也同步同样的 DNS。这样手机连上灯板热点后，不仅能访问本地页面，也更容易访问外部服务。

这对后面的 AI 接口调用非常重要，因为 AI 请求要访问公网域名。

---

## 8. 用户如何把家里 Wi-Fi 交给设备

项目通过网页表单提交 SSID 和密码，进入 `wifi_connect_post_handler()`：

```c
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
```

### 这段流程可以拆成 5 步

1. 读取 HTTP 请求体。
2. 解析出 `ssid`、`pass`、`wifi_only`。
3. 组装 `wifi_config_t`。
4. 调用 `esp_wifi_set_config()` 和 `esp_wifi_connect()`。
5. 保存状态并返回 JSON 成功响应。

### 为什么要先 `esp_wifi_disconnect()`？

因为 STA 可能已经连着另一个网络。先断开，再切换配置，能避免旧连接状态干扰新连接。

---

## 9. 什么是 Wi-Fi only 模式

项目里还有一个很实用的设计：`wifi_only`。

```c
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
```

### 它解决了什么问题？

当用户希望灯板只做联网控制、不再播放本地动画时，可以进入 Wi-Fi only 模式。进入后：

- 当前特效会被停掉
- LED 会清屏
- 设备专注于网络服务

退出后，又可以恢复之前的模拟模式。

这体现了调度器思想在联网层面的延伸：**不仅特效能切换，系统运行策略也能切换。**

---

## 10. HTTP 服务如何把控制能力暴露出去

热点和 STA 建好后，最终还要用 HTTP 把控制接口开放出来。

```c
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wifi_connect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &wifi_only_uri));
    return ESP_OK;
}
```

### 这段代码体现了什么设计思想

它把所有页面和 API 都注册到同一个 HTTP 服务里：

- `/` 负责显示页面
- `/api/state` 负责返回当前状态
- `/api/mode` 负责切换特效
- `/api/wifi/connect` 负责配网
- `/api/wifi/only` 负责切换系统策略

这让浏览器和设备之间的交互非常清晰：前端只需要发 HTTP 请求，不需要知道底层是怎么连 Wi-Fi 的。

---

## 11. 本课最重要的工程结论

第 14 课不是单纯在讲 Wi-Fi API，而是在讲一个完整的嵌入式联网架构：

1. **AP 负责入口**：保证设备永远能被用户找到。
2. **STA 负责上网**：让设备进入互联网世界。
3. **NAT 负责转发**：让 AP 侧客户端也能借道上网。
4. **DNS 同步负责可用性**：让网页和外部服务都能顺畅访问。
5. **HTTP 服务负责控制面**：把硬件能力暴露为网页接口。

换句话说，ESP32 在这里不只是一个“会发光的板子”，而是一个真正具备网络身份的控制节点。

---

## 本课小结

这一课的重点，是理解 **AP/STA 混合模式不是“两个 Wi-Fi 功能叠在一起”那么简单**，而是一整套从建网、连网、事件回调、NAT 转发到 HTTP 控制的系统工程。

如果把前面的物理模拟比作“让灯板活起来”，那这一课就是“让灯板走出本地，变成可联网的设备”。

---
[👉 下一课：第 15 课：嵌入式 HTTP 服务端实现](curriculum_outline.md)
下一课我们会继续沿着这里的 HTTP 服务往下走，深入讲清楚 GET/POST 路由、请求体解析，以及前端如何通过接口驱动灯板状态。
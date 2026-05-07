# 第 15 课：嵌入式 HTTP 服务端实现

在上一课中我们让 ESP32 成功接入了网络并开启了 Web 服务器。今天我们将深入探讨这个服务器是如何处理复杂请求的：包括 **GET 状态查询**、**POST 模式切换**、以及如何编写能在单片机这种内存敏感环境下稳定运行的 **请求解析器**。

---

## 1. 注册 HTTP 路由 (Handlers)

ESP-IDF 组件 `esp_http_server` 的核心是“路由注册”。在 [main/web_control.c](main/web_control.c#L1127) 的 `start_http_server` 函数中，我们定义了各种 API 接口。

```c
// 1. 定义一个 URI 处理器结构体
httpd_uri_t state_uri = {
    .uri      = "/api/state",      // 网址路径
    .method   = HTTP_GET,          // GET 请求常用于查询
    .handler  = state_get_handler, // 收到请求后去执行哪个 C 函数
    .user_ctx = NULL,
};

// 2. 将处理器注册到运行中的 server 实例
httpd_register_uri_handler(s_server, &state_uri);
```

> **设计思路：** 我们将静态网页（`/`）和数据接口（`/api/...`）分开。静态网页返回 HTML，数据接口返回 JSON。这种“前后端分离”的思想可以让硬件更高效地只处理数据逻辑。

---

## 2. 状态查询：将 C 结构体序列化为 JSON

当用户手机打开网页，第一步就是询问灯板：“你现在在跑什么模式？亮度是多少？”

在 `state_get_handler` 函数中，我们手动构建了一个 JSON 字符串并发送：

```c
static esp_err_t state_get_handler(httpd_req_t* req) {
    char resp[224];
    const char* mode = mode_to_str(sim_manager_current()); // 获取当前模式名
    
    uint8_t r8, g8, b8;
    rgb_get_global_color8(&r8, &g8, &b8); // 获取当前全局颜色设置

    // 将这些 C 语言的变量“拼”成 JSON 文本
    snprintf(resp, sizeof(resp),
             "{\"mode\":\"%s\",\"r8\":%u,\"g8\":%u,\"b8\":%u,\"sta_connected\":%s,\"sta_ssid\":\"%s\"}",
             mode, (unsigned)r8, (unsigned)g8, (unsigned)b8,
             s_sta_connected ? "true" : "false", s_sta_ssid);

    // 设置 Content-Type 为 JSON，告诉浏览器这是数据
    httpd_resp_set_type(req, "application/json"); 
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
```

---

## 3. 请求解析：在受限内存下安全处理数据

相比 GET 处理，POST 请求更棘手。我们需要解析用户发来的 Body 数据（例如表单或 JSON）。因为 ESP32 内存（RAM）非常有限，直接 `malloc` 太大的空间可能会让系统崩溃。

### (1) 安全读取 Body (Body Buffer)
看看 `read_request_body` 的实现：

```c
static esp_err_t read_request_body(httpd_req_t* req, size_t max_len, char** out_body, size_t* out_len) {
    int content_len = req->content_len;
    // 1. 严格检查数据长度，防止缓冲区溢出攻击
    if (content_len <= 0 || (size_t)content_len > max_len) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "payload too large");
        return ESP_ERR_INVALID_SIZE;
    }

    // 2. 只有检查通过后，才分配内存
    char* body = (char*)malloc((size_t)content_len + 1);
    
    // 3. 循环接收数据，确保每一字节都正确读入
    int offset = 0;
    while (offset < content_len) {
        int n = httpd_req_recv(req, body + offset, content_len - offset);
        if (n <= 0) { /* 错误处理... */ }
        offset += n;
    }
    body[offset] = '\0'; // 加上字符串终结符
    *out_body = body;
    return ESP_OK;
}
```

### (2) URL 解码 (URL Decoding)
网页提交的表单数据中，特殊字符会被编码。例如空格变 `+`，特殊字符变 `%20`。我们要实现 `url_decode_inplace` 将其还原回 C 字符串：

```c
static void url_decode_inplace(char* s) {
    char* src = s; char* dst = s;
    while (*src) {
        if (*src == '+') { *dst++ = ' '; src++; }
        else if (*src == '%' && isxdigit((unsigned char)src[1]) /* ... */) {
            // 解析 %HEX
            int hi = hex_to_int(src[1]);
            int lo = hex_to_int(src[2]);
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else { *dst++ = *src++; }
    }
    *dst = '\0';
}
```

---

## 4. 控制指令：从网络数据到硬件动作

当控制中心收到 `/api/mode` 请求时：
1. 解析出参数 `value=fire`。
2. 调用 `sim_manager_switch(SIM_MODE_FIRE)`。
3. 调度器停止当前任务，启动火焰模拟。
4. 返回 `{"ok":true}`。

```c
static esp_err_t mode_post_handler(httpd_req_t* req) {
    char value[16] = {0};
    // 从 URL 查询字符串中提取 mode
    httpd_query_key_value(query, "value", value, sizeof(value));

    sim_mode_t mode;
    if (str_to_mode(value, &mode)) {
        // 调用我们第 13 课学过的调度器
        sim_manager_switch(mode); 
        httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return ESP_OK;
}
```

---

## 本课小结

嵌入式 Web 开发与桌面端最大的区别在于 **资源受限**。

*   **资源管理**：我们通过严格检查 `req->content_len` 来防御内存耗尽。
*   **通信范式**：利用 JSON 作为“通用语言”，让手机浏览器（JavaScript）和 ESP32（C 语言）能听懂彼此的意图。
*   **非阻塞调用**：HTTP 处理器代码应该执行迅速，把沉重的物理计算交给后台任务（Task），自己只管下命令。

---
[👉 下一课：第 16 课：嵌入式全栈：HTML 字符串与 CSS 布局](curriculum_outline.md)
我们处理了逻辑，但网页长什么样？下一课我们将揭秘如何把成千上万行的 HTML/CSS 前端代码“塞”进不到 1MB 的单片机 Flash 里！
# 第 17 课：AI 控制流设计：从自然语言到硬件动作

在前几课中，我们已经实现了 Web 界面控制和 HTTP API。今天我们将迎来本项目最激进的控制方式：**AI 代理控制**。我们将学习如何让 ESP32 作为一个“桥梁”，连接用户的自然语言指令与底层的 LED 硬件逻辑。

---

## 1. 嵌入式 AI 代理架构 (Device Proxy)

由于 ESP32 本身无法运行大型 LLM（大语言模型），我们采用 **设备代理模式**：
1. **浏览器** 发送用户指令给 **ESP32**。
2. **ESP32** 注入系统提示词（System Prompt），并携带 API Key 转发给 **云端 AI**（如智谱 GLM）。
3. **ESP32** 接收 AI 返回的 JSON 格式指令，由浏览器解析并最终驱动硬件。

这种模式的优势在于：**安全**（由硬件控制流）且 **逻辑可控**。

---

## 2. 系统提示词：给 AI 的“剧本”

在 [main/web_control.c](main/web_control.c#L808) 的 JavaScript 部分，我们定义了极其关键的 `chatSystemPrompt`。这是教 AI 如何“说话”和“控制硬件”的说明书：

```javascript
const chatSystemPrompt = '你是ESP32灯板内置AI... 请不要输出markdown代码块。' +
    '若需要驱动灯板，请输出严格JSON，格式为：' +
    '{"reply":"自然语言回答","control":{"mode":"fire|water|custom","color":"#RRGGBB","bitmap":"64位0/1字符串"}}';
```

> **设计思路**：我们要求 AI 返回一种“混合格式”——既有给人类看的文字，也有给机器运行的 `control` 结构。

---

## 3. 实现反向代理 (Reverse Proxy Handler)

在 C 语言端，[chat_post_handler](main/web_control.c#L400) 负责最苦最累的活：建立 SSL 连接并透传数据。

```c
static esp_err_t chat_post_handler(httpd_req_t* req) {
    // 1. 从自定义 Header 中提取 API Key
    char api_key[CHAT_MAX_API_KEY_LEN] = {0};
    read_header_value(req, "X-Zhipu-Api-Key", api_key, sizeof(api_key));

    // 2. 配置 HTTP 客户端（HTTPS，带根证书校验）
    esp_http_client_config_t config = {
        .url = CHAT_API_URL,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach, // 使用 IDF 内置证书束验证
        .timeout_ms = 480000,                       // AI 回复较慢，超时设长一点
    };

    // 3. 建立连接并发送用户对话
    esp_http_client_handle_t client = esp_http_client_init(&config);
    // ... 发送 Authorization: Bearer <Key> 等 Header ...
    
    // 4. 流式读取并转发 (Streaming Chunk)
    char chunk[1024];
    while (1) {
        int read = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read <= 0) break;
        // 收到云端一段话，立刻转发给手机浏览器，不占用过多缓存
        httpd_resp_send_chunk(req, chunk, read);
    }
}
```

---

## 4. 将 AI 意图转化为硬件操作 (Action)

当前端浏览器收到 AI 的回复后，会执行 [applyAiControl](main/web_control.c#L1017) 函数：

```javascript
async function applyAiControl(control, userText) {
    if (!control) return;

    // AI 想切换模式？
    if (control.mode) {
        await setMode(control.mode, true); // true 表示这是来自 AI 的指令
    }

    // AI 想改颜色？
    if (control.color) {
        // 解析 #RRGGBB 并调用第 15 课的 API
        const rgb = hexColorToRgb(control.color);
        state.r8 = rgb.r; state.g8 = rgb.g; state.b8 = rgb.b;
        await applyColor(true); 
    }

    // AI 想画画？ (64位位图)
    if (control.bitmap) {
        state.grid = bitmapToGrid(control.bitmap);
        makeGrid(); // 更新网页画板显示
        await applyPattern(true); // 调用自定义模式 API 发送像素点
    }
}
```

---

## 5. 关键词兜底 (Heuristic Control)

如果云端 AI “调皮”了，没有返回正确的 JSON 而是回了一句“好的，我把灯变红了”，我们的前端还有一层 **语义兜底** [inferControlFromText](main/web_control.c#L1008)：

```javascript
function inferControlFromText(userText, assistantReply) {
    // 正则匹配：如果文本里提到了“火焰”或“fire”
    if (/火焰|fire/i.test(assistantReply)) {
        return { mode: 'fire' };
    }
    // ... 其他关键词匹配 ...
}
```

---

## 本课小结

这一课我们完成了一个非常“现代”的设计：
1. **数据中继**：ESP32 不只是控制器，还是处理 HTTPS 请求的安全网关。
2. **结构化输出**：利用 JSON 让非结构化的 AI 生成变得可编程。
3. **协同控制**：展示了“云端大脑（AI）+ 本地逻辑（C 代码）+ 用户界面（JS）”如何共同驱动一颗 LED 灯珠的高级形态。

---
[👉 下一课：第 18 课：NVS 持久化存储：断电也不怕](curriculum_outline.md)
用户辛辛苦苦配好的 Wi-Fi 密码和灯光模式，断电就丢了？下一课我们将学习如何操作 ESP32 的“闪存盘”——NVS。
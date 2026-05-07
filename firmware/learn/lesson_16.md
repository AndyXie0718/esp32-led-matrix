# 第 16 课：嵌入式全栈：HTML 字符串与 CSS 布局

在上一课中，我们实现了处理数据的 API。但用户如何访问这些 API 呢？本课我们将探讨如何将完整的 Web 控制面板“塞”进 ESP32，并实现美观的响应式 UI。

---

## 1. 内存中的“网页文件”

在标准的 Web 开发中，HTML 存放在 `.html` 文件里。但在嵌入式 IDF 中，我们通常有几种选择：
1. **SPIFFS 文件系统**：从 Flash 读取文件（较慢，需额外配置）。
2. **嵌入式字符串 (String Embedding)**：将 HTML 直接写在 C 代码里（最快，适合小型页面）。

在本项目 [main/web_control.c](main/web_control.c#L508) 中，我们使用了 **静态长字符串** 的方式：

```c
// 使用 static const 修饰，确保字符串存储在 Flash 区域（RoData），不占用昂贵的 RAM
static const char s_index_html[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>" // 关键：适配手机屏幕
    "<title>LED Matrix Control</title>"
    "<style>"
    // 这里是 CSS，使用了原生变量 (--bg, --ink) 实现现代感色调
    ":root{--bg:#f4efe4;--ink:#1f2a31;--card:#fffdf8;--line:#d6cab8;--active:#e66f3c;--btn:#2f5d50;}"
    "body{margin:0;font-family:sans-serif;background:linear-gradient(135deg,#f4efe4,#efe4d0);}"
    // 使用 Flex 和 Grid 布局，自动适配手机和电脑
    ".modes{display:flex;gap:10px;flex-wrap:wrap;}"
    ".card{background:var(--card);border-radius:14px;box-shadow:0 8px 24px rgba(0,0,0,0.1);}"
    "</style></head>"
    "<body>"
    "<main><h1>ESP32 LED Matrix</h1>"
    // UI 组件：模式按钮、颜色拾取器、画板
    "<div class='modes'><button id='mode-fire'>Fire</button>...</div>"
    "</main></body></html>";
```

---

## 2. 响应式控制逻辑 (JavaScript)

为了让页面动起来，我们在 HTML 字符串的末尾嵌入了大量的 JavaScript。

### (1) 状态同步
当页面加载时，它需要向 ESP32 询问当前的状态：

```javascript
async function loadState() {
    try {
        const r = await fetch('/api/state'); // 调用第 15 课写的接口
        const j = await r.json();
        state.mode = j.mode;
        state.r8 = j.r8;
        // 更新 UI 按钮状态
        updatePreview();
        setModeBtn(); 
    } catch(e) { console.error('Load state failed'); }
}
```

### (2) 指令下发
点击按钮后，前端立即发送指令：

```javascript
async function setMode(mode) {
    // 调用 POST 接口
    const r = await fetch('/api/mode?value=' + mode, { method: 'POST' });
    if (r.ok) {
        state.mode = mode;
        setModeBtn(); // 改变按钮颜色
    }
}
```

---

## 3. 面向移动端的细节优化

*   **Viewport 设置**：`<meta name='viewport' content='width=device-width,initial-scale=1'>` 非常关键。没有它，手机浏览器会把 760px 的卡片缩小成一个小点，无法操作。
*   **CSS 渐变 (Gradients)**：我们使用了 `radial-gradient`。比起背景图片，代码生成的渐变完全不占空间（仅几十个字节），但能极大地提升视觉质感。
*   **点击反馈**：通过 `:active` 伪类或 `button.active` Class，给用户实时的按下反馈，这在控制硬件时尤为重要，能让用户感知“指令已发出”。

---

## 4. 传输效率优化

当 HTML 字符串越来越长时，返回请求的函数 [main/web_control.c](main/web_control.c#L1066) 是这样做的：

```c
static esp_err_t root_get_handler(httpd_req_t* req) {
    // 设置响应头，告诉浏览器这是网页
    httpd_resp_set_type(req, "text/html");
    
    // 发送字符串。HTTPD_RESP_USE_STRLEN 会自动计算长度。
    // 如果网页非常大 (超过 4KB)，建议使用 httpd_resp_send_chunk 分块发送。
    httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
```

---

## 本课小结

通过这一课，你学会了如何将 ESP32 变成一个真正的“全栈服务器”：
1.  **RoData 存储**：利用存储特性在 Flash 中保存静态网页素材。
2.  **现代 CSS**：利用 Flexbox 和 CSS Variables 让界面在手机上完美呈现。
3.  **异步通信 (Fetch API)**：让浏览器在不刷新页面的情况下控制硬件，实现如 App 般的丝滑体验。

---
[👉 下一课：第 17 课：AI 控制流设计：从自然语言到硬件动作](curriculum_outline.md)
网页上那个 “AI Chat” 框是怎么工作的？下一课我们将学习如何通过 ESP32 代理请求，实现语音/文字一键控制灯效。
# 第 18 课：NVS 非易失性存储：让设备拥有“记忆”

在之前的课程中，我们实现了 Wi-Fi 连接和 AI 控制。但你会发现，一旦拔掉电源重启，Wi-Fi 密码就丢了。今天我们学习如何使用 ESP32 的 **NVS (Non-volatile Storage)** 组件，把数据永久刻在 Flash 芯片里。

---

## 1. 什么是 NVS？

NVS 是 ESP-IDF 提供的一套类似“键值对数据库”的系统（类似于网页的 LocalStorage）。
- **特点**：它是基于 Flash 的，断电不丢失。
- **结构**：`命名空间 (Namespace) -> 键 (Key) -> 值 (Value)`。

在本项目 [main/web_control.c](main/web_control.c#L24) 中，我们定义了存储 Wi-Fi 参数的专用命名空间：

```c
#define WIFI_NVS_NS "wifi_sta"       // 命名空间：就像文件夹名字
#define WIFI_NVS_KEY_SSID "ssid"     // 键：存放账号
#define WIFI_NVS_KEY_PASS "pass"     // 键：存放密码
```

---

## 2. 数据的持久化：保存密码

当用户在网页点击“连接”并成功获取 IP 后，我们会调用 [save_sta_credentials](main/web_control.c#L132) 来保存信息：

```c
static esp_err_t save_sta_credentials(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    // 1. 打开 NVS 命名空间 (读写模式)
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    // 2. 写入字符串
    // 如果没有 SSID，存入空字符串；有则存入实际值
    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASS, pass ? pass : "");
    }

    // 3. 提交 (Commit) —— 关键步骤！
    // 就像 Git 提交代码一样，不 commit 数据不会真正写进 Flash
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    // 4. 关闭句柄，释放资源
    nvs_close(handle);
    return err;
}
```

---

## 3. 启动时的恢复：加载记忆

当灯板重新上电时，在初始化阶段会调用 [load_sta_credentials](main/web_control.c#L152) 检查有没有历史记录：

```c
static esp_err_t load_sta_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len, bool* has_data) {
    nvs_handle_t handle;
    // 1. 打开命名空间 (只读模式，更安全)
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t ssid_size = ssid_len;
    // 2. 读取字符串
    // nvs_get_str 会根据传入的长度自动截断，防止内存溢出
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        size_t pass_size = pass_len;
        err = nvs_get_str(handle, WIFI_NVS_KEY_PASS, pass, &pass_size);
    }

    nvs_close(handle);
    
    // 如果 SSID 不为空，说明我们曾经保存过连接信息
    if (err == ESP_OK && ssid[0] != '\0') {
        *has_data = true;
    }
    return err;
}
```

---

## 4. Flash 寿命与磨损均衡

初学者常问：“Flash 有擦写次数限制（通常 10 万次），我会把它写坏吗？”

*   **NVS 的优化**：ESP-IDF 的 NVS 实现了 **磨损均衡 (Wear Leveling)**。它不会总写在同一个物理位置，而是在一块区域内轮流写。
*   **最佳实践**：
    *   **不要在 Loop 或渲染循环里写 NVS**（比如每秒保存一次亮度是不对的）。
    *   **只在用户修改配置并确认时写**（比如点击“保存”按钮时）。
    *   **仅保存状态变更**：代码中只有在 `IP_EVENT_STA_GOT_IP` 成功后才保存，确保存入的是有效信息。

---

## 5. 初始化的“仪式感”

在 `app_main` 最开始，必须先执行 NVS 的硬件初始化，否则后续所有读写都会报错：

```c
// 初始化 NVS 默认分区
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // 如果分区损坏或格式不对，强制擦除并重新初始化
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

---

## 本课小结

通过 NVS，我们的灯板从一个“纯逻辑机”变成了有“记忆”的智能设备：
1.  **持久化**：学会了 `Open -> Write/Read -> Commit -> Close` 的标准流程。
2.  **安全性**：理解了命名空间的作用，避免不同组件的数据互相冲突。
3.  **可靠性**：学会了处理 NVS 初始化异常和 Flash 磨损保护。

---
[👉 下一课：第 19 课：多任务管理 (RTOS)：物理引擎与网络服务的共舞](curriculum_outline.md)
既然灯板要算物理碰撞，又要处理 HTTPS 数据转发，还要刷新灯珠，ESP32 是怎么同时做这么多事的？下一课揭秘 FreeRTOS 任务调度。
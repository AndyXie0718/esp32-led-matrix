# 第 8 课：非易失性存储 (NVS) 实战

通常情况下，单片机的变量存储在 RAM（随机存取存储器）中，掉电后数据会全部丢失。但在本项目中，我们需要 ESP32 记住 Wi-Fi 的 SSID 和密码，即使拔掉电源重启也能自动联网。这就要用到 ESP32 的 **NVS (Non-volatile Storage)**。

---

## 1. 什么是 NVS？

NVS 是 ESP-IDF 提供的一种机制，它将 Flash 存储器的一部分划分出来，以 **键值对 (Key-Value Pair)** 的形式存储数据（类似于网页代码中的 `localStorage` 或 Python 中的字典）。

### NVS 的核心概念：
*   **命名空间 (Namespace)**：为了防止不同模块的数据冲突（例如 Wi-Fi 模块和游戏模块都想存一个叫 `score` 的变量），NVS 使用命名空间进行隔离。
*   **键 (Key)**：数据的名字（最长 15 个字符）。
*   **值 (Value)**：实际存储的数据，可以是整数、字符串或二进制数据（Blob）。

---

## 2. 源码逻辑分析：Wi-Fi 信息的存取

打开 [main/web_control.c](main/web_control.c)，我们来看看本项目是如何持久化存储 Wi-Fi 凭据的。

### (1) 写入数据（持久化 SSID 和 密码）
```c
/**
 * @brief 将 Wi-Fi 凭据保存到 NVS 中
 */
static esp_err_t save_sta_credentials(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    
    // 1. 打开 NVS 命名空间 "wifi_sta"，模式为“读写”
    esp_err_t err = nvs_open("wifi_sta", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    // 2. 写入字符串。
    // WIFI_NVS_KEY_SSID 的值是 "ssid"，WIFI_NVS_KEY_PASS 的值是 "pass"
    err = nvs_set_str(handle, "ssid", ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", pass ? pass : "");
    }

    // 3. 【极度重要】：提交更改！
    // 类似于数据库的 commit。如果没有这一步，数据只会停留在内存缓存中，不会真正写进 Flash
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    // 4. 关闭句柄，释放资源
    nvs_close(handle);
    return err;
}
```

### (2) 读取数据（启动时自动加载）
```c
/**
 * @brief 从 NVS 加载保存的 Wi-Fi 凭据
 */
static esp_err_t load_sta_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len, bool* has_data) {
    nvs_handle_t handle;
    *has_data = false;

    // 1. 打开命名空间，此时用“只读”模式更安全
    esp_err_t err = nvs_open("wifi_sta", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;

    // 2. 读取字符串
    // 如果 Flash 中没有对应的键值，这里会返回 ESP_ERR_NVS_NOT_FOUND
    err = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "pass", pass, &pass_size);
    }

    nvs_close(handle);

    // 3. 检查是否真的读到了有效数据
    if (err == ESP_OK && ssid[0] != '\0') {
        *has_data = true;
    }
    return err;
}
```

---

## 3. 注意事项与性能建议

1.  **寿命问题**：Flash 存储器是有擦写寿命的（通常为 10 万次）。因此，**不要在循环中频繁调用 `nvs_commit`**（例如不要每秒存一次重力传感器数据）。只在用户修改配置（如修改亮度、改 Wi-Fi）时调用。
2.  **分区检查**：在 `app_main` 中，通常需要首先初始化 NVS 库：
    `esp_err_t ret = nvs_flash_init();`
    如果 Flash 布局改变导致初始化失败，代码通常会先执行 `nvs_flash_erase()` 再重新初始化。

---

## 4. 动手实践

1.  **观察持久化**：通过网页给 ESP32 连接一个 Wi-Fi，然后按下板子的 Reset 键或拔插电源。查看串口日志，观察它是否能自动调用 `load_sta_credentials` 并重新连接。
2.  **扩展尝试（思考）**：如果你想让设备记住上次用户选择的“LED 整体亮度”，你应该在 `save_sta_credentials` 的基础上使用哪个 NVS 函数？（提示：查找 `nvs_set_u8`）。
3.  **清空操作**：尝试在代码中调用 `nvs_erase_all(handle)`，看看重启后设备是否变回了初始的“未联网”状态。

---

## 本课小结
*   **NVS** 提供了类似于字典的持久化存储能力。
*   操作三部曲：**Open（打开管口） -> Set/Get（读写） -> Commit（真正存入 Flash）**。
*   命名空间有效防止了不同功能模块间的干扰。
*   它是嵌入式产品实现“配置记忆”和“用户偏好”的核心手段。

---
[👉 下一课：姿态解算与重力分量提取](curriculum_outline.md#第三部分章节) 准备好回到数学的世界，看看重力是如何“驱动”像素的了吗？
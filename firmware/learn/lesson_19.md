# 第 19 课：多任务管理 (RTOS)：物理引擎与网络服务的共舞

在单片机上，我们要同时运行物理模拟（每一帧都要刷点）、处理复杂的 HTTPS 网络代理、监听物理按键、维护 Web 服务器。这些任务如果串行执行，界面会卡死。今天我们学习 ESP32 的核心技能：**FreeRTOS 多任务调度**。

---

## 1. 任务的“分而治之”

在本项目中，我们将负载拆分为三个主要任务：
1. **Simulation Task (优先级 5)**：负责物理计算和 LED 渲染（对实时性要求最高）。
2. **Web Server Task (优先级 4-5)**：由 IDF 内部创建，处理 HTTP 请求。
3. **Key Task (优先级 4)**：在 [main/main.c](main/main.c#L20) 中定义，负责监听物理按键切换。

---

## 2. 核心函数：xTaskCreatePinnedToCore

在 [main/main.c](main/main.c#L106) 中，我们启动了物理按键监听任务：

```c
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    // 如果是双核芯片（如 ESP32-S3/C3 双核版），我们将任务固定在 Core 0
    ok = xTaskCreatePinnedToCore(
        mode_switch_task,  // 函数名
        "mode_switch",     // 任务调试名
        3072,              // 栈大小（单位：字节）
        NULL,              // 传递给任务的参数
        4,                 // 优先级（数值越大优先级越高）
        NULL,              // 任务句柄（用于控制任务）
        0                  // 固定在哪个核 (0 或 1)
    );
#else
    // 单核模式
    ok = xTaskCreate(mode_switch_task, "mode_switch", 3072, NULL, 4, NULL);
#endif
```

> **设计亮点**：物理引擎被固定在 Core 1，按键和网络被放在 Core 0。这样计算量巨大的流体模拟就不会因为网络延迟而掉帧。

---

## 3. 任务循环与睡眠 (vTaskDelay)

每一个 FreeRTOS 任务都必须是一个 **死循环**，并且必须包含 **阻塞（Blocking）** 语句，否则它会榨干 CPU，导致低优先级任务无法运行。

看看 [main/main.c](main/main.c#L59) 中的写法：

```c
static void mode_switch_task(void* arg) {
    while (1) {
        // ... 检测按键跳变逻辑 ...
        
        // 关键：礼让 CPU。即使只延迟 20ms，系统也能切换到其他任务
        vTaskDelay(pdMS_TO_TICKS(MODE_KEY_POLL_MS));
    }
}
```

---

## 4. 动态销毁与重建：任务管理器

我们不希望所有的模拟任务（水流、火焰、自定义）同时跑在后台占内存。因此在 [components/Middlewares/SIM/sim_manager.c](components/Middlewares/SIM/sim_manager.c#L91) 中，我们设计了一个“状态切换机”：

```c
esp_err_t sim_manager_switch(sim_mode_t mode) {
    // 1. 先停掉旧的任务（优雅退出，释放内存）
    stop_mode(s_current);
    
    // 2. 将全局状态设为 NONE，防止冲突
    s_current = SIM_MODE_NONE;

    // 3. 启动新任务（动态创建 Task）
    start_mode(mode);
    
    s_current = mode;
    return ESP_OK;
}
```

---

## 5. 任务同步：原子性与竞态

由于 Web Server 和物理按键都在尝试切换模式，会产生“竞态条件”。我们使用了两种防御手段：
- **互斥锁（Mutex）**：虽然本代码简单未使用，但在多处访问变量时建议加上。
- **原子状态机**：`s_current = SIM_MODE_NONE` 是一个简单有效的保护位，确保切换过程不会被重复打断。

---

## 本课小结

通过本课，你揭开了 ESP32 “同时做多件事”的秘密：
1.  **优先级管理**：物理引擎 > 网络服务 > 按键扫描。
2.  **多核并行**：利用多核优势，将计算与通信分开放置。
3.  **动态生命周期**：任务不是一成不变的，根据需要创建和销毁是节省内存的关键。

---
[👉 下一课：第 20 课：项目总结：从每一行代码到 AIoT 艺术品](curriculum_outline.md)
我们走过了驱动编写、算法实现、网络通信、AI 路由。最后一课我们将回顾整个知识图谱，并探讨如何以此为基础开发更复杂的作品。
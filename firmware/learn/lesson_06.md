# 第 6 课：FreeRTOS 任务创建与生命周期

在前几课中，我们看到了按键检测、重力感应和水流模拟等多个功能。在 ESP32 这种单片机上，它们是如何做到“同时”运行的呢？答案就是 **FreeRTOS**。

本课我们将学习 FreeRTOS 任务（Task）的核心概念，并分析本项目是如何管理这些并发任务的。

---

## 1. 什么是任务 (Task)？

在 FreeRTOS 中，一个任务就是一个无限循环的函数。你可以把它想象成一个独立的“小员工”，每个员工负责一个特定的工作（比如一个负责扫地，一个负责洗碗）。

### 任务的核心要素：
*   **任务函数**：任务执行的具体逻辑。
*   **栈空间 (Stack)**：任务运行时占用的内存。
*   **优先级 (Priority)**：谁的工作更重要？数字越大，优先级越高。
*   **内核绑定 (Core Affinity)**：ESP32 有两个内核（Core 0 和 Core 1），任务可以指定在哪个核上跑。

---

## 2. 任务创建：`xTaskCreate` 与双核调度

打开 [main/main.c](main/main.c)，观察 `app_main` 是如何启动功能的：

### (1) 普通任务创建
```c
xTaskCreate(mode_switch_task, "mode_switch", 3072, NULL, 4, NULL);
```
*   `mode_switch_task`: 任务函数名。
*   `"mode_switch"`: 任务的名字（方便调试）。
*   `3072`: 栈大小（字节）。如果任务里局部变量太多，这里太小会导致“栈溢出”崩溃。
*   `4`: 优先级。

### (2) 双核绑定：`xTaskCreatePinnedToCore`
在 `main.c` 的宏判断中：
```c
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    xTaskCreatePinnedToCore(mode_switch_task, "mode_switch", 3072, NULL, 4, NULL, 0);
#endif
```
*   最后一个参数 `0`: 强制该任务在 **Core 0**（通常用于处理通讯和底层外设）上运行。
*   **本项目策略**：通常将网络（Wi-Fi/HTTP）放在 Core 0，而将高负载的计算（如水流模拟算法）放在 **Core 1**。

---

## 3. 任务的“让权”：`vTaskDelay`

**这是嵌入式开发最重要的一点！**
在一个任务的 `while(1)` 循环中，**必须**调用让出 CPU 的函数，否则低优先级的任务将永远得不到运行，甚至触发看门狗复位。

在 `mode_switch_task` 中：
```c
vTaskDelay(pdMS_TO_TICKS(MODE_KEY_POLL_MS)); 
```
*   `pdMS_TO_TICKS`: 将毫秒转换为操作系统的“滴答（Tick）”数。
*   当执行到这一行时，该任务会进入“阻塞状态”，CPU 会立即去处理其他任务。20ms 后，系统重燃该任务。

---

## 4. 动态销毁：任务的生命周期

本项目的一个高级功能是**模式切换**。当你从“流水”切换到“火焰”时，旧的任务必须干净地关闭。

打开 [Middlewares/SIM/water_sim.c](components/Middlewares/SIM/water_sim.c)，查看停止逻辑：
```c
void water_sim_stop(uint32_t timeout_ms) {
    s_running = false; // 1. 发出停止信号
    // ... 等待任务自毁 ...
}

static void water_sim_task(void* arg) {
    while (s_running) {
        // 渲染逻辑...
    }
    vTaskDelete(NULL); // 2. 逻辑结束，自毁并释放内存
}
```
这种“外部信号 + 内部自毁”的模式是管理动态任务最安全的方式。

---

## 5. 动手实践

1.  **观察优先级影响**：尝试将 `mode_switch_task` 的优先级改为 `1`（极低），并在 `app_main` 里写一个死循环 `while(1);`，你会发现按键完全失效了，因为死循环占满了时间片。
2.  **栈溢出测试**：尝试将任务的栈大小改为 `512`，观察系统是否会抛出 `Stack canary watchpoint triggered` 错误。
3.  **多核分配（思考）**：为什么要把物理模拟算法和 Wi-Fi 处理分在不同的核？（提示：防止复杂的数学计算导致 Wi-Fi 信号的心跳包处理不及时而断连）。

---

## 本课小结
*   FreeRTOS 任务是并发运行的基础。
*   `vTaskDelay` 是任务协作的“灵魂”，严禁在任务中使用死循环阻塞。
*   ESP32 双核可以实现更高效的算力分配。
*   动态任务需要考虑创建 (`xTaskCreate`) 与 销毁 (`vTaskDelete`) 的闭环。

---
[👉 下一课：任务间通信：队列与信号量](curriculum_outline.md#第二部分章节) 建议在理解任务调度后再学习如何让任务之间“说话”。
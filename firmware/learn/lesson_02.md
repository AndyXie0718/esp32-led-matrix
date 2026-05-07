# 第 2 课：GPIO 与按键状态机任务

在嵌入式系统中，GPIO（General Purpose Input/Output）是最基础的外设。本课我们将通过分析 `main/main.c` 中的代码，学习如何利用 ESP32 的 GPIO 进行按键输入，并实现一个**非阻塞**的长按检测状态机。

---

## 1. 硬件连接与 GPIO 配置

在本项目中，我们使用了一个物理按键连接到 ESP32。
*   **引脚定义**：`#define MODE_KEY_PIN GPIO_NUM_0`
*   **逻辑电平**：`#define MODE_KEY_ACTIVE_LOW 1`（表示按键按下时引脚为低电平，这是最常用的接线方式，通常配合内部上拉电阻使用）。

### 为什么选择 GPIO 0？
在 ESP32 系列（尤其是 ESP32-C3/S3）中，GPIO 0 通常是 Boot 按键。在程序运行时，我们可以复用这个按键作为功能切换键，而无需额外焊接外设。

---

## 2. 非阻塞编程思想：为什么不用 delay？

如果你在主循环中使用 `while(key_pressed) { delay(100); }`，CPU 会在这一百毫秒内“卡死”，无法处理流体模拟渲染或 Web 服务器请求。

在本代码中，我们采用的是 **轮询（Polling）+ 任务调度（Task）** 的方式：
*   **采样频率**：`#define MODE_KEY_POLL_MS 20`。每 20 毫秒检查一次按键状态（足以过滤机械抖动并保持响应速度）。
*   **独立任务**：按键检测运行在独立的 `mode_switch_task` 任务中，利用 FreeRTOS 的 `vTaskDelay` 让出 CPU。

---

## 3. 按键状态机逻辑解析

打开 [main/main.c](main/main.c)，找到 `mode_switch_task` 函数，我们可以看到一个简单而严谨的状态逻辑：

### 核心变量：
*   `pressed`: 标记当前按键是否正处于按下状态。
*   `switched`: 标记本次按下是否已经触发过“长按切换”动作（防止一次长按导致模式连续跳变）。
*   `press_tick`: 记录按下的起始时间。

### 逻辑流（伪代码）：
1.  读取 `level = gpio_get_level()`。
2.  **如果按下**：
    *   如果是刚按下：设置 `pressed = true`，记录当前时间 `now`。
    *   如果是一直按着且没触发过：判断 `now - press_tick` 是否超过 1 秒。
    *   超过 1 秒：调用 `sim_manager_switch()` 切换模式，并标记 `switched = true`。
3.  **如果松开**：
    *   重置 `pressed = false` 和 `switched = false`。

---

## 4. 关键 API：`sim_manager_switch`

当长按判定成功后，代码执行了以下切换逻辑：
```c
sim_mode_t cur = sim_manager_current();
sim_mode_t next = SIM_MODE_WATER;
if (cur == SIM_MODE_WATER) {
    next = SIM_MODE_FIRE;
} else if (cur == SIM_MODE_FIRE) {
    next = SIM_MODE_CUSTOM;
}
sim_manager_switch(next);
```
这体现了**松耦合**的设计思想：按键检测任务不关心“火焰”或“流水”是怎么画出来的，它只负责发出一个“切换到模式 X”的指令。

---

## 5. 动手实践

1.  **改变长按时间**：尝试将 `MODE_SWITCH_LONG_PRESS_MS` 从 `1000` 改为 `200`，编译并观察按键是否变得过于灵敏。
2.  **增加单击逻辑（思考）**：现在的代码只支持长按切换。如果要在单击时改变 LED 的整体亮度，你应该在“松开”逻辑中增加什么样的判断？（提示：判断松开时的持续时间是否小于长按阈值）。

---

## 本课小结
*   GPIO 输入需要考虑有效电平（Active Low/High）。
*   多任务环境下避免使用阻塞式的 `delay`。
*   利用 `TickCount` 差异可以实现精确的时间判定状态机。

---
[👉 下一课：I2C 协议与 MPU6050 传感器](curriculum_outline.md#第一部分章节) 建议在熟悉按键任务后再阅读。
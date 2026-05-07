# 第 7 课：任务间通信：竞争与同步（自旋锁）

在前一课中，我们学习了如何创建并行运行的任务。今天我们将直面多任务开发中最棘手的问题：**数据竞争（Race Condition）**，并分析本项目是如何利用 **自旋锁（Spinlock / Critical Section）** 安全地在不同任务间传递重力感应数据的。

---

## 1. 为什么“直接赋值”是不安全的？

想象一下：
1.  **任务 A（重力感应任务）** 正在向全局变量 `s_gx` 写入一个 32 位的浮点数。
2.  由于 CPU 执行非常快，它可能刚写完前 16 位，还没来得及写后 16 位。
3.  就在这万分之一秒，**任务 B（渲染引擎任务）** 刚好被系统唤醒，去读取 `s_gx`。
4.  **结果**：任务 B 读到的数据一半是旧的，一半是新的，导致物理模拟出现严重的异常脉冲（例如水流突然瞬移）。

这种两个任务同时访问同一块内存导致的问题，就叫**数据竞争**。

---

## 2. 本项目的解决方案：临界区（Critical Section）

打开 [components/Middlewares/GRAVITY/gravity.c](components/Middlewares/GRAVITY/gravity.c)，我们可以看到它是如何保护数据的：

### (1) 定义一把“锁”
```c
// 定义一个端点互斥量（在 ESP-IDF 中常用于保护极简数据的“自旋锁”）
static portMUX_TYPE s_gravity_mux = portMUX_INITIALIZER_UNLOCKED;

// 用于存储重力数据的全局变量（被保护的对象）
static float s_gx = 0.0f;
static float s_gy = 0.0f;
```

### (2) 安全地写入数据（生产者）
当重力任务采集到新数据时，它不会直接赋值，而是先“上锁”：
```c
/**
 * @brief 更新重力分量，该函数被重力感应任务调用
 */
void gravity_set(float gx, float gy) {
    // 【进入临界区】：
    // 这行代码会关闭当前 CPU 的调度，甚至是中断，确保接下来的操作是“原子”的
    portENTER_CRITICAL(&s_gravity_mux);
    
    s_gx = gx; // 在锁的保护下安全赋值
    s_gy = gy;
    s_valid = true;
    
    // 【退出临界区】：
    // 操作完成，释放锁，允许其他核或任务访问
    portEXIT_CRITICAL(&s_gravity_mux);
}
```

### (3) 安全地读取数据（消费者）
物理引擎（渲染任务）在获取数据时，也必须遵守同样的规则：
```c
/**
 * @brief 获取当前重力分量，被渲染引擎调用
 */
void gravity_get(float* gx, float* gy) {
    portENTER_CRITICAL(&s_gravity_mux); // 申请访问权
    
    if (gx) *gx = s_gx; // 拷贝数据到局部变量
    if (gy) *gy = s_gy;
    
    portEXIT_CRITICAL(&s_gravity_mux); // 归还访问权
}
```

---

## 3. 为什么不用队列（Queue）？

可能有同学听过 FreeRTOS 的“队列”。为什么这里不用队列？
*   **队列**：适合传递指令或事件。如果你需要“每一个按键动作都不丢失”，请用队列。
*   **自旋锁 + 全局变量**：适合传递**最新的状态值**。对于物理引擎来说，它只关心“此刻”的重力是多少，并不关心 1 毫秒前的历史。这种方式开销极低，响应极快。

---

## 4. 动手实践：感受“竞争”的后果（模拟思考）

1.  **移除保护实验**：尝试在 `gravity.c` 中临时注释掉 `portENTER_CRITICAL` 和 `portEXIT_CRITICAL`。
2.  **极端测试**：在 Web 控制台快速切换各种复杂的模式。虽然 8x8 屏幕上由于数据量小可能不明显，但在更复杂的 64x64 甚至更高的物理计算中，你会观察到偶尔的“屏幕撕裂”或流体炸裂，这就是没加锁的后果。

---

## 本课小结
*   多核/多任务环境下，**共享数据必须保护**。
*   **临界区 (Critical Section)** 是保护极简数据（如 float, int）最轻量高效的方式。
*   `portENTER_CRITICAL` 会暂时“冻结”系统调度，因此内部的代码必须**极短且快**（严禁在里面打印日志或延时）。
*   遵循“读写对称”原则：写入要加锁，读取也必须加锁。

---
[👉 下一课：非易失性存储 (NVS) 实战](curriculum_outline.md#第二部分章节) 准备好让你的设备学会“记忆”了吗？
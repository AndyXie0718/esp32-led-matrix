# 第 3 课：I2C 协议与 MPU6050 传感器

在本项目中，要让 LED 屏幕上的“水滴”随板子倾斜而流动，重力感应是核心。本课我们将深入探讨如何通过 I2C 协议驱动 MPU6050 传感器，并将原始数据转化为可用的重力分量。

---

## 1. I2C 总线基础

I2C（Inter-Integrated Circuit）是一种两线式串行总线，由两条线组成：
*   **SDA (Serial Data)**：串行数据线。
*   **SCL (Serial Clock)**：串行时钟线。

### 本项目中的引脚配置：
打开 [Middlewares/GRAVITY/gravity.c](components/Middlewares/GRAVITY/gravity.c)，我们可以看到引脚定义：
*   **SCL**: GPIO 4
*   **SDA**: GPIO 3
*   **频率**: 400kHz (快速模式)

---

## 2. 硬件初始化流程

### (1) I2C 控制器配置
代码通过 `i2c_bus_init_once()` 函数配置 ESP32 为 **Master（主机）** 模式：
```c
i2c_config_t conf = {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = 3,
    .scl_io_num = 4,
    .sda_pullup_en = GPIO_PULLUP_ENABLE, // 使用内部上拉电阻
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 400000,
};
```

### (2) MPU6050 设备初始化
MPU6050 是一个集成了 3 轴加速度计和 3 轴陀螺仪的芯片。
*   **I2C 地址**: 默认为 `0x68`。
*   **唤醒**: 芯片开机默认是睡眠模式，驱动程序需要向电源管理寄存器写入数据将其唤醒。

---

## 3. 读取加速度数据与低通滤波

在 `gravity_sensor_task` 任务中，程序以 **50Hz (20ms/次)** 的频率读取数据。

### (1) 获取原始数据
```c
mpu6050_acce_value_t acce;
mpu6050_get_acce(s_mpu, &acce); // 获取 X, Y, Z 三轴加速度（单位通常为 g）
```

### (2) 低通滤波 (Low Pass Filter)
由于传感器存在噪声且我们手抖会产生高频振动，直接使用数据会导致 LED 像素乱跳。代码采用了简单的**一阶 IIR 低通滤波**算法：
```c
// alpha 越小，滤波越平滑，但延迟越高
// s_gx = s_gx * (1 - alpha) + new_data * alpha
gx_f = gx_f * (1.0f - SENSOR_LPF_ALPHA) + (acce.acce_x * SENSOR_LPF_ALPHA);
gy_f = gy_f * (1.0f - SENSOR_LPF_ALPHA) + (acce.acce_y * SENSOR_LPF_ALPHA);
```
这段代码确保了“重力方向”的改变是平滑过渡的。

---

## 4. 重力分量的数学意义

想象板子平放：
*   $Z$ 轴加速度 $\approx 1g$（指向地心）。
*   $X, Y$ 轴加速度 $\approx 0$。

当板子向左倾斜：
*   重力的分量会转换到 $X$ 轴上。依靠读取到的 `acce_x` 和 `acce_y` 的值，物理引擎（如 `FLIP`）就能算出粒子应该往哪个方向“滚”。

---

## 5. 动手实践

1.  **观察日志**：在 `gravity_sensor_task` 中添加 `ESP_LOGI(TAG, "AX: %.2f, AY: %.2f", acce.acce_x, acce.acce_y);`，编译运行并在串口监视器中旋转板子，观察数值变化。
2.  **调整灵敏度**：修改 `gravity.c` 中的 `SENSOR_LPF_ALPHA`。
    *   将其改为 `1.0f`（无滤波），看看水滴流动是否变得非常“急促”且伴随抖动。
    *   将其改为 `0.05f`（极强滤波），看看操作感是否变得非常“迟钝”。

---

## 本课小结
*   I2C 是连接传感器最常用的协议。
*   原始传感器数据必须经过**软件滤波**才能用于物理模拟。
*   加速度计的 $X/Y$ 分量直接决定了 UI 特效的受力方向。

---
[👉 下一课：WS2812 驱动原理与 RMT 外设](curriculum_outline.md#第一部分章节) 建议在理解传感器数据流后再继续。
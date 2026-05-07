# 第 9 课：姿态解算与重力分量提取

欢迎进入第三部分：**数学与物理模拟引擎**。

在控制 LED 屏幕上的流水和火焰之前，我们需要一个“驱动力”。在这个项目中，驱动力就是真实世界的**重力**。本课我们将深入解析 `components/Middlewares/GRAVITY/gravity.c` 中的代码，看看原始的三轴加速度是如何一步步转换为物理引擎能够使用的“上帝之手”的。

---

## 1. 从传感器原始数据到“标准 g”

加速度计测量的是设备受到的整体加速度（包含重力和设备运动本身产生的加速度）。MPU6050 驱动库返回的数据通常以 $m/s^2$ 为单位（在地球表面，静止时的重力加速度约等于 $9.81 m/s^2$）。

然而，物理引擎（后续的 FLIP 流体算法）更喜欢在特定的数值区间（如 `-1.0 ~ 1.0`）内工作，因此我们需要对其进行**归一化（Normalization）**换算。

### 源码解析：`normalize_to_g`
```c
/**
 * @brief 将原始加速度转换为以 "g" (标准重力) 为单位的值
 * @param ax, ay, az 分别是指向三轴加速度的指针
 */
static inline void normalize_to_g(float* ax, float* ay, float* az) {
    // 1. 计算三维空间中的向量模长 (勾股定理：sqrt(x^2 + y^2 + z^2))
    float mag = sqrtf((*ax) * (*ax) + (*ay) * (*ay) + (*az) * (*az));
    
    // 2. 判断单位：如果模长大于 5.0，说明原始数据可能是以 m/s^2 为单位提供的
    // （因为标准 1g 就是 9.8，远大于 5.0。如果本来就是以 g 为单位，mag 应该在 1.0 左右）
    if (mag > 5.0f) {
        // 将 m/s^2 转换为 g
        const float inv_g = 1.0f / 9.80665f; 
        *ax *= inv_g;
        *ay *= inv_g;
        *az *= inv_g;
    }
}
```
经过这一步转换，当我们把板子水平静止放置时，$Z$ 轴的值将变成约 `1.0`，而把板子垂直立起来时，$X$ 或 $Y$ 轴的值将变成约 `1.0` 或 `-1.0`。

---

## 2. 轴向映射与符号校正 (Axis Mapping)

在实际的硬件产品设计中，芯片焊接的方向往往和 LED 屏幕的方向不一致（例如屏幕的正上方，在传感器看来可能是 X 轴的负方向）。

为了让“流动方向”与屏幕视觉对应，代码通过宏定义了一套映射规则：
```c
// 决定最终重力矢量的 X 是取自传感器的 X 还是 Y
#define GX_FROM_AX 1
#define GY_FROM_AY 1

// 符号校正：如果翻转了方向，乘以 -1 即可修正
#define GX_SIGN (-1.0f)
#define GY_SIGN (1.0f)
```
在 `gravity_sensor_task` 中，这套逻辑被翻译成：
```c
// 提取 X 和 Y 轴（这里 Z 轴被舍弃了，因为我们的 LED 矩阵是一个二维平面）
#if GX_FROM_AX
    gx = ax;
#else
    gx = ay;
#endif

// 符号反转适应安装角度
gx *= GX_SIGN;  
gy *= GY_SIGN;
```

---

## 3. 防爆音机制：限幅保护 (Clamping)

我们知道，当用力甩动开发板时，加速度计测到的瞬间加速度可能高达 `3g` 或 `5g`。
如果直接把这种几倍于正常重力的极值喂给流体引擎，**粒子在模拟计算时会因为受力过大而“飞出”边界（数值爆炸）**。

因此，代码使用了 `clampf_fast` 来进行**限幅（钳位）**：
```c
#define G_CLAMP 1.5f

/**
 * @brief 快速限幅函数：确保 x 不小于 lo，也不大于 hi
 */
static inline float clampf_fast(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

// 实际应用：将重力分量强制限制在 -1.5g 到 1.5g 之间
gx = clampf_fast(gx, -G_CLAMP, G_CLAMP);
gy = clampf_fast(gy, -G_CLAMP, G_CLAMP);
```
这意味着无论你怎么用力甩板子，流体引擎感受到的最大“风暴”也不会超过 `1.5` 个重力。

---

## 4. 全链路拼图 (The Pipeline)

结合我们在第 3 和第 7 课学到的滤波与自旋锁机制，最终的核心采集逻辑如下：

```c
// 在重力任务 while(1) 循环中：
mpu6050_get_acce(s_mpu, &acce);

float ax = acce.acce_x;
float ay = acce.acce_y;
float az = acce.acce_z;

// 1. 单位转换到标准 g
normalize_to_g(&ax, &ay, &az);

// 2. 映射与符号校验
float gx = ax * GX_SIGN;
float gy = ay * GY_SIGN;

// 3. 限幅防爆
gx = clampf_fast(gx, -1.5f, 1.5f);
gy = clampf_fast(gy, -1.5f, 1.5f);

// 4. 一阶低通滤波 (滤除高频噪声)
gx_f = (1.0f - SENSOR_LPF_ALPHA) * gx_f + SENSOR_LPF_ALPHA * gx;
gy_f = (1.0f - SENSOR_LPF_ALPHA) * gy_f + SENSOR_LPF_ALPHA * gy;

// 5. 加锁写入全局保护变量，通知物理引擎
gravity_set(gx_f, gy_f);
```

---

## 5. 动手实践

1.  **方向倒错测试**：如果你觉得点阵屏幕上水流的方向反了，尝试在 `gravity.c` 中把 `#define GX_SIGN (-1.0f)` 改成 `(1.0f)`。
2.  **疯狂抖动测试**：把 `#define G_CLAMP` 的值从 `1.5f` 改为 `10.0f`。用力摇晃开发板，看看在这强大的受力下，水滴模式会不会因为粒子乱窜而彻底“崩溃”。
3.  **日志观察**：利用我们在第一课学到的串口监视器，观察 `ax` 和编译后的 `gx_f` 的数值变化，直观感受限幅和滤波的作用。

---
## 本课小结
*   计算前要统一单位空间：将 `m/s^2` 换算为人类直觉得到的 `g`（重力加速度）。
*   **硬件解耦**：通过编译宏 (`GX_FROM_AX` 等) 使得不管传感器焊成什么方向，代码逻辑都能自洽。
*   **限幅 (Clamping)** 是保证后续复杂物理数值解算能够收敛、不“爆炸”的基础防线。

---
[👉 下一课：火焰特效：元胞自动机算法](curriculum_outline.md#第三部分章节) 从物理世界回到算法世界，看看那团火是怎么“烧”起来的！
# 第 11 课：流体力学基础：粒子法与网格法

在体验过 DOOM Fire 模拟（基于网格元胞）后，我们将进入视觉效果更震撼、计算也更复杂的领域：**流体力学模拟（水流特效）**。

在本项目 `water_sim.c` 及底层的 `FLIP` 中间件里，我们实现了一个二维的微型水面物理引擎，并且让这汪水完美响应现实中的重力倾斜。

---

## 1. 模拟流体的两种视角：拉格朗日法 vs 欧拉法

要用单片机模拟水流，我们首先得学会如何用数据去“定义”水。在物理界有两大经典流派：

1.  **拉格朗日视角 (Lagrangian, 粒子法)**：
    *   **核心思维**：把水看成无数个“水分子（小球）”。我们追踪每一个小球的坐标、速度、加速度。
    *   **优点**：能量守恒好，能轻易做出飞溅、水滴的视觉效果。
    *   **缺点**：粒子聚集时很难计算它们互相挤压的压力，容易爆炸、体积不守恒。

2.  **欧拉视角 (Eulerian, 网格法)**：
    *   **核心思维**：把空间划分成一个个固定的网格（就像我们前面看到的火焰像素）。我们不去追踪水分子，而是坐在格子里看：**此刻流过我这个格子的水，密度是多少？流速往哪边走？**
    *   **优点**：求解压力极其稳定，不可压缩流体（比如水）的体积保持得很好。
    *   **缺点**：因为是在固定网格间计算平流传递，算久了流体会受到数值损耗变得“粘稠”，很难表现锐利的水花飞散。

### 本项目的杀手锏：FLIP 算法（混合法）

本项目使用的核心引擎基于 **FLIP (Fluid-Implicit Particle)** 算法。它结合了以上两派的优点：
*   **算密度和压力**：用网格（Eulerian），保证水流不聚集、不被压扁变形。
*   **记录位置和速度**：用粒子（Lagrangian），让水花能到处飞溅不损失细节。

---

## 2. 源码解析：Water Simulation RTOS 任务

打开源码 [components/Middlewares/SIM/water_sim.c](components/Middlewares/SIM/water_sim.c)，这是连接硬件重力仪和 FLIP 算法的桥梁。

### (1) 流体引擎的初始化
在进入 `while(s_running)` 的无限循环前，我们需要分配流体世界的内存，并设定物理参数：
```c
// f 是一个指向 FlipFluid 世界实例的指针
// 参数含义：宽度比例(1.0)，高度比例(1.0)，分辨率(横向LED数目，即W)，填充率(0.6即装60%的水)
f = flip_create(1.0f, 1.0f, W, 0.6f);
if (!f) {
    ESP_LOGE(TAG, "flip_create failed");
    goto exit_task;
}

// 设置环境参数，地球重力的基础缩放
flip_set_gravity_scale(f, 9.81f);

// 性能与质量的平衡设定（这是单片机优化的关键）
int dyn_push_iters = 1;       // 碰撞检测迭代次数 (数字小=少占CPU)
int dyn_pressure_iters = 10;  // 压力求解器的迭代次数 (控制水的不可压缩性)
const float dyn_flip_ratio = 0.9f; 
flip_set_solver_quality(f, dyn_push_iters, dyn_pressure_iters, dyn_flip_ratio);
```

> **思考：为什么需要压力求解迭代？**  
> 因为在一帧里可能会有很多水粒子因为重力掉进了同一个格子里。我们需要让它们“互相嫌弃”并反弹出去（把水往周围没装满的格子里挤）。迭代 10 次就是做 10 遍“检查拥挤 $\to$ 散开”的操作，次数越多水越硬，但计算越慢。

### (2) 主循环：接入重力，推动时间
代码里使用 FreeRTOS 的 `vTaskDelayUntil` 来维系绝对严格的帧率（FPS = 30）。
```c
const float dt = 1.0f / (float)SIM_FPS; // 每一帧代表的时间步长 (约0.033秒)

while (s_running) {
    vTaskDelayUntil(&last_wake, frame_ticks); // 准时睡醒

    // 1. 获取带有互斥锁保护的硬件传感器重力矢量
    gravity_xy_t g = gravity_get();
    float gx = g.valid ? g.gx : 0.0f;
    float gy = g.valid ? g.gy : 0.0f;

    // 2. 物理学魔法时刻：向 FLIP 传递这0.033秒内，所有水分子受到的重力
    // 此时底层会完成：粒子速度叠加重力 -> 映射到网格 -> 算压力 -> 解拥挤 -> 映射回粒子 -> 更新粒子坐标
    flip_step(f, dt, gx, gy);

    // 3. 把粒子化作浓浊的“水深网格”(LED上的灰度值)
    flip_get_led_grid(f, grid);
    
    // ... 后续接管渲染代码 (类似火焰中的光栅化流程)
}
```

---

## 3. 从虚拟格子到 LED 实物颜色的渲染
`flip_get_led_grid` 会把粒子存在的密度转换为一个长为 `LED_COUNT` 的浮点数数组。在 `water_sim.c` 的下半部分：
```c
for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
        float v = grid_get(grid, x, y); // 获取该位置的“水深”或密度
        
        // 防呆保护，过滤非有限(NaN)和负数
        if (!isfinite(v)) v = 0.0f;
        if (v < 0.0f) v = 0.0f;
        
        // 缩放并限幅到 0 ~ 255 的 LED 亮度级别
        if (v > LED_VAL_MAX_F) v = LED_VAL_MAX_F;
        int lv = (int)(v + 0.5f); // 浮点四舍五入转整数

        // 像火焰课一样，这里也用了空间换时间的 LUT 查找表
        rgb8_t c = s_pal_lut[s_palette_idx][lv];

        // 最终调用底层驱动
        uint16_t idx = panel_led_index(x, y);
        rgb_set_fast((uint32_t)idx, c.r, c.g, c.b);
    }
}
```

---

## 本课小结
*   物理引擎本质上是对时间微分($dt$)不断**数值积分**的过程。只要 $dt$ 足够小，算出来的效果就接近真实物理定律。
*   在嵌入式系统上跑流体力学非常吃力，我们通过平衡 `dyn_pressure_iters` (压力求解迭代次数) 来在**画面稳定性**和 **CPU负载** 之间寻找出路。
*   代码中的高内聚设计：只把 `(dt, gx, gy)` 暴露给具体的应用层，隐藏了内部极其复杂的稀疏矩阵运算。

---
[👉 返回课程大纲](curriculum_outline.md)
我们已经完成了大部分核心原理解析，你觉得我们下一节是继续深挖物理引擎如何做**边界检测与碰撞**呢，还是探索如何与大模型结合进行**AI语音控制灯光**呢？
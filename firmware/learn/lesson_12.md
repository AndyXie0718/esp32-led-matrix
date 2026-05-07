# 第 12 课：流体碰撞处理与物理步进

上一课中，我们见识了混合粒子与网格的 **FLIP (Fluid-Implicit Particle)** 算法的宏观调用。今天，我们将深入引擎内部，探讨两个最基础、但也最致命的物理引擎细节：**碰撞边界约束**与**时间离散化（步进 dt）**。

如果你曾经写过哪怕最简单的“弹球”代码，一定遇到过球飞出屏幕外（穿模）的 bug。水流模拟更是如此，只不过它有几百个“球”挤在一起。

---

## 1. 碰撞检测与绝对边界 (Boundary Conditions)

在我们的 8x8 LED 矩阵周围，实际上存在着一圈无形的玻璃墙。如果不管它们，水流会受重力影响一直往下掉，直到导致程序内存越界崩溃。

打开 [components/Middlewares/FLIP/flip.c](components/Middlewares/FLIP/flip.c#L201)，找到 `handle_particle_collisions` 函数，这是物理引擎最末端、最铁腕的“防线”。

```c
static void handle_particle_collisions(int n, float* pos, float* vel,
                                       float f_inv_spacing, int f_num_x,
                                       int f_num_y, float particle_radius) {
    float h = 1.0f / f_inv_spacing;
    float r = particle_radius;

    // 计算出上下左右的绝对墙壁边界坐标
    // h 代表一层网格的厚度（墙壁网格），r 是粒子本身的半径
    float min_x = h + r;
    float max_x = (f_num_x - 1) * h - r;
    float min_y = h + r;
    float max_y = (f_num_y - 1) * h - r;

    // 遍历每一个水分子粒子
    for (int i = 0; i < n; i++) {
        float x = pos[2 * i + 0]; // X 坐标
        float y = pos[2 * i + 1]; // Y 坐标

        // --- X 轴边界检测 ---
        if (x < min_x) {
            x = min_x;               // 强制拉回墙壁内部（防止穿模）
            vel[2 * i + 0] = 0.0f;   // 完全非弹性碰撞：拍在墙上瞬间失去 X 轴速度
        }
        if (x > max_x) {
            x = max_x;
            vel[2 * i + 0] = 0.0f;
        }

        // --- Y 轴边界检测 ---
        if (y < min_y) {
            y = min_y;
            vel[2 * i + 1] = 0.0f;
        }
        if (y > max_y) {
            y = max_y;
            vel[2 * i + 1] = 0.0f;
        }

        // 写回更新后的安全坐标
        pos[2 * i + 0] = x;
        pos[2 * i + 1] = y;
    }
}
```

> **算法思维：**
> 这里采用的是**完全非弹性碰撞**（撞墙后速度清零，而不是反弹）。在流体网格计算中，如果是反弹（`vel = -vel`），大量的粒子在墙角反弹会导致不可抗拒的高频振荡（水面沸腾）。直接让撞墙的粒子“停下”，配合后续的不可压缩网格求解，能产生极其粘滞、平滑的水杯玻璃触感。

---

## 2. 物理步进与时间离散化 (Physical Stepping)

现实世界的时间是连续的，但代码世界的时间是一顿一顿的（离散化）。物理引擎把时间切碎，每一刀的间隔被称为 **时间步（Delta Time, 缩写为 dt）**。

在 [components/Middlewares/FLIP/flip.c](components/Middlewares/FLIP/flip.c#L73) 的 `integrate_particles` 运算中：
```c
static void integrate_particles(int n, float* pos, float* vel, float dt,
                                float gx, float gy) {
    const float dgx = gx * dt;
    const float dgy = gy * dt;
    
    // 省略了 ESP-DSP 加速库调用细节...
    
    // 物理学圣经：牛顿运动学微积分
    for (int i = 0; i < n; i++) {
        // v = v0 + a * dt (速度加上这段时间带来的重力加速度)
        // 省略了上面计算的 dsps_addc_f32 矢量加法

        // s = s0 + v * dt (坐标加上这段时间移动的距离)
        pos[2 * i + 0] += vel[2 * i + 0] * dt;
        pos[2 * i + 1] += vel[2 * i + 1] * dt;
    }
}
```

### 为什么 `dt` 的稳定性至关重要？
如果在上一课的 `water_sim.c` 中，我们的 FreeRTOS 任务因为被其他高优先级任务抢占，导致这一帧卡了，实际过去了 `0.1秒`，但程序还当做 `1/30` 秒处理，速度就会变慢（慢动作）。
反过来，如果是真实计算变大了，且系统直接用真实 `dt=0.1秒` 扔给上面的方程，会引发典型的 **CFL (Courant–Friedrichs–Lewy) 灾难**：
> 这一帧粒子的速度极其快，经过 `v * dt` 计算后，粒子的下一步位置直接跨越了上面讲到的 `min_x` 和 `max_x` 墙壁的连线，瞬间“隧道效应”遁出了屏幕！进而导致数组越界，程序重启。

**防呆设计：** 所以本系统中强制锁死了 `const float dt = 1.0f / SIM_FPS;`，不管真实现实世界过去多久，流体世界的物理只推进固定的时间量，用 `vTaskDelayUntil` 来维系系统的帧率严格对齐现实，杜绝了物理引擎爆炸的可能。

---

## 3. 看懂一帧画面的渲染管线 (Pipeline)

结合前两节内容，一帧画面产生的生命周期在 `flip_step` 中被严格执行：

1.  `integrate_particles()`：根据重力和 dt 更新每一滴水的位置。
2.  `push_particles_apart()`：检测拥挤，将相互之间贴得太近的粒子稍微推开一点。
3.  `handle_particle_collisions()`：铁腕纠正，凡是被推移到屏幕外的粒子，强行拉回来！
4.  `transfer_velocities()` 与 `solve_incompressibility()`：将水滴映射到上帝视角的网格求解压力，保持整杯水的体积不会被压缩。

这就是工业标准的二维流体力学引擎的运行骨架，它在极度袖珍的 ESP32 单片机上高效运转着。

---
[👉 下一课：第 13 课：平滑切换：引擎状态调度器](curriculum_outline.md)
我们写了火焰、又写了水流，这么多特效堆在一起，ESP32 内存不够用了怎么办？下一节，我们将学习如何构建一个“状态机/模拟管理器”，在特效之间丝滑切换，释放枯竭的 RAM 内存。
# 第 4 课：WS2812 驱动原理与 RMT/SPI 外设

前面的课程中我们已经拿到了重力传感器的数值。现在，我们需要把物理特效转换成视觉信号，点亮板子上的 8x8 LED 矩阵。本课我们将深入探讨 **WS2812 协议** 以及 ESP32 如何利用底层硬件高效驱动灯条。

---

## 1. WS2812 通信协议：单线之美

WS2812（通常被称为 NeoPixel）是一种内置控制 IC 的彩色 LED。它的魅力在于：只需 **1 根信号线** 就能串联控制成百上千个灯珠。

### 关键特性：
*   **归零码时序**：它不依赖时钟线，而是靠高低电平的**持续时间**来区分数据 `0` 和 `1`。
    *   **高电平长、低电平短** = 数据 `1`
    *   **高电平短、低电平长** = 数据 `0`
*   **级联传输**：每个灯珠“吃掉”接收到的前 24 位（G-R-B 颜色），然后把剩余的数据传给下一个灯珠。

---

## 2. ESP32 驱动方案：为什么用 SPI/RMT？

由于 WS2812 的时序极其精确（纳秒级），用普通 GPIO 翻转电平（Bit-banging）会严重占用 CPU。本项目使用了 ESP32 特有的 **SPI (Serial Peripheral Interface)** 硬件模拟方案。

### 驱动代码解析：
打开 [components/BSP/RGB/rgb.c](components/BSP/RGB/rgb.c)，可以看到初始化逻辑：
```c
led_strip_config_t strip_config = {
    .strip_gpio_num = 10,       // 连接到灯板的引脚 (GPIO 10)
    .max_leds = 64,             // 8x8 = 64 个灯珠
    .led_model = LED_MODEL_WS2812,
    .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
};

led_strip_spi_config_t spi_config = {
    .spi_bus = SPI2_HOST,       // 使用硬件 SPI2 控制器
    .flags.with_dma = true,      // 启用 DMA（直接内存访问），不占 CPU 刷新
};
```
**亮点**：通过启用 **DMA**，CPU 只需准备好颜色数据并“下达指令”，专门的硬件电路就会自动把数据转换成 WS2812 的单线时序，后台自动完成刷新。

---

## 3. 面向安全的显示保护：亮度限制与全局调光

LED 全亮时功耗极高（每个灯珠全白约 60mA，64 个灯珠接近 4A），可能会导致 USB 供电不足或烧坏引脚。本项目代码中实现了两层保护机制：

### (1) 全局颜色映射 (`apply_global_color`)
通过 Web 服务器发出的全局颜色（s_global_r8 等）会与特效颜色做乘法：
$$Target = (Pixel \times Global) / 255$$
这实际上起到了**总亮度调节**的作用。

### (2) 硬性限流 (`limit_panel_max`)
在 [rgb.c](components/BSP/RGB/rgb.c) 中：
```c
#define PANEL_LED_VALUE_MAX 20 // 定义最大亮度（最高 255）
```
如果任意通道超过 20，代码会自动等比例压缩三色值。这种**等比例压缩**保证了颜色不会因为亮度截断而失色。

---

## 4. 坐标变换：2D 网格到 1D 灯条

物理上的灯珠是按“S型”或“列优先”布线的，但在代码里我们更习惯用 `(x, y)` 坐标。

打开 [components/BSP/RGB/panel_config.h](components/BSP/RGB/panel_config.h)，查看映射函数：
```c
static inline int panel_led_index(int x, int y) {
#if PANEL_COLUMN_MAJOR
    return x * PANEL_HEIGHT + y; // 本项目采用列优先布线
#else
    return y * PANEL_WIDTH + x;  // 标准行优先布线
#endif
}
```
这意味着当你调用 `rgb_set_pixel(2, 3)` 时，底层会自动将其转换为灯带上的第 `(2*8 + 3) = 19` 个灯珠。

---

## 5. 动手实践

1.  **改变亮度上限**：在 `panel_config.h` 中找到 `PANEL_LED_VALUE_MAX`，将其从 `20` 尝试修改为 `50`（注意供电安全！），观察灯板是否亮度大幅提升。
2.  **测试布线逻辑**：如果你的灯板显示图像是倒转的，尝试在 `panel_led_index` 中修改计算公式（例如将 `y` 改为 `PANEL_HEIGHT - 1 - y`）。
3.  **色彩实验**：在 `main.c` 初始化后，尝试循环调用 `rgb_set` 函数，手动点亮前 3 个像素点分别为 红、绿、蓝，确认 G-R-B 顺序是否正确。

---

## 本课小结
*   WS2812 是一种精密时序的单线协议。
*   ESP32 利用 SPI+DMA 实现零 CPU 占用的显示刷新。
*   通过 `apply_global_color` 和硬件限流限压保护电路安全。
*   `panel_led_index` 负责将逻辑坐标映射到物理硬件。

---
[👉 下一课：二维像素映射与坐标变换](curriculum_outline.md#第一部分章节) 建议在点亮第一个像素后继续！
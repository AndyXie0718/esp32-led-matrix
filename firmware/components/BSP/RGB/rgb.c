#include "rgb.h"

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/portmacro.h"
#include "led_strip.h"
#include "panel_config.h"

/* 驱动引脚与硬件参数配置 */
#define RGB_GPIO_PIN 10                 // WS2812 的数据控制引脚 (DI)
#define RGB_LED_COUNT PANEL_LED_COUNT   // 矩阵上的 LED 总数量
#define RGB_SPI_HOST SPI2_HOST          // 使用 SPI2 外设来驱动 WS2812 (利用 SPI+DMA 可以极大降低 CPU 负担)

static led_strip_handle_t s_strip = NULL; // led_strip 组件的句柄
static uint8_t s_global_r8 = 255;         // 全局色彩滤镜/比例：Red (0~255)
static uint8_t s_global_g8 = 255;         // 全局色彩滤镜/比例：Green (0~255)
static uint8_t s_global_b8 = 255;         // 全局色彩滤镜/比例：Blue (0~255)
static portMUX_TYPE s_color_mux = portMUX_INITIALIZER_UNLOCKED; // 互斥锁，用于在多任务或中断中保护全局颜色变量和并发访问

/**
 * @brief 应用全局颜色/亮度滤镜
 * @details 把传入的原始 r,g,b 与全局 s_global_x8 配置按比例相乘。
 *          起到一种类似于 "全局亮度控制" 或者是 "全局调色盘" 的效果。
 * @param r Red 分量指针
 * @param g Green 分量指针
 * @param b Blue 分量指针
 */
static inline void apply_global_color(uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t gr;
    uint8_t gg;
    uint8_t gb;

    // 进入临界区，保证在读取全局参数时不被打断
    portENTER_CRITICAL(&s_color_mux);
    gr = s_global_r8;
    gg = s_global_g8;
    gb = s_global_b8;
    portEXIT_CRITICAL(&s_color_mux);

    // 计算实际发出比例：(原始值 * 全局比例) / 255
    // 强制转换为 uint16_t 是为了防止乘法过程中的 8-bit 溢出
    *r = (uint8_t)(((uint16_t)(*r) * gr) / 255u);
    *g = (uint8_t)(((uint16_t)(*g) * gg) / 255u);
    *b = (uint8_t)(((uint16_t)(*b) * gb) / 255u);
}

/**
 * @brief 限制面板最大亮度输出
 * @details 保护机制：WS2812 矩阵全亮时电流非常大，为了防止烧毁线材或触发过流保护，
 *          将最大的通道强制等比例压缩到 PANEL_LED_VALUE_MAX 范围内。
 */
static inline void limit_panel_max(uint8_t* r, uint8_t* g, uint8_t* b) {
    // 找出 R, G, B 中的最大值
    uint8_t m = *r;
    if (*g > m) {
        m = *g;
    }
    if (*b > m) {
        m = *b;
    }

    // 如果最大值突破了系统设定的硬件安全最大阈值
    if (m > PANEL_LED_VALUE_MAX) {
        // 等比例把它压回来，保证色相不变的前提下降低整体亮度
        *r = (uint8_t)(((uint16_t)(*r) * PANEL_LED_VALUE_MAX) / m);
        *g = (uint8_t)(((uint16_t)(*g) * PANEL_LED_VALUE_MAX) / m);
        *b = (uint8_t)(((uint16_t)(*b) * PANEL_LED_VALUE_MAX) / m);
    }
}

/**
 * @brief 综合应用全彩色滤镜和硬件电流限制
 */
static inline void apply_global_and_panel_limit(uint8_t* r, uint8_t* g, uint8_t* b) {
    apply_global_color(r, g, b); // 第一步：全局过滤
    limit_panel_max(r, g, b);    // 第二步：硬件限流保护
}

/**
 * @brief 初始化 WS2812 LED 等效硬件与驱动底座
 */
void rgb_init(void) {
    // 避免重复初始化
    if (s_strip) {
        return;
    }

    // 基本条带配置
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO_PIN,                  // 控制引脚
        .max_leds = RGB_LED_COUNT,                       // 最大灯珠数
        .led_model = LED_MODEL_WS2812,                   // 定位型号
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // 协议色序 (WS2812 原生使用的是 GRB 格式而不是 RGB)
        .flags = {
            .invert_out = false,                         // 信号不反相
        }};

    // 底层外设配置 (使用 SPI + DMA)
    // 相比使用 RMT，使用 SPI DMA 在驱动大型矩阵面板时效能极佳，因为无需 CPU 一直参与打节拍
    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT,                 // 默认时钟源
        .spi_bus = RGB_SPI_HOST,                        // SPI 设备节点 (如 SPI2)
        .flags = {
            .with_dma = true,                           // 开启 DMA 特性 (非常重要)
        },
    };

    // 基于 SPI 实例化出 LED 条带控制句柄
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &s_strip));
    
    // 初始化完成后，全屏刷黑一次以确保状态可控
    ESP_ERROR_CHECK(led_strip_clear(s_strip));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
}

/**
 * @brief 释放 WS2812 所占用的底层资源
 */
void rgb_deinit(void) {
    if (!s_strip) {
        return;
    }

    led_strip_del(s_strip);
    s_strip = NULL;
}

/**
 * @brief 设定特定像素灯珠的 RGB 颜色 (安全过滤版)
 * @param index 灯珠索引号 (0 ~ RGB_LED_COUNT-1)
 * @param r 红色分量
 * @param g 绿色分量
 * @param b 蓝色分量
 */
void rgb_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
    // 越界检查
    if (!s_strip || index >= RGB_LED_COUNT) {
        return;
    }

    // 通过限流过滤机制调色
    apply_global_and_panel_limit(&r, &g, &b);

    // 将改变写入到内部的 RAM Buffer 显存中，(此步并不会立即将信号发往灯珠)
    led_strip_set_pixel(s_strip, index, r, g, b);
}

/**
 * @brief 快速设定特定像素 RGB (内部同 rgb_set)
 */
void rgb_set_fast(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip || index >= RGB_LED_COUNT) {
        return;
    }
    apply_global_and_panel_limit(&r, &g, &b);
    led_strip_set_pixel(s_strip, index, r, g, b);
}

/**
 * @brief 清空显存的所有数据 (将其全部置黑)
 */
void rgb_clear(void) {
    if (!s_strip) {
        return;
    }
    led_strip_clear(s_strip);
}

/**
 * @brief 设定特定像素灯珠的 HSV 颜色 (色相，饱和度，明度)
 * @details 相比 RGB，通过旋转 HSV 的 Hue (0-360) 能很方便地做出彩虹流光等动画
 * @param index 灯珠索引号
 * @param hue 色相 (0 - 360)
 * @param light 明度/亮度
 */
void rgb_set_hsv(uint32_t index, uint16_t hue, uint16_t light) {
    if (!s_strip || index >= RGB_LED_COUNT) {
        return;
    }

    // 将色相限定在 360 度圆盘内
    hue %= 360;
    
    // 安全硬件限流截断处理
    light = (light > PANEL_LED_VALUE_MAX) ? PANEL_LED_VALUE_MAX : light;
    
    // 调用官方组件提供的 HSV 设置接口 (此处强锁 Saturation 饱和度为 255)
    led_strip_set_pixel_hsv(s_strip, index, hue, 255, light);
}

/**
 * @brief 设定整个 LED 面板的全局色彩加权系数 (常用于制作渐明渐暗，或做统一遮色罩)
 */
void rgb_set_global_color8(uint8_t r8, uint8_t g8, uint8_t b8) {
    portENTER_CRITICAL(&s_color_mux);
    s_global_r8 = r8;
    s_global_g8 = g8;
    s_global_b8 = b8;
    portEXIT_CRITICAL(&s_color_mux);
}

/**
 * @brief 获取现有的 LED 面板全局色彩加权系数
 */
void rgb_get_global_color8(uint8_t* r8, uint8_t* g8, uint8_t* b8) {
    if (!r8 || !g8 || !b8) {
        return;
    }
    portENTER_CRITICAL(&s_color_mux);
    *r8 = s_global_r8;
    *g8 = s_global_g8;
    *b8 = s_global_b8;
    portEXIT_CRITICAL(&s_color_mux);
}

/**
 * @brief 发送帧显存到物理 LED，触发实际显示 (Render / Swap Buffer)
 * @details WS2812 需要一帧数据一次性发完，所有的画图修改必须积累在这个指令之后一同生效。
 */
void rgb_show(void) {
    if (!s_strip) {
        return;
    }

    // 将本地的 RAM 灯带数组数据，通过 SPI DMA 真实推送到数据线引发灯带变化
    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        // 如果遇到系统忙资源冲突重试一次
        led_strip_refresh(s_strip);
    }
    
    // WS2812 协议规定：Latch(锁存刷新)的信号是一段较长时间的低电平 (标准要求 > 50us)
    // 强制软件阻塞延迟 80 us，能提高大面积高帧率冲刷时的稳定性，避免丢电平脉冲
    esp_rom_delay_us(80);
}

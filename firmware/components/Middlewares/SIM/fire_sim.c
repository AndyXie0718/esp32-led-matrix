#include "fire_sim.h"

#include <math.h>
#include <stdint.h>

#include "doom.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gravity.h"
#include "key.h"
#include "panel_config.h"
#include "rgb.h"

/**
 * @file fire_sim.c
 * @brief 火焰粒子模拟核心逻辑
 * 
 * 1. 映射面板参数到本地宏：将全局面板配置映射为 local 宏简化使用。
 * 2. 仿真参数：定义 FPS 和 LED 最大亮度。
 * 3. 调色板定义：包含经典火焰（红橙）、蓝火、紫火三种调色板。
 * 4. 颜色 LUT (Lookup Table)：为了性能，预计算热量值到 RGB 颜色的映射表。
 * 5. 坐标映射：预计算仿真网格到 LED 物理索引的映射，减少每帧计算量。
 * 6. 仿真任务：
 *    - 初始化 RGB 与按键。
 *    - 处理重力偏移使火焰随倾斜摆动。
 *    - 检测按键点击切换调色板。
 *    - 执行 Doom-style 火焰算法并渲染。
 */

// 映射面板参数到本地宏
#define W PANEL_WIDTH
#define H PANEL_HEIGHT

// 仿真参数：控制刷新率与亮度阈值
#define SIM_FPS 30
#define LED_VAL_MAX_I PANEL_LED_VALUE_MAX

// 按键配置：用于物理按键（如 BOOT 键）切换当前火焰配色
#define KEY_GPIO_PIN GPIO_NUM_0
#define KEY_DEBOUNCE_MS 50

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb8_t;

static const char* TAG = "fire_sim";

// 调色板参数定义
#define PAL_SIZE 37        // 每个调色板包含 37 种基础颜色
#define PALETTE_COUNT 3   // 总计支持的调色板数量（经典、蓝、紫）

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static key_t s_key;
static uint8_t s_palette_idx = 0;          // 当前选中的调色板索引
static uint16_t s_fire_led_map[W * H];    // 从仿真网格节点到 LED 硬件索引的预映射表
static bool s_fire_led_map_ready = false;

/**
 * 经典火焰调色板（橙红渐变）
 * 源自经典的 Doom 火焰实现，从暗红到亮黄最后到白色的过渡
 */
static const uint8_t PAL_CLASSIC[PAL_SIZE][3] = {
    {0x07, 0x07, 0x07}, {0x1f, 0x07, 0x07}, {0x2f, 0x0f, 0x07}, {0x47, 0x0f, 0x07}, {0x57, 0x17, 0x07},
    {0x67, 0x1f, 0x07}, {0x77, 0x1f, 0x07}, {0x8f, 0x27, 0x07}, {0x9f, 0x2f, 0x07}, {0xaf, 0x3f, 0x07},
    {0xbf, 0x47, 0x07}, {0xc7, 0x47, 0x07}, {0xdf, 0x4f, 0x07}, {0xdf, 0x57, 0x07}, {0xdf, 0x57, 0x07},
    {0xd7, 0x5f, 0x07}, {0xd7, 0x67, 0x0f}, {0xcf, 0x6f, 0x0f}, {0xcf, 0x77, 0x0f}, {0xcf, 0x7f, 0x0f},
    {0xcf, 0x87, 0x17}, {0xc7, 0x87, 0x17}, {0xc7, 0x8f, 0x17}, {0xc7, 0x97, 0x1f}, {0xbf, 0x9f, 0x1f},
    {0xbf, 0x9f, 0x1f}, {0xbf, 0xa7, 0x27}, {0xbf, 0xa7, 0x27}, {0xbf, 0xaf, 0x2f}, {0xb7, 0xaf, 0x2f},
    {0xb7, 0xb7, 0x2f}, {0xb7, 0xb7, 0x37}, {0xcf, 0xcf, 0x6f}, {0xdf, 0xdf, 0x9f}, {0xef, 0xef, 0xc7},
    {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff},
};

/**
 * 蓝火调色板
 * 从深蓝到青色，最后到达白色的冰冷感火焰
 */
static const uint8_t PAL_BLUE[PAL_SIZE][3] = {
    {0x05, 0x05, 0x05}, {0x05, 0x05, 0x10}, {0x05, 0x05, 0x20}, {0x05, 0x05, 0x30}, {0x05, 0x05, 0x40},
    {0x09, 0x09, 0x50}, {0x09, 0x09, 0x60}, {0x09, 0x09, 0x70}, {0x09, 0x09, 0x80}, {0x09, 0x09, 0x90},
    {0x0f, 0x0f, 0xa0}, {0x0f, 0x0f, 0xb0}, {0x1f, 0x1f, 0xc0}, {0x2f, 0x2f, 0xd0}, {0x3f, 0x3f, 0xe0},
    {0x4f, 0x4f, 0xf0}, {0x5f, 0x5f, 0xff}, {0x6f, 0x6f, 0xff}, {0x7f, 0x7f, 0xff}, {0x8f, 0x8f, 0xff},
    {0x9f, 0x9f, 0xff}, {0xaf, 0xaf, 0xff}, {0xbf, 0xbf, 0xff}, {0xcf, 0xcf, 0xff}, {0xdf, 0xdf, 0xff},
    {0xef, 0xef, 0xff}, {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff}, {0xe0, 0xff, 0xff}, {0xd0, 0xff, 0xff},
    {0xc0, 0xff, 0xff}, {0xb0, 0xff, 0xff}, {0xa0, 0xff, 0xff}, {0x90, 0xff, 0xff}, {0x80, 0xff, 0xff},
    {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff},
};

/**
 * 紫色火焰调色板
 * 从暗紫色到明亮的洋红过渡
 */
static const uint8_t PAL_PURPLE[PAL_SIZE][3] = {
    {0x05, 0x00, 0x05}, {0x10, 0x00, 0x10}, {0x20, 0x00, 0x20}, {0x30, 0x00, 0x30}, {0x40, 0x00, 0x40},
    {0x50, 0x00, 0x50}, {0x60, 0x05, 0x60}, {0x70, 0x05, 0x70}, {0x80, 0x05, 0x80}, {0x90, 0x0a, 0x90},
    {0xa0, 0x0a, 0xa0}, {0xb0, 0x0a, 0xb0}, {0xc0, 0x10, 0xc0}, {0xd0, 0x15, 0xd0}, {0xe0, 0x20, 0xe0},
    {0xf0, 0x30, 0xf0}, {0xff, 0x40, 0xff}, {0xff, 0x50, 0xff}, {0xff, 0x60, 0xff}, {0xff, 0x70, 0xff},
    {0xff, 0x80, 0xff}, {0xff, 0x90, 0xff}, {0xff, 0xa0, 0xff}, {0xff, 0xb0, 0xff}, {0xff, 0xc0, 0xff},
    {0xff, 0xd0, 0xff}, {0xff, 0xe0, 0xff}, {0xff, 0xf0, 0xff}, {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff},
    {0xfd, 0xee, 0xfd}, {0xfb, 0xdd, 0xfb}, {0xf9, 0xcc, 0xf9}, {0xf7, 0xbb, 0xf7}, {0xf5, 0xaa, 0xf5},
    {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff},
};

static rgb8_t s_lut[PALETTE_COUNT][DOOM_HEAT_LEVELS];
static uint8_t s_lut_ready[PALETTE_COUNT];

/**
 * 根据索引获取对应的调色板数组指针
 */
static inline const uint8_t (*get_palette(uint8_t idx))[3] {
    if (idx == 1) {
        return PAL_BLUE;
    }
    if (idx == 2) {
        return PAL_PURPLE;
    }
    return PAL_CLASSIC;
}

/**
 * 将 0-255 的 RGB 值缩放到面板允许的最大亮度范围内
 */
static inline void scale_rgb_to_ledmax(uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = (uint8_t)(((uint16_t)(*r) * LED_VAL_MAX_I) / 255u);
    *g = (uint8_t)(((uint16_t)(*g) * LED_VAL_MAX_I) / 255u);
    *b = (uint8_t)(((uint16_t)(*b) * LED_VAL_MAX_I) / 255u);
}

/**
 * 构建 颜色查找表 (LUT)
 * 此函数将 37 级的调色板映射到 DOOM_HEAT_LEVELS (通常为 256 或更多) 级别，
 * 以便在渲染循环中通过热量值快速查找颜色。
 */
static void build_lut(uint8_t pal_idx) {
    const uint8_t (*pal)[3] = get_palette(pal_idx);

    for (int hv = 0; hv < DOOM_HEAT_LEVELS; hv++) {
        // 将热量等级线性映射到调色板索引 (0~36)
        int ci = (hv * 30) / 40; 
        if (ci > (PAL_SIZE - 1)) {
            ci = PAL_SIZE - 1;
        }

        uint8_t r = pal[ci][0];
        uint8_t g = pal[ci][1];
        uint8_t b = pal[ci][2];
        scale_rgb_to_ledmax(&r, &g, &b);

        s_lut[pal_idx][hv] = (rgb8_t){r, g, b};
    }
    s_lut_ready[pal_idx] = 1;
}

/**
 * 预计算仿真网格坐标到 LED 驱动索引的映射表。
 * 仿真网格通常是 (0,0) 在左上角，而 LED 面板布局可能不同（如蛇形布线、(0,0)在底部等）。
 * 预计算后，渲染循环只需查表即可，显著提升 CPU 效率。
 */
static void build_fire_led_map_once(void) {
    if (s_fire_led_map_ready) {
        return;
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            // 垂直镜像映射：仿真 y=0 对应显示屏底部
            s_fire_led_map[y * W + x] = (uint16_t)panel_led_index(x, H - 1 - y);
        }
    }
    s_fire_led_map_ready = true;
}

/**
 * 火焰仿真主任务逻辑
 */
static void sim_task(void* arg) {
    (void)arg;

    bool key_inited = false;
    doom_fire_t fire;

    rgb_init();
    // 初始化物理按键
    if (key_init(&s_key, KEY_GPIO_PIN, true, KEY_DEBOUNCE_MS) != ESP_OK) {
        ESP_LOGE(TAG, "key_init failed");
        goto exit_task;
    }
    key_inited = true;

    // 预加载所有调色板的 LUT
    for (int i = 0; i < PALETTE_COUNT; i++) {
        build_lut((uint8_t)i);
    }
    build_fire_led_map_once();

    // 初始化火焰算法核心
    doom_fire_init(&fire, esp_random());
    doom_fire_reset(&fire);

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / SIM_FPS);

    while (s_running) {
        vTaskDelayUntil(&last_wake, frame_ticks);

        // 获取加速度计/陀螺仪数据（如有），用于火焰倾斜效果
        gravity_xy_t g = gravity_get();
        float gx = 0.0f;
        if (g.valid) {
            gx = g.gx * 2.5f; // 缩放系数影响偏转灵敏度
            if (gx < -2.5f) {
                gx = -2.5f;
            }
            if (gx > 2.5f) {
                gx = 2.5f;
            }
        }

        // 执行一步火焰模拟
        uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        doom_fire_step(&fire, gx, t_ms);

        // 检测按键切换配色
        if (key_get_press(&s_key)) {
            s_palette_idx = (uint8_t)((s_palette_idx + 1) % PALETTE_COUNT);
            if (!s_lut_ready[s_palette_idx]) {
                build_lut(s_palette_idx);
            }
            ESP_LOGD(TAG, "palette -> %u", (unsigned)s_palette_idx);
        }

        // 将热量数据渲染到 LED 缓冲区
        const float* heat = doom_fire_heat(&fire);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float v = heat[y * W + x];
                if (v < 0.0f) {
                    v = 0.0f;
                }
                if (v > (float)DOOM_HEAT_MAX) {
                    v = (float)DOOM_HEAT_MAX;
                }

                int hv = (int)(v + 0.5f);
                if (hv < 0) {
                    hv = 0;
                }
                if (hv >= DOOM_HEAT_LEVELS) {
                    hv = DOOM_HEAT_LEVELS - 1;
                }

                // 通过 LUT 查找颜色并设置 LED
                rgb8_t c = s_lut[s_palette_idx][hv];
                uint16_t idx = s_fire_led_map[y * W + x];
                rgb_set_fast((uint32_t)idx, c.r, c.g, c.b);
            }
        }
        rgb_show(); // 刷新显示
    }

exit_task:
    // 清理资源
    if (key_inited) {
        key_deinit(&s_key);
    }
    rgb_clear();
    rgb_show();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/**
 * 启动垂直火焰仿真任务
 * @param core_id 绑定的核心 ID (-1 为无绑定)
 * @param stack_size 任务堆栈大小
 * @param priority 任务优先级
 */
esp_err_t fire_sim_start(int core_id, uint32_t stack_size, int priority) {
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;
    BaseType_t ok = pdFAIL;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    int target_core = core_id;
    if (target_core < 0 || target_core >= CONFIG_FREERTOS_NUMBER_OF_CORES) {
        target_core = tskNO_AFFINITY;
    }
    ok = xTaskCreatePinnedToCore(sim_task, "fire_sim", stack_size, NULL, priority, &s_task, target_core);
#else
    (void)core_id;
    ok = xTaskCreate(sim_task, "fire_sim", stack_size, NULL, priority, &s_task);
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "fire sim task create failed");
        s_running = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * 停止火焰仿真任务
 * @param timeout_ms 最大等待退出时长（毫秒）
 */
esp_err_t fire_sim_stop(uint32_t timeout_ms) {
    if (!s_task) {
        return ESP_OK;
    }

    s_running = false;

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (s_task) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            TaskHandle_t task = s_task;
            if (task) {
                vTaskDelete(task); // 超时强行销毁
            }
            s_task = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

/**
 * 查询仿真是否正在运行
 */
bool fire_sim_is_running(void) {
    return s_task != NULL && s_running;
}

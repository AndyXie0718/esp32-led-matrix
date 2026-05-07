#include "water_sim.h"

#include <math.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "flip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gravity.h"
#include "key.h"
#include "panel_config.h"
#include "rgb.h"

// 定义面板尺寸和LED总数
#define W PANEL_WIDTH
#define H PANEL_HEIGHT
#define LED_COUNT PANEL_LED_COUNT

// 模拟的每秒帧数
#define SIM_FPS 30

// LED亮度等级相关宏
#define LED_VAL_MAX_I PANEL_LED_VALUE_MAX
#define LED_VAL_MAX_F ((float)PANEL_LED_VALUE_MAX)
#define LED_LEVELS (LED_VAL_MAX_I + 1)

// 按键配置
#define KEY_GPIO_PIN GPIO_NUM_0
#define KEY_DEBOUNCE_MS 50

// 调色板数量及每个调色板的锚点颜色数
#define PAL_N 6
#define PALETTE_COUNT 3

static const char* TAG = "water_sim";

// 简单的RGB结构体，用于调色板计算
typedef struct {
    uint8_t r, g, b;
} rgb8_t;

static rgb8_t s_pal_lut[PALETTE_COUNT][LED_LEVELS]; // 调色板查找表，存储映射后的RGB值
static uint8_t s_lut_ready[PALETTE_COUNT];         // 查找表是否已初始化的标志
static uint8_t s_palette_idx = 0;                  // 当前使用的调色板索引
static key_t s_key;                                // 按键句柄

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;

// 预定义的调色板方案：蓝色（水）、粉色、绿色
static const rgb8_t PALETTES[PALETTE_COUNT][PAL_N] = {
    {
        {0, 0, 0},
        {102, 255, 255},
        {0, 255, 255},
        {0, 204, 255},
        {0, 153, 255},
        {0, 102, 255},
    },
    {
        {0, 0, 0},
        {255, 153, 255},
        {255, 102, 255},
        {255, 51, 204},
        {255, 51, 153},
        {255, 0, 102},
    },
    {
        {0, 0, 0},
        {20, 255, 30},
        {15, 255, 20},
        {10, 255, 10},
        {5, 255, 5},
        {0, 255, 0},
    },
};

/**
 * 线性插值计算两个RGB颜色之间的中间色
 * @param a 起始颜色
 * @param b 结束颜色
 * @param t 插值因子 [0.0, 1.0]
 */
static inline rgb8_t lerp_rgb(rgb8_t a, rgb8_t b, float t) {
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }

    rgb8_t o;
    o.r = (uint8_t)lrintf((float)a.r + ((float)b.r - (float)a.r) * t);
    o.g = (uint8_t)lrintf((float)a.g + ((float)b.g - (float)a.g) * t);
    o.b = (uint8_t)lrintf((float)a.b + ((float)b.b - (float)a.b) * t);
    return o;
}

/**
 * 构建特定调色板的查找表（LUT）
 * 将流动的“亮度/密度”值线性映射到调色板的RGB空间，并根据最大亮度进行归一化缩放
 * @param pal_idx 调色板索引
 */
static void build_palette_lut(uint8_t pal_idx) {
    const rgb8_t* pal = PALETTES[pal_idx];

    for (int lv = 0; lv < LED_LEVELS; lv++) {
        float t = (LED_VAL_MAX_F > 0.0f) ? ((float)lv / LED_VAL_MAX_F) : 0.0f;
        float p = t * (float)(PAL_N - 1);
        int i0 = (int)p;
        int i1 = (i0 + 1 < PAL_N) ? (i0 + 1) : i0;
        float ft = p - (float)i0;

        // 根据当前亮度等级，在调色板的片段之间进行插值
        rgb8_t c = lerp_rgb(pal[i0], pal[i1], ft);
        
        // 寻找RGB中的最大分量，作为归一化的基准
        uint8_t m = c.r;
        if (c.g > m) {
            m = c.g;
        }
        if (c.b > m) {
            m = c.b;
        }

        // 按比例缩放颜色分量，确保最终亮度与流体密度对应
        if (m > 0) {
            float k = (float)lv / (float)m;
            c.r = (uint8_t)lrintf((float)c.r * k);
            c.g = (uint8_t)lrintf((float)c.g * k);
            c.b = (uint8_t)lrintf((float)c.b * k);
        } else {
            c.r = 0;
            c.g = 0;
            c.b = 0;
        }

        s_pal_lut[pal_idx][lv] = c;
    }

    s_lut_ready[pal_idx] = 1;
}

// 从流体网格数组中根据 (x,y) 坐标获取值
static inline float grid_get(const float* grid, int x, int y) {
    return grid[x * H + y];
}

/**
 * 流体模拟主任务逻辑
 */
static void sim_task(void* arg) {
    (void)arg;

    bool key_inited = false;
    FlipFluid* f = NULL;
    float grid[LED_COUNT];

    // 初始化显示和按键
    rgb_init();
    if (key_init(&s_key, KEY_GPIO_PIN, true, KEY_DEBOUNCE_MS) != ESP_OK) {
        ESP_LOGE(TAG, "key_init failed");
        goto exit_task;
    }
    key_inited = true;

    // 预构建所有调色板的查找表
    for (int i = 0; i < PALETTE_COUNT; i++) {
        build_palette_lut((uint8_t)i);
    }

    // 创建 FLIP 流体模拟实例
    // 参数：重力缩放、模拟精度、密度映射因子、面板宽度
    f = flip_create(1.0f, 1.0f, W, 0.6f);
    if (!f) {
        ESP_LOGE(TAG, "flip_create failed");
        goto exit_task;
    }
    flip_set_gravity_scale(f, 9.81f);
    
    // 设置解算器迭代次数和粒子混合比例
    int dyn_push_iters = 1;
    int dyn_pressure_iters = 10;
    const float dyn_flip_ratio = 0.9f;
    flip_set_solver_quality(f, dyn_push_iters, dyn_pressure_iters,
                            dyn_flip_ratio);

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / SIM_FPS);
    const float dt = 1.0f / (float)SIM_FPS;
    
    // 性能监控相关的变量
    uint32_t perf_frames = 0;
    uint64_t perf_sum_us = 0;
    uint32_t perf_max_us = 0;
    uint64_t perf_last_log_us = esp_timer_get_time();

    while (s_running) {
        vTaskDelayUntil(&last_wake, frame_ticks);
        uint64_t frame_begin_us = esp_timer_get_time();

        // 获取重力向量，用于实时改变液体流动方向
        gravity_xy_t g = gravity_get();
        float gx = g.valid ? g.gx : 0.0f;
        float gy = g.valid ? g.gy : 0.0f;

        // 执行一步流体物理模拟
        flip_step(f, dt, gx, gy);
        // 获取当前流体在 LED 网格上的密度分布
        flip_get_led_grid(f, grid);

        // 如果按键按下，切换下一个调色板
        if (key_get_press(&s_key)) {
            s_palette_idx = (uint8_t)((s_palette_idx + 1) % PALETTE_COUNT);
            if (!s_lut_ready[s_palette_idx]) {
                build_palette_lut(s_palette_idx);
            }
            ESP_LOGD(TAG, "palette -> %u", (unsigned)s_palette_idx);
        }

        // 获取全局亮度/颜色配置，用于覆盖渲染颜色
        uint8_t global_r = 255;
        uint8_t global_g = 255;
        uint8_t global_b = 255;
        rgb_get_global_color8(&global_r, &global_g, &global_b);
        const bool use_global_color =
            !(global_r == 255 && global_g == 255 && global_b == 255);

        // 遍历面板像素并更新 LED
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float v = grid_get(grid, x, y);
                // 确保密度值在合法范围内
                if (!isfinite(v)) {
                    v = 0.0f;
                }
                if (v < 0.0f) {
                    v = 0.0f;
                }
                if (v > LED_VAL_MAX_F) {
                    v = LED_VAL_MAX_F;
                }

                int lv = (int)(v + 0.5f);
                if (lv < 0) {
                    lv = 0;
                }
                if (lv >= LED_LEVELS) {
                    lv = LED_LEVELS - 1;
                }

                rgb8_t c;
                if (use_global_color) {
                    // 如果设置了全局颜色，则通过缩放全局颜色来渲染
                    c.r = (uint8_t)(((uint16_t)global_r * (uint16_t)lv) /
                                    LED_VAL_MAX_I);
                    c.g = (uint8_t)(((uint16_t)global_g * (uint16_t)lv) /
                                    LED_VAL_MAX_I);
                    c.b = (uint8_t)(((uint16_t)global_b * (uint16_t)lv) /
                                    LED_VAL_MAX_I);
                } else {
                    // 默认情况下使用调色板查找表
                    c = s_pal_lut[s_palette_idx][lv];
                }
                
                // 将 (x,y) 坐标转换为 LED 阵列索引
                int idx = panel_led_index(x, y);
                rgb_set((uint32_t)idx, c.r, c.g, c.b);
            }
        }
        // 推送到硬件显示
        rgb_show();

        // 性能统计与动态质量调整
        uint32_t frame_us = (uint32_t)(esp_timer_get_time() - frame_begin_us);
        perf_frames++;
        perf_sum_us += frame_us;
        if (frame_us > perf_max_us) {
            perf_max_us = frame_us;
        }

        uint64_t now_us = esp_timer_get_time();
        if (now_us - perf_last_log_us >= 2000000ULL) {
            const uint32_t budget_us = (1000000 / SIM_FPS);
            uint32_t avg_us =
                (perf_frames > 0) ? (uint32_t)(perf_sum_us / perf_frames) : 0;

            // 根据任务负载自动调整压力解算器的迭代次数，以平衡效果和运行帧率
            int new_pressure = dyn_pressure_iters;
            if (avg_us > (budget_us * 95u) / 100u || perf_max_us > budget_us) {
                new_pressure -= 1; // 负载过高，降低质量
            } else if (avg_us < (budget_us * 65u) / 100u &&
                       perf_max_us < (budget_us * 85u) / 100u) {
                new_pressure += 1; // 负载余裕，提高质量
            }

            if (new_pressure < 6) {
                new_pressure = 6;
            }
            if (new_pressure > 16) {
                new_pressure = 16;
            }

            if (new_pressure != dyn_pressure_iters) {
                dyn_pressure_iters = new_pressure;
                flip_set_solver_quality(f, dyn_push_iters, dyn_pressure_iters,
                                        dyn_flip_ratio);
            }

            ESP_LOGD(TAG,
                     "perf avg=%uus max=%uus budget=%uus, pressure_iters=%d",
                     avg_us, perf_max_us, budget_us, dyn_pressure_iters);
            perf_frames = 0;
            perf_sum_us = 0;
            perf_max_us = 0;
            perf_last_log_us = now_us;
        }
    }

exit_task:
    // 清理资源
    if (f) {
        flip_destroy(f);
    }
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
 * 外部 API：启动水流模拟
 */
esp_err_t water_sim_start(int core_id, uint32_t stack_size, int priority) {
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
    // 创建任务并固定到指定核心（如果是多核系统）
    ok = xTaskCreatePinnedToCore(sim_task, "water_sim", stack_size, NULL, priority, &s_task, target_core);
#else
    (void)core_id;
    ok = xTaskCreate(sim_task, "water_sim", stack_size, NULL, priority, &s_task);
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "water sim task create failed");
        s_running = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * 停止模拟任务
 * @param timeout_ms 最大等待时间（毫秒）
 */
esp_err_t water_sim_stop(uint32_t timeout_ms) {
    if (!s_task) {
        return ESP_OK;
    }

    s_running = false;

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    // 等待任务自行结束并清理
    while (s_task) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            TaskHandle_t task = s_task;
            if (task) {
                vTaskDelete(task);
            }
            s_task = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

/**
 * 检查模拟是否正在运行
 */
bool water_sim_is_running(void) {
    return s_task != NULL && s_running;
}

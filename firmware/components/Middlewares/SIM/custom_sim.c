#include "custom_sim.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_config.h"
#include "rgb.h"

// 映射面板参数到本地宏
#define W PANEL_WIDTH
#define H PANEL_HEIGHT
#define LED_COUNT PANEL_LED_COUNT

// 默认仿真刷新率
#define CUSTOM_FPS 20

// 坐标系方向定义，用于适配不同的 LED 矩阵安装方向
#define GX_SIGN -1
#define GY_SIGN 1

static const char* TAG = "custom_sim";

// 静态变量：任务句柄、运行状态掩码、互斥锁及位图缓冲区
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static SemaphoreHandle_t s_lock = NULL;
static uint8_t s_bitmap[LED_COUNT]; // 存储 0 或 1，代表 LED 亮灭


/**
 * @brief 在锁定状态下绘制位图到 RGB 硬件缓冲区
 * 
 * 根据 GX_SIGN 和 GY_SIGN 的配置，将二维 bitmap 坐标映射到物理 LED 索引。
 */
static void draw_bitmap_locked(void) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx;
            // 处理镜像和翻转逻辑
            if(GX_SIGN == -1)
            {
                if(GY_SIGN == -1)
                    idx = panel_led_index(W-1-x, H-1-y);
                else
                    idx = panel_led_index(W-1-x, y);
            }
            else
            {
                if(GY_SIGN == -1)
                    idx = panel_led_index(x, H-1-y);
                else
                    idx = panel_led_index(x, y);
            }
            
            // 如果位图对应点为非零，则设置为全局颜色（白色），否则熄灭
            if (s_bitmap[y * W + x]) {
                rgb_set_fast((uint32_t)idx, 255, 255, 255);
            } else {
                rgb_set_fast((uint32_t)idx, 0, 0, 0);
            }
        }
    }
    // 推送数据到 LED 硬件（SPI/RMT）
    rgb_show();
}

/**
 * @brief 仿真循环任务
 */
static void sim_task(void* arg) {
    (void)arg;

    // 初始化硬件
    rgb_init();
    rgb_clear();
    rgb_show();

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / CUSTOM_FPS);

    while (s_running) {
        // 维持稳定的帧率
        vTaskDelayUntil(&last_wake, frame_ticks);

        // 获取锁以安全读取 s_bitmap 并更新显示
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            draw_bitmap_locked();
            xSemaphoreGive(s_lock);
        }
    }

    // 退出前清空显示
    rgb_clear();
    rgb_show();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 启动自定义仿真任务
 * 
 * @param core_id 指定运行核心 (0/1 或 tskNO_AFFINITY)
 * @param stack_size 任务栈大小
 * @param priority 任务优先级
 * @return esp_err_t 
 */
esp_err_t custom_sim_start(int core_id, uint32_t stack_size, int priority) {
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    // 惰性初始化互斥锁
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_running = true;
    BaseType_t ok = pdFAIL;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    int target_core = core_id;
    if (target_core < 0 || target_core >= CONFIG_FREERTOS_NUMBER_OF_CORES) {
        target_core = tskNO_AFFINITY;
    }
    ok = xTaskCreatePinnedToCore(sim_task, "custom_sim", stack_size, NULL, priority, &s_task, target_core);
#else
    (void)core_id;
    ok = xTaskCreate(sim_task, "custom_sim", stack_size, NULL, priority, &s_task);
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "custom sim task create failed");
        s_running = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 停止自定义仿真任务
 * 
 * @param timeout_ms 等待任务退出的超时时间
 */
esp_err_t custom_sim_stop(uint32_t timeout_ms) {
    if (!s_task) {
        return ESP_OK;
    }

    s_running = false; // 信号通知任务退出循环

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    // 等待任务结束
    while (s_task) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            // 如果超时，强制删除任务
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
 * @brief 检查仿真是否正在运行
 */
bool custom_sim_is_running(void) {
    return s_task != NULL && s_running;
}

/**
 * @brief 设置当前显示的位图数据
 * 
 * @param bitmap 输入数据，非零值代表点亮
 * @param len 数据长度，必须等于面板 LED 总数
 */
esp_err_t custom_sim_set_bitmap(const uint8_t* bitmap, size_t len) {
    if (!bitmap || len != LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    // 加锁拷贝数据，防止任务在渲染过程中数据被修改
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < LED_COUNT; i++) {
        s_bitmap[i] = bitmap[i] ? 1 : 0;
    }

    // 如果任务正在运行，立即重绘一帧以提高响应速度
    if (s_running) {
        draw_bitmap_locked();
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief 设置点亮像素的颜色
 */
esp_err_t custom_sim_set_color8(uint8_t r8, uint8_t g8, uint8_t b8) {
    rgb_set_global_color8(r8, g8, b8);

    if (s_running) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        draw_bitmap_locked();
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

/**
 * @brief 获取当前的全局显示颜色
 */
void custom_sim_get_color8(uint8_t* r8, uint8_t* g8, uint8_t* b8) {
    rgb_get_global_color8(r8, g8, b8);
}

#include "custom_sim.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_config.h"
#include "rgb.h"

#define W PANEL_WIDTH
#define H PANEL_HEIGHT
#define LED_COUNT PANEL_LED_COUNT

#define CUSTOM_FPS 20

#define GX_SIGN -1
#define GY_SIGN 1

static const char* TAG = "custom_sim";

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static SemaphoreHandle_t s_lock = NULL;
static uint8_t s_bitmap[LED_COUNT];


static void draw_bitmap_locked(void) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx;
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
            // int idx = panel_led_index(W-1-x, y);
            if (s_bitmap[y * W + x]) {
                rgb_set_fast((uint32_t)idx, 255, 255, 255);
            } else {
                rgb_set_fast((uint32_t)idx, 0, 0, 0);
            }
        }
    }
    rgb_show();
}

static void sim_task(void* arg) {
    (void)arg;

    rgb_init();
    rgb_clear();
    rgb_show();

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / CUSTOM_FPS);

    while (s_running) {
        vTaskDelayUntil(&last_wake, frame_ticks);

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            draw_bitmap_locked();
            xSemaphoreGive(s_lock);
        }
    }

    rgb_clear();
    rgb_show();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t custom_sim_start(int core_id, uint32_t stack_size, int priority) {
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

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

esp_err_t custom_sim_stop(uint32_t timeout_ms) {
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
                vTaskDelete(task);
            }
            s_task = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool custom_sim_is_running(void) {
    return s_task != NULL && s_running;
}

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

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < LED_COUNT; i++) {
        s_bitmap[i] = bitmap[i] ? 1 : 0;
    }

    if (s_running) {
        draw_bitmap_locked();
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

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

void custom_sim_get_color8(uint8_t* r8, uint8_t* g8, uint8_t* b8) {
    rgb_get_global_color8(r8, g8, b8);
}

/**
 * @file sim_manager.c
 * @brief 仿真管理器：负责统一控制 LED 点阵的各种仿真模式（如水流、火焰、自定义）的生命周期。
 */

#include "sim_manager.h"

#include <stdbool.h>

#include "custom_sim.h"
#include "esp_log.h"
#include "fire_sim.h"
#include "water_sim.h"

static const char* TAG = "sim_manager";

/**
 * @brief 静态运行时配置，定义仿真任务的系统参数
 */
static sim_runtime_config_t s_cfg = {
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    .core_id = 1,           // 双核芯片默认运行在 App Core (Core 1)
#else
    .core_id = 0,           // 单核芯片运行在 Core 0
#endif
    .stack_size = 8192,     // 仿真任务堆栈大小（字节）
    .priority = 5,          // 仿真任务优先级
    .stop_timeout_ms = 1000, // 停止仿真时的超时等待时间（毫秒）
};

static sim_mode_t s_current = SIM_MODE_NONE; // 当前运行的模式
static bool s_inited = false;                // 管理器是否已初始化

/**
 * @brief 启动特定仿真模式的辅助函数
 * @param mode 目标模式
 * @return esp_err_t 成功返回 ESP_OK
 */
static esp_err_t start_mode(sim_mode_t mode) {
    if (mode == SIM_MODE_WATER) {
        return water_sim_start(s_cfg.core_id, s_cfg.stack_size, s_cfg.priority);
    }
    if (mode == SIM_MODE_FIRE) {
        return fire_sim_start(s_cfg.core_id, s_cfg.stack_size, s_cfg.priority);
    }
    if (mode == SIM_MODE_CUSTOM) {
        return custom_sim_start(s_cfg.core_id, s_cfg.stack_size, s_cfg.priority);
    }
    return ESP_ERR_INVALID_ARG;
}

/**
 * @brief 停止特定仿真模式的辅助函数
 * @param mode 运行中的模式
 * @return esp_err_t 成功返回 ESP_OK
 */
static esp_err_t stop_mode(sim_mode_t mode) {
    if (mode == SIM_MODE_WATER) {
        return water_sim_stop(s_cfg.stop_timeout_ms);
    }
    if (mode == SIM_MODE_FIRE) {
        return fire_sim_stop(s_cfg.stop_timeout_ms);
    }
    if (mode == SIM_MODE_CUSTOM) {
        return custom_sim_stop(s_cfg.stop_timeout_ms);
    }
    return ESP_OK;
}

/**
 * @brief 初始化仿真管理器状态
 * @param cfg 初始配置，若为 NULL 则使用默认静态配置
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t sim_manager_init(const sim_runtime_config_t* cfg) {
    if (cfg) {
        s_cfg = *cfg;
    }
    s_current = SIM_MODE_NONE;
    s_inited = true;
    return ESP_OK;
}

/**
 * @brief 启动指定的仿真模式
 * @param mode 要启动的模式
 * @return esp_err_t 若管理器未初始化或已有模式正在运行，返回 ESP_ERR_INVALID_STATE
 */
esp_err_t sim_manager_start(sim_mode_t mode) {
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_current != SIM_MODE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = start_mode(mode);
    if (err == ESP_OK) {
        s_current = mode;
    } else {
        ESP_LOGE(TAG, "start mode %d failed: %s", (int)mode, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 切换仿真模式（停止当前模式并启动新模式）
 * @param mode 目标模式
 * @return esp_err_t 成功返回 ESP_OK，失败或超时则记录错误并返回
 */
esp_err_t sim_manager_switch(sim_mode_t mode) {
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == s_current) {
        return ESP_OK;
    }

    // 先停止当前运行的模式
    esp_err_t err = stop_mode(s_current);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "stop mode %d failed: %s", (int)s_current, esp_err_to_name(err));
        return err;
    }
    s_current = SIM_MODE_NONE;

    // 启动新模式
    err = start_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start mode %d failed: %s", (int)mode, esp_err_to_name(err));
        return err;
    }

    s_current = mode;
    return ESP_OK;
}

/**
 * @brief 停止当前正在运行的仿真
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t sim_manager_stop(void) {
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = stop_mode(s_current);
    s_current = SIM_MODE_NONE;
    return err;
}

sim_mode_t sim_manager_current(void) {
    return s_current;
}

#include "esp_err.h"
#include "esp_log.h"

// 引入 GPIO 和 FreeRTOS 相关库
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 引入业务组件头文件
#include "gravity.h"      // 重力传感器模块
#include "sim_manager.h"  // 物理模拟状态机管理器
#include "web_control.h"  // 网页控制与网络模块

static const char* TAG = "app_main";

// 系统的默认开机模式：可设定为火焰或流水模式
// static const sim_mode_t g_boot_mode = SIM_MODE_FIRE;     // 火焰模式
static const sim_mode_t g_boot_mode = SIM_MODE_WATER;       // 流水模式

// 定义模式切换按键的相关参数
#define MODE_KEY_PIN GPIO_NUM_0                 // 按键连接的 GPIO 引脚（通常是 ESP32 开发板上的 BOOT 键）
#define MODE_KEY_ACTIVE_LOW 1                   // 低电平有效（按下时接地）
#define MODE_KEY_POLL_MS 20                     // 按键轮询的周期（毫秒），兼做软件消抖
#define MODE_SWITCH_LONG_PRESS_MS 1000          // 触发模式切换所需的长按时间（毫秒）

/**
 * @brief 按键监听与模式切换的 RTOS 任务
 * @param arg 传递给任务的参数（当前置空）
 */
static void mode_switch_task(void* arg) {
    (void)arg;

    bool pressed = false;    // 记录按键当前是否处于按下状态
    bool switched = false;   // 记录本次按下是否已经触发过切换（防止长按时连续触发）
    TickType_t press_tick = 0; // 记录按键按下时刻的系统 Tick

    while (1) {
        // 读取按键引脚的电平
        int level = gpio_get_level(MODE_KEY_PIN);
        // 根据 active 掩码判断是否按下
        bool down = MODE_KEY_ACTIVE_LOW ? (level == 0) : (level != 0);
        TickType_t now = xTaskGetTickCount(); // 获取当前系统时间 (Ticks)

        if (down) {
            // 如果刚刚按下
            if (!pressed) {
                pressed = true;
                switched = false;
                press_tick = now; // 记录按下的初始时间
            } 
            // 如果处于按下状态，且未发生过切换，判断是否达到长按的时间阈值
            else if (!switched && (now - press_tick) >= pdMS_TO_TICKS(MODE_SWITCH_LONG_PRESS_MS)) {
                // 获取当前模式，计算下一个模式
                sim_mode_t cur = sim_manager_current();
                sim_mode_t next = SIM_MODE_WATER;
                if (cur == SIM_MODE_WATER) {
                    next = SIM_MODE_FIRE;
                } else if (cur == SIM_MODE_FIRE) {
                    next = SIM_MODE_CUSTOM;
                }
                
                // 执行模式切换
                esp_err_t err = sim_manager_switch(next);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "mode switched: %d -> %d", (int)cur, (int)next);
                } else {
                    ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(err));
                }
                switched = true; // 锁定状态，必须松开按键后才能在此触发
            }
        } else {
            // 按键松开时，重置状态
            pressed = false;
            switched = false;
        }

        // 挂起任务，释放 CPU 资源。起到消抖作用。
        vTaskDelay(pdMS_TO_TICKS(MODE_KEY_POLL_MS));
    }
}

/**
 * @brief 系统主函数入口
 */
void app_main(void) {
    // 1. 初始化重力传感器（如 MPU6050）
    gravity_init();

    // 2. 启动重力传感器模块的数据采集
    esp_err_t err = gravity_sensor_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gravity_sensor_start failed: %s", esp_err_to_name(err));
        return;
    }

    // 3. 配置模拟运行时的 RTOS 参数
    sim_runtime_config_t cfg = {
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
        .core_id = 1,                  // 若为双核处理器，物理计算放在 Core 1（Core 0 处理网络）
#else
        .core_id = 0,                  // 单核处理器则全在 Core 0
#endif
        .stack_size = 8192,            // 为物理引擎分配较大的 8KB 堆栈
        .priority = 5,                 // 设置较高优先级，保证渲染帧率
        .stop_timeout_ms = 1000,       // 模式切换时，旧任务退出的超时时间
    };

    // 4. 初始化并在默认模式下启动模拟管理器
    err = sim_manager_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sim_manager_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = sim_manager_start(g_boot_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sim_manager_start failed: %s", esp_err_to_name(err));
        return;
    }

    // 5. 启动 Web 控制服务器与 Wi-Fi 连接任务
    err = web_control_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web_control_start failed: %s", esp_err_to_name(err));
    }

    // 6. 创建模式按键监听任务
    BaseType_t ok = pdFAIL;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    // 双核环境下，将按键监听固定到 Core 0
    ok = xTaskCreatePinnedToCore(mode_switch_task, "mode_switch", 3072, NULL, 4, NULL, 0);
#else
    // 单核环境下的常规任务创建
    ok = xTaskCreate(mode_switch_task, "mode_switch", 3072, NULL, 4, NULL);
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "mode switch task create failed");
    }
}

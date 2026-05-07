#include "key.h"

#include "driver/gpio.h"
#include "esp_check.h"

/**
 * @brief 记录全局 GPIO 中断服务是否已被安装的标志位
 * @details ESP32 的所有 GPIO 共享同一个中断服务例程(ISR)分配器。
 *          只能初始化一次，后续不同的引脚只需要 add handler 即可。
 */
static bool s_isr_service_installed = false;

/**
 * @brief 硬件按键中断服务例程 (ISR)
 * @note IRAM_ATTR 宏非常关键，它会把这个函数强制放在单片机的内部 RAM (高速内存) 中，
 *       而不是常规的 Flash (慢速闪存) 中。响应中断要求极速，且如果此时 Flash Cache 
 *       正在做其他耗时操作 (如写 NVS) ，跑在 Flash 里的中断会直接导致系统崩溃重启 (Core Panic)。
 * @param arg 传递给中断的回调参数，这里是我们定义的 key_t 结构体指针
 */
static void IRAM_ATTR key_isr(void* arg) {
    key_t* key = (key_t*)arg;
    // 从中断专用 API 获取当前系统的 RTOS Tick 滴答数
    TickType_t now = xTaskGetTickCountFromISR();

    // 极其轻量级的软件防抖：如果距离上一次触发的时间跨度小于设定的消抖阈值，则视为杂波，无视它
    if ((now - key->last_tick) < key->debounce_ticks) {
        return;
    }

    key->last_tick = now;
    // 命中！标记本按键已按下。
    // (在中断上下文中，绝对不要去执行诸如 printf 等耗时任务，只改变状态标志是最安全的)
    key->pressed = 1;
}

/**
 * @brief 初始化按键的 GPIO 驱动及外部硬件中断
 * @param key 按键对象的结构体句柄
 * @param pin 目标 GPIO 引脚号 (如 GPIO_NUM_9)
 * @param active_low 是否低电平有效 (大部分物理按钮按下会使得引脚接地，所以是 true)
 * @param debounce_ms 软件消抖时间（毫秒级），一般机械按键弹片抖动需设为 20-50ms 左右
 */
esp_err_t key_init(key_t* key, gpio_num_t pin, bool active_low, uint32_t debounce_ms) {
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, "key", "key is NULL");

    key->pin = pin;
    key->active_low = active_low;
    key->pressed = 0;
    key->last_tick = 0;
    // 将自然毫秒数转换为 FreeRTOS 识别的系统滴答时钟数(Ticks)
    key->debounce_ticks = pdMS_TO_TICKS(debounce_ms);

    // 配置底层的引脚电气属性
    gpio_config_t io = {0};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_INPUT; // 按键本质就是电平输入
    // 根据有效电平自适应内部上下拉电阻。如果接地的按键(active_low=true)，则默认需通过内部电阻拉回到 3.3V
    io.pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io.pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    // 下降沿(平时高，按下变低) 还是 上升沿(平时低，按下变高) 触发中断
    io.intr_type = active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&io), "key", "gpio_config failed");

    // 单例模式：全局挂载一次 GPIO 中断服务体系
    if (!s_isr_service_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        // 如果是 ESP_ERR_INVALID_STATE 说明系统其他地方（比如其他驱动）已经帮忙注册过了，不当做失败
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_isr_service_installed = true;
    }

    // 针对这个具体的引脚上膛“扳机”，装载我们编写的 key_isr 函数
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(pin, key_isr, key), "key", "gpio_isr_handler_add failed");
    return ESP_OK;
}

/**
 * @brief 卸载按键驱动，回收中断资源
 */
esp_err_t key_deinit(key_t* key) {
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, "key", "key is NULL");

    esp_err_t err = gpio_isr_handler_remove(key->pin);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    key->pressed = 0;
    return ESP_OK;
}

/**
 * @brief 非阻塞地读取按键此刻触发状态 (事件消化模式)
 * @details 这是应用层跟底层硬件中断“握手”通信的桥梁。主程序调用此函数：
 *          1. 如果发现 key->pressed 为 1，则将其“消费”(迅速清零) 并向主程序返回 true。
 *          2. 如果未按下返回 false，绝不阻塞卡死主程序的其余流程运转。
 * @return true 表示自上次轮询以来发生了有效的物理按下动作
 */
bool key_get_press(key_t* key) {
    if (!key) {
        return false;
    }
    if (key->pressed) {
        key->pressed = 0;
        return true;
    }
    return false;
}

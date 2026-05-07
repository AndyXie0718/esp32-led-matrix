#include "uart.h"

/**
 * @brief 初始化串口 (UART) 硬件外设
 * @details 该函数负责配置串口的波特率、数据位、停止位和硬件流控等底层参数，
 *          并将配置应用到特定的 GPIO 引脚上，最后在操作系统中注册并安装串口驱动。
 * @param baudrate 目标波特率（如 115200、921600 等）
 */
void usart_init(uint32_t baudrate){
    // 初始化并清零配置结构体
    uart_config_t uart_config = {0};

    // --- 1. 配置串口底层硬件参数 ---
    uart_config.baud_rate = baudrate;                      // 动态配置通信波特率
    uart_config.data_bits = UART_DATA_8_BITS;              // 8位数据位（主流标准配置）
    uart_config.parity = UART_PARITY_DISABLE;              // 无奇偶校验位
    uart_config.stop_bits = UART_STOP_BITS_1;              // 1位停止位
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;      // 关闭 RTS/CTS 硬件流控机制
    uart_config.rx_flow_ctrl_thresh = 122;                 // 硬件流控阈值（虽然禁用了硬件流控，但依旧赋了安全经验值）
    uart_config.source_clk = UART_SCLK_APB;                // 指定时钟源为 APB 时钟 (一般为 80MHz)，保证分频出波特率的高精度

    // 将这些配置参数装载到指定的硬件串口外设号栈 (USART_UX，如 UART_NUM_1)
    uart_param_config(USART_UX, &uart_config);

    // --- 2. 配置引脚映射 ---
    // 将逻辑 UART 内部总线绑定到实际的物理 GPIO 外部引脚。
    // RTS 和 CTS 因为不用，所以传 UART_PIN_NO_CHANGE 表示不进行映射绑定。
    uart_set_pin(USART_UX, USART_TX_GPIO_PIN, USART_RX_GPIO_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // --- 3. 注册安装串口驱动程序，并分配底层缓存和 RTOS 通信机制 ---
    // 参数 1: USART_UX 选择目标串口号。
    // 参数 2: rx_buffer_size，设置接收环形缓冲区大小的 2 倍 (避免系统调度滞后导致数据覆盖丢失)。
    // 参数 3: tx_buffer_size，设置发送环形缓冲区大小的 2 倍。
    // 参数 4: event_queue_size，串口事件队列长度，这里设为 20。当串口发生事件（接收数据、溢出等）时可以用作通知。
    // 参数 5: uart_queue，这里传 NULL，代表调用方目前不需要此 Event 队列的句柄引用。
    // 参数 6: intr_alloc_flags，中断分配标志位 (通常传 0 代表使用标准默认的非 ISR 中断优先级)。
    uart_driver_install(USART_UX, RX_BUF_SIZE * 2, RX_BUF_SIZE * 2, 20, NULL, 0);
}


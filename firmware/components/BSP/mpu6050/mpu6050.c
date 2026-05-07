/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_system.h"
#include "driver/i2c.h"
#include "mpu6050.h"

/**
 * @brief 互补滤波参数 及 弧度转角度系数
 * @details MPU6050 结算姿态时，陀螺仪高频响应好但有温漂（积分误差），加速度计无温漂但容易受高频震动干扰。
 *          ALPHA 0.99 意味着：99% 的信任给陀螺仪积分结果，1% 用加速度计的绝对重力分掉来纠正。
 */
#define ALPHA                       0.99f        /*!< 陀螺仪权重系数 */
#define RAD_TO_DEG                  57.27272727f /*!< 弧度到角度的换算率 (180/PI) */

/* MPU6050 内部核心寄存器映射表 */
#define MPU6050_GYRO_CONFIG         0x1Bu   // 陀螺仪量程配置寄存器 (例如 ±250°/s)
#define MPU6050_ACCEL_CONFIG        0x1Cu   // 加速度计量程配置寄存器 (例如 ±2g)
#define MPU6050_INTR_PIN_CFG         0x37u  // 中断引脚配置寄存器
#define MPU6050_INTR_ENABLE          0x38u  // 中断使能寄存器
#define MPU6050_INTR_STATUS          0x3Au  // 中断状态标志位寄存器
#define MPU6050_ACCEL_XOUT_H        0x3Bu   // 三轴加速度计数据起始地址 (X轴高8位，接着是低8位/Y/Z)
#define MPU6050_GYRO_XOUT_H         0x43u   // 三轴陀螺仪数据起始地址 (X轴高8位)
#define MPU6050_TEMP_XOUT_H         0x41u   // 温度传感器数据起始地址
#define MPU6050_PWR_MGMT_1          0x6Bu   // 电源管理寄存器1 (用于唤醒/复位/选时钟源)
#define MPU6050_WHO_AM_I            0x75u   // 设备ID寄存器 (默认固定返回 0x68)

const uint8_t MPU6050_DATA_RDY_INT_BIT =      (uint8_t) BIT0;
const uint8_t MPU6050_I2C_MASTER_INT_BIT =    (uint8_t) BIT3;
const uint8_t MPU6050_FIFO_OVERFLOW_INT_BIT = (uint8_t) BIT4;
const uint8_t MPU6050_MOT_DETECT_INT_BIT =    (uint8_t) BIT6;
const uint8_t MPU6050_ALL_INTERRUPTS = (MPU6050_DATA_RDY_INT_BIT | MPU6050_I2C_MASTER_INT_BIT | MPU6050_FIFO_OVERFLOW_INT_BIT | MPU6050_MOT_DETECT_INT_BIT);

typedef struct {
    i2c_port_t bus;             // 绑定的 ESP32 I2C 硬件端口号 (例如 I2C_NUM_0)
    gpio_num_t int_pin;         // 可选的硬件中断引脚
    uint16_t dev_addr;          // MPU6050 从机 I2C 地址 (一般是 0x68 或 0x69)
    uint32_t counter;           // 采样计数器
    float dt;                   // 两次姿态采样之间的时间差积分变量 (dT)，必须小而准，否则陀螺仪积分角度会跑偏
    struct timeval *timer;      // 记录上次采样时间的定时器结构
} mpu6050_dev_t;

/**
 * @brief 底层 I2C 寄存器写操作
 * @param sensor 传感器上下文句柄
 * @param reg_start_addr 写入起始寄存器首地址
 * @param data_buf 要写入的数据缓存首指针
 * @param data_len 要写入的字节长度
 */
static esp_err_t mpu6050_write(mpu6050_handle_t sensor, const uint8_t reg_start_addr, const uint8_t *const data_buf, const uint8_t data_len)
{
    mpu6050_dev_t *sens = (mpu6050_dev_t *) sensor;
    esp_err_t  ret;

    // 创建一个 I2C 命令链路，标准的 I2C 写时序：START -> [ADDR|W] -> REGISTER_ADDR -> [DATA...] -> STOP
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ret = i2c_master_start(cmd);
    assert(ESP_OK == ret);
    // 指明从机地址，附带发写方向 (Write) 信令
    ret = i2c_master_write_byte(cmd, sens->dev_addr | I2C_MASTER_WRITE, true);
    assert(ESP_OK == ret);
    ret = i2c_master_write_byte(cmd, reg_start_addr, true);
    assert(ESP_OK == ret);
    ret = i2c_master_write(cmd, data_buf, data_len, true);
    assert(ESP_OK == ret);
    ret = i2c_master_stop(cmd);
    assert(ESP_OK == ret);
    // 阻塞式下发整个命令链路包给 I2C 总线去执行
    ret = i2c_master_cmd_begin(sens->bus, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    return ret;
}

/**
 * @brief 底层 I2C 寄存器读操作
 * @details I2C 读取时序需要一个 Dummy Write 来指定寄存器：START -> [ADDR|W] -> REGISTER_ADDR -> RESTART -> [ADDR|R] -> [READ_DATA...] -> NACK -> STOP
 */
static esp_err_t mpu6050_read(mpu6050_handle_t sensor, const uint8_t reg_start_addr, uint8_t *const data_buf, const uint8_t data_len)
{
    mpu6050_dev_t *sens = (mpu6050_dev_t *) sensor;
    esp_err_t  ret;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ret = i2c_master_start(cmd);
    assert(ESP_OK == ret);
    ret = i2c_master_write_byte(cmd, sens->dev_addr | I2C_MASTER_WRITE, true);
    assert(ESP_OK == ret);
    // 告诉 MPU6050：我要从这个寄存器地址开始读
    ret = i2c_master_write_byte(cmd, reg_start_addr, true);
    assert(ESP_OK == ret);
    // RESTART 重复起始位，开始调头接收数据
    ret = i2c_master_start(cmd);
    assert(ESP_OK == ret);
    ret = i2c_master_write_byte(cmd, sens->dev_addr | I2C_MASTER_READ, true);
    assert(ESP_OK == ret);
    ret = i2c_master_read(cmd, data_buf, data_len, I2C_MASTER_LAST_NACK);
    assert(ESP_OK == ret);
    ret = i2c_master_stop(cmd);
    assert(ESP_OK == ret);
    ret = i2c_master_cmd_begin(sens->bus, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    return ret;
}

/**
 * @brief MPU6050 驱动对象在内存堆上的创造与初始化
 * @param port I2C 节点端口 (确保这个 port 已经被 I2C config 初始化过了)
 * @param dev_addr MPU6050从机地址（未左移前的 7-bit地址，通常是 0x68）
 */
mpu6050_handle_t mpu6050_create(i2c_port_t port, const uint16_t dev_addr)
{
    // 利用 calloc 创建结构体会自动将内存全部置零初始化
    mpu6050_dev_t *sensor = (mpu6050_dev_t *) calloc(1, sizeof(mpu6050_dev_t));
    sensor->bus = port;
    sensor->dev_addr = dev_addr << 1; // ESP-IDF 的 I2C 读写需要 8-bit 地址（原始 7-bit 地址左移一位以腾出最低的读写 R/W 位空间）
    sensor->counter = 0;
    sensor->dt = 0;
    sensor->timer = (struct timeval *) calloc(1, sizeof(struct timeval));
    return (mpu6050_handle_t) sensor;
}

/**
 * @brief 释放传感器资源，释放占用的 RAM
 */
void mpu6050_delete(mpu6050_handle_t sensor)
{
    mpu6050_dev_t *sens = (mpu6050_dev_t *) sensor;
    free(sens->timer); // 防止发生底层内存泄漏，需先把内部的 timer 指针也 free 掉 (原生库遗漏的细节补充)
    free(sens);
}

/**
 * @brief 验证读取设备 WHO_AM_I ID
 * @note 出厂默认值为 0x68
 */
esp_err_t mpu6050_get_deviceid(mpu6050_handle_t sensor, uint8_t *const deviceid)
{
    return mpu6050_read(sensor, MPU6050_WHO_AM_I, deviceid, 1);
}

/**
 * @brief 唤醒 MPU6050 (解除睡眠模式)
 * @details MPU6050 在上电时默认是进入 Sleep 睡眠省电状态的，必须手动操作电源管理寄存器 (位 6 清 0) 来点亮传感器网络核心。
 */
esp_err_t mpu6050_wake_up(mpu6050_handle_t sensor)
{
    esp_err_t ret;
    uint8_t tmp;
    ret = mpu6050_read(sensor, MPU6050_PWR_MGMT_1, &tmp, 1);
    if (ESP_OK != ret) {
        return ret;
    }
    tmp &= (~BIT6); // BIT6置0：取消 SLEEP 状态
    ret = mpu6050_write(sensor, MPU6050_PWR_MGMT_1, &tmp, 1);
    return ret;
}

/**
 * @brief 让 MPU6050 进入睡眠低功耗模式
 */
esp_err_t mpu6050_sleep(mpu6050_handle_t sensor)
{
    esp_err_t ret;
    uint8_t tmp;
    ret = mpu6050_read(sensor, MPU6050_PWR_MGMT_1, &tmp, 1);
    if (ESP_OK != ret) {
        return ret;
    }
    tmp |= BIT6; // BIT6置1：开启 SLEEP 状态
    ret = mpu6050_write(sensor, MPU6050_PWR_MGMT_1, &tmp, 1);
    return ret;
}

/**
 * @brief 配置传感器的量程
 * @param acce_fs 加速度计量程（例如 ACCE_FS_2G 表示正负 2个 G 的重力加速度测量上限）
 * @param gyro_fs 陀螺仪量程（例如 GYRO_FS_250DPS 表示正负 250 度/秒 的旋转角速度测量上限）
 */
esp_err_t mpu6050_config(mpu6050_handle_t sensor, const mpu6050_acce_fs_t acce_fs, const mpu6050_gyro_fs_t gyro_fs)
{
    // 在对应的配置寄存器中，有效配置都是在其寄存器的位[4:3]中，所以要左移 3 位 (<< 3)
    uint8_t config_regs[2] = {gyro_fs << 3,  acce_fs << 3};
    // 连续用数组的方式将这两个寄存器一起写进入 (利用 I2C 地址自增特性连写)
    return mpu6050_write(sensor, MPU6050_GYRO_CONFIG, config_regs, sizeof(config_regs));
}

/**
 * @brief 动态获取当前配置里的加速度计 LSB 灵敏度换算乘数
 * @details MPU6050 返回的物理数据全是没有度量的 Raw ADC 原始数字，必须除以这里计算出来的 Sensitivity 系数，才会变成标量的 'g'
 */
esp_err_t mpu6050_get_acce_sensitivity(mpu6050_handle_t sensor, float *const acce_sensitivity)
{
    esp_err_t ret;
    uint8_t acce_fs;
    ret = mpu6050_read(sensor, MPU6050_ACCEL_CONFIG, &acce_fs, 1);
    acce_fs = (acce_fs >> 3) & 0x03;
    switch (acce_fs) {
    case ACCE_FS_2G:
        *acce_sensitivity = 16384;
        break;

    case ACCE_FS_4G:
        *acce_sensitivity = 8192;
        break;

    case ACCE_FS_8G:
        *acce_sensitivity = 4096;
        break;

    case ACCE_FS_16G:
        *acce_sensitivity = 2048;
        break;

    default:
        break;
    }
    return ret;
}

/**
 * @brief 动态获取当前配置里的陀螺仪 LSB 灵敏度换算乘数
 * @details 将数字信号除以这个 Sensitivity 后即可得到物理世界的角速度 度/秒 (dps)
 */
esp_err_t mpu6050_get_gyro_sensitivity(mpu6050_handle_t sensor, float *const gyro_sensitivity)
{
    esp_err_t ret;
    uint8_t gyro_fs;
    ret = mpu6050_read(sensor, MPU6050_GYRO_CONFIG, &gyro_fs, 1);
    gyro_fs = (gyro_fs >> 3) & 0x03;
    switch (gyro_fs) {
    case GYRO_FS_250DPS:
        *gyro_sensitivity = 131; // 满量程 ±250dps 对应内部 16-bit有符号数，131 LSB = 1度/秒
        break;

    case GYRO_FS_500DPS:
        *gyro_sensitivity = 65.5;
        break;

    case GYRO_FS_1000DPS:
        *gyro_sensitivity = 32.8;
        break;

    case GYRO_FS_2000DPS:
        *gyro_sensitivity = 16.4;
        break;

    default:
        break;
    }
    return ret;
}

/**
 * @brief 硬件中断的高级引脚行为配置
 * @details 设定 MPU 的 INT 引脚是高有效还是低有效，推挽输出还是开漏，以及触发完后怎么清除等硬件级细节
 */
esp_err_t mpu6050_config_interrupts(mpu6050_handle_t sensor, const mpu6050_int_config_t *const interrupt_configuration)
{
    esp_err_t ret = ESP_OK;

    if (NULL == interrupt_configuration) {
        ret = ESP_ERR_INVALID_ARG;
        return ret;
    }

    if (GPIO_IS_VALID_GPIO(interrupt_configuration->interrupt_pin)) {
        // Set GPIO connected to MPU6050 INT pin only when user configures interrupts.
        mpu6050_dev_t *sensor_device = (mpu6050_dev_t *) sensor;
        sensor_device->int_pin = interrupt_configuration->interrupt_pin;
    } else {
        ret = ESP_ERR_INVALID_ARG;
        return ret;
    }

    uint8_t int_pin_cfg = 0x00;

    ret = mpu6050_read(sensor, MPU6050_INTR_PIN_CFG, &int_pin_cfg, 1);

    if (ESP_OK != ret) {
        return ret;
    }

    if (INTERRUPT_PIN_ACTIVE_LOW == interrupt_configuration->active_level) {
        int_pin_cfg |= BIT7;
    }

    if (INTERRUPT_PIN_OPEN_DRAIN == interrupt_configuration->pin_mode) {
        int_pin_cfg |= BIT6;
    }

    if (INTERRUPT_LATCH_UNTIL_CLEARED == interrupt_configuration->interrupt_latch) {
        int_pin_cfg |= BIT5;
    }

    if (INTERRUPT_CLEAR_ON_ANY_READ == interrupt_configuration->interrupt_clear_behavior) {
        int_pin_cfg |= BIT4;
    }

    ret = mpu6050_write(sensor, MPU6050_INTR_PIN_CFG, &int_pin_cfg, 1);

    if (ESP_OK != ret) {
        return ret;
    }

    gpio_int_type_t gpio_intr_type;

    // 根据高/低电平有效选择 ESP32 对应的下降沿(NEGEDGE)或上升沿(POSEDGE)硬件中断触发类型
    if (INTERRUPT_PIN_ACTIVE_LOW == interrupt_configuration->active_level) {
        gpio_intr_type = GPIO_INTR_NEGEDGE;
    } else {
        gpio_intr_type = GPIO_INTR_POSEDGE;
    }

    gpio_config_t int_gpio_config = {
        .mode = GPIO_MODE_INPUT,
        .intr_type = gpio_intr_type,
        .pin_bit_mask = (BIT0 << interrupt_configuration->interrupt_pin)
    };

    // 初始化 ESP32 端与之连接的 GPIO 引脚
    ret = gpio_config(&int_gpio_config);

    return ret;
}

/**
 * @brief 在 ESP32 系统上注册 MPU 数据中断回调 (当数据准备好时快速提醒主控)
 */
esp_err_t mpu6050_register_isr(mpu6050_handle_t sensor, const mpu6050_isr_t isr)
{
    esp_err_t ret;
    mpu6050_dev_t *sensor_device = (mpu6050_dev_t *) sensor;

    if (NULL == sensor_device) {
        ret = ESP_ERR_INVALID_ARG;
        return ret;
    }

    ret = gpio_isr_handler_add(
              sensor_device->int_pin,
              ((gpio_isr_t) * (isr)),
              ((void *) sensor)
          );

    if (ESP_OK != ret) {
        return ret;
    }

    ret = gpio_intr_enable(sensor_device->int_pin);

    return ret;
}

/**
 * @brief 开启特定的 MPU 内置中断源输出（如：数据已就绪(Data Ready) 或 FIFO满溢）
 */
esp_err_t mpu6050_enable_interrupts(mpu6050_handle_t sensor, uint8_t interrupt_sources)
{
    esp_err_t ret;
    uint8_t enabled_interrupts = 0x00;

    ret = mpu6050_read(sensor, MPU6050_INTR_ENABLE, &enabled_interrupts, 1);

    if (ESP_OK != ret) {
        return ret;
    }

    if (enabled_interrupts != interrupt_sources) {

        enabled_interrupts |= interrupt_sources;

        ret = mpu6050_write(sensor, MPU6050_INTR_ENABLE, &enabled_interrupts, 1);
    }

    return ret;
}

/**
 * @brief 关闭指定的 MPU 中断源输出
 */
esp_err_t mpu6050_disable_interrupts(mpu6050_handle_t sensor, uint8_t interrupt_sources)
{
    esp_err_t ret;
    uint8_t enabled_interrupts = 0x00;

    ret = mpu6050_read(sensor, MPU6050_INTR_ENABLE, &enabled_interrupts, 1);

    if (ESP_OK != ret) {
        return ret;
    }

    if (0 != (enabled_interrupts & interrupt_sources)) {
        enabled_interrupts &= (~interrupt_sources);

        ret = mpu6050_write(sensor, MPU6050_INTR_ENABLE, &enabled_interrupts, 1);
    }

    return ret;
}

/**
 * @brief 获取并在原端清除中断状态位
 * @details 读取 `MPU6050_INTR_STATUS` 寄存器会自动清除中断硬件信号，为下一次中断让路。
 */
esp_err_t mpu6050_get_interrupt_status(mpu6050_handle_t sensor, uint8_t *const out_intr_status)
{
    esp_err_t ret;

    if (NULL == out_intr_status) {
        ret = ESP_ERR_INVALID_ARG;
        return ret;
    }

    ret = mpu6050_read(sensor, MPU6050_INTR_STATUS, out_intr_status, 1);

    return ret;
}

/*
 * 一组简易的位掩码宏包装函数：分别检查中断源是不是 因为数据采样准备完毕 / I2C 主机模式出错 / FIFO 缓冲管溢出 等导致的。
 */
inline uint8_t mpu6050_is_data_ready_interrupt(uint8_t interrupt_status)
{
    return (MPU6050_DATA_RDY_INT_BIT == (MPU6050_DATA_RDY_INT_BIT & interrupt_status));
}

inline uint8_t mpu6050_is_i2c_master_interrupt(uint8_t interrupt_status)
{
    return (uint8_t) (MPU6050_I2C_MASTER_INT_BIT == (MPU6050_I2C_MASTER_INT_BIT & interrupt_status));
}

inline uint8_t mpu6050_is_fifo_overflow_interrupt(uint8_t interrupt_status)
{
    return (uint8_t) (MPU6050_FIFO_OVERFLOW_INT_BIT == (MPU6050_FIFO_OVERFLOW_INT_BIT & interrupt_status));
}

/**
 * @brief 从 MPU 提取三轴加速度的原始 16 位 ADC 数值 (未除以 Sensitivity 量程转换)
 * @details I2C 读取出来的是分离的两个字节，必须手动 High_Byte 左移 8 位后加上 Low_Byte 才能拼成一个完整的 16-bit 宽的有符号数字。
 */
esp_err_t mpu6050_get_raw_acce(mpu6050_handle_t sensor, mpu6050_raw_acce_value_t *const raw_acce_value)
{
    uint8_t data_rd[6];
    // x起, y次之, z末：连读 6 个寄存器一次性把三轴的 高低位 (3 x 2) 全收下来，减少 I2C 寻址握手开销
    esp_err_t ret = mpu6050_read(sensor, MPU6050_ACCEL_XOUT_H, data_rd, sizeof(data_rd));

    raw_acce_value->raw_acce_x = (int16_t)((data_rd[0] << 8) + (data_rd[1]));
    raw_acce_value->raw_acce_y = (int16_t)((data_rd[2] << 8) + (data_rd[3]));
    raw_acce_value->raw_acce_z = (int16_t)((data_rd[4] << 8) + (data_rd[5]));
    return ret;
}

/**
 * @brief 从 MPU 提取三轴陀螺仪的原始 16 位 ADC 数值
 */
esp_err_t mpu6050_get_raw_gyro(mpu6050_handle_t sensor, mpu6050_raw_gyro_value_t *const raw_gyro_value)
{
    uint8_t data_rd[6];
    esp_err_t ret = mpu6050_read(sensor, MPU6050_GYRO_XOUT_H, data_rd, sizeof(data_rd));

    raw_gyro_value->raw_gyro_x = (int16_t)((data_rd[0] << 8) + (data_rd[1]));
    raw_gyro_value->raw_gyro_y = (int16_t)((data_rd[2] << 8) + (data_rd[3]));
    raw_gyro_value->raw_gyro_z = (int16_t)((data_rd[4] << 8) + (data_rd[5]));

    return ret;
}

/**
 * @brief 读取并转换为标准的真实物理加速度度量 (单位为 `g` ，重力加速度)
 */
esp_err_t mpu6050_get_acce(mpu6050_handle_t sensor, mpu6050_acce_value_t *const acce_value)
{
    esp_err_t ret;
    float acce_sensitivity;
    mpu6050_raw_acce_value_t raw_acce;

    // 先查当前配置档测算出换算标准率，再读 ADC
    ret = mpu6050_get_acce_sensitivity(sensor, &acce_sensitivity);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mpu6050_get_raw_acce(sensor, &raw_acce);
    if (ret != ESP_OK) {
        return ret;
    }

    // 例如读出来 raw = 16384，并且量程配置在 2g 档位 (sensitivity = 16384)，那么计算出当前承受重力为 = 1g
    acce_value->acce_x = raw_acce.raw_acce_x / acce_sensitivity;
    acce_value->acce_y = raw_acce.raw_acce_y / acce_sensitivity;
    acce_value->acce_z = raw_acce.raw_acce_z / acce_sensitivity;
    return ESP_OK;
}

/**
 * @brief 读取并转换为标准的真实物理角速度 (单位为度每秒 °/s)
 */
esp_err_t mpu6050_get_gyro(mpu6050_handle_t sensor, mpu6050_gyro_value_t *const gyro_value)
{
    esp_err_t ret;
    float gyro_sensitivity;
    mpu6050_raw_gyro_value_t raw_gyro;

    ret = mpu6050_get_gyro_sensitivity(sensor, &gyro_sensitivity);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mpu6050_get_raw_gyro(sensor, &raw_gyro);
    if (ret != ESP_OK) {
        return ret;
    }

    gyro_value->gyro_x = raw_gyro.raw_gyro_x / gyro_sensitivity;
    gyro_value->gyro_y = raw_gyro.raw_gyro_y / gyro_sensitivity;
    gyro_value->gyro_z = raw_gyro.raw_gyro_z / gyro_sensitivity;
    return ESP_OK;
}

/**
 * @brief 获取 MPU6050 内部芯片温度
 * @details MPU6050 自带温度补偿，按照官方 Datasheet 的换算公式: Temp = (Raw / 340) + 36.53 摄氏度
 */
esp_err_t mpu6050_get_temp(mpu6050_handle_t sensor, mpu6050_temp_value_t *const temp_value)
{
    uint8_t data_rd[2];
    esp_err_t ret = mpu6050_read(sensor, MPU6050_TEMP_XOUT_H, data_rd, sizeof(data_rd));
    temp_value->temp = (int16_t)((data_rd[0] << 8) | (data_rd[1])) / 340.00 + 36.53;
    return ret;
}

/**
 * @brief 互补滤波算法 (Core Algorithm)
 * @details 用于将加速度计(去漂移)与陀螺仪(去高频震荡)的数据融合，结算出稳定的姿态角(欧拉角：Roll 和 Pitch)。
 * @note 此算法通过传感器结构中缓存的 `timer` 进行两次采样前后的动态时间积分 (`dt`) 的差值运算来实现追踪。
 */
esp_err_t mpu6050_complimentory_filter(mpu6050_handle_t sensor, const mpu6050_acce_value_t *const acce_value,
                                       const mpu6050_gyro_value_t *const gyro_value, complimentary_angle_t *const complimentary_angle)
{
    float acce_angle[2];
    float gyro_angle[2];
    float gyro_rate[2];
    mpu6050_dev_t *sens = (mpu6050_dev_t *) sensor;

    sens->counter++;
    // 如果是第一次采样，陀螺仪无法积分(无历史)，完全采信加速度计的反三角函数算出来的绝对重力姿态角初始化开局
    if (sens->counter == 1) {
        // 利用 arctan2 求出当前重力分量在轴上的映射角度
        acce_angle[0] = (atan2(acce_value->acce_y, acce_value->acce_z) * RAD_TO_DEG);
        acce_angle[1] = (atan2(acce_value->acce_x, acce_value->acce_z) * RAD_TO_DEG);
        complimentary_angle->roll = acce_angle[0];
        complimentary_angle->pitch = acce_angle[1];
        gettimeofday(sens->timer, NULL);
        return ESP_OK;
    }

    // 动态计算本次采样与上次采样实际消耗的时间差 `dt`，以秒为单位
    struct timeval now, dt_t;
    gettimeofday(&now, NULL);
    timersub(&now, sens->timer, &dt_t);
    sens->dt = (float) (dt_t.tv_sec) + (float)dt_t.tv_usec / 1000000;
    gettimeofday(sens->timer, NULL);

    // 1. 求新的加速度计标量角绝对值
    acce_angle[0] = (atan2(acce_value->acce_y, acce_value->acce_z) * RAD_TO_DEG);
    acce_angle[1] = (atan2(acce_value->acce_x, acce_value->acce_z) * RAD_TO_DEG);

    // 2. 求这段微小时间内陀螺仪新转动的积分角相对值
    gyro_rate[0] = gyro_value->gyro_x;
    gyro_rate[1] = gyro_value->gyro_y;
    gyro_angle[0] = gyro_rate[0] * sens->dt;
    gyro_angle[1] = gyro_rate[1] * sens->dt;

    // 3. 互补滤波结合：99% 相信上一刻角+现积分变化量，用 1% 的加速度绝对值拉扯修正零点温漂
    complimentary_angle->roll = (ALPHA * (complimentary_angle->roll + gyro_angle[0])) + ((1 - ALPHA) * acce_angle[0]);
    complimentary_angle->pitch = (ALPHA * (complimentary_angle->pitch + gyro_angle[1])) + ((1 - ALPHA) * acce_angle[1]);

    return ESP_OK;
}

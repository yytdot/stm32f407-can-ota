/**
  ******************************************************************************
  * @file    i2c_sensors.c
  * @brief   I2C 双传感器驱动实现 —— SHT30 + BH1750
  *
  * I2C1 配置：
  *   PB6 = SCL (AF4), PB7 = SDA (AF4)
  *   快速模式 400kHz，上拉电阻（板载或内部都可）
  ******************************************************************************
  */

#include "i2c_sensors.h"
#include <string.h>
#include <math.h>

/* I2C1 句柄：定义在 main.c（CubeMX 生成），与 SSD1306 驱动共用同一条总线 */
extern I2C_HandleTypeDef hi2c1;


/**
  * @brief  CRC-8/DOW —— SHT30 专用
  *
  * 多项式：x^8 + x^5 + x^4 + 1 = 0x31
  * 初始值：0xFF
  * 结果：无 final xor（直接就是计算值）
  *
  * 和 ota_protocol.h 里的 CRC8 完全不同，注意区分！
  */
bool sht30_crc8_check(const uint8_t *data, uint8_t data_len, uint8_t crc)
{
    uint8_t calc = 0xFF;
    for (uint8_t i = 0; i < data_len; i++) {
        calc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (calc & 0x80) {
                calc = (calc << 1) ^ 0x31;
            } else {
                calc <<= 1;
            }
        }
    }
    return (calc == crc);
}


/**
  * @brief  初始化 I2C1 + 两个传感器
  */
HAL_StatusTypeDef i2c_sensors_init(void)
{
    /* ---- 1. 时钟 / GPIO / 总线时序 ----
     * 全部由 CubeMX 的 MX_I2C1_Init() + HAL_I2C_MspInit() 完成
     * （PB6=SCL, PB7=SDA, AF4 开漏, 100kHz）。
     * 这里不再用另一套参数把总线重新初始化一遍，
     * 否则会覆盖 CubeMX 的配置，甚至把总线拉死。
     */

    /* ---- 4. SHT30 软复位 ---- */
    uint8_t soft_reset_cmd[2] = { SHT30_CMD_SOFT_RESET };
    HAL_I2C_Master_Transmit(&hi2c1, SHT30_I2C_ADDR, soft_reset_cmd, 2, 100);
    HAL_Delay(10);   /* SHT30 复位需要几毫秒稳定 */

    /* ---- 5. BH1750 设置为连续高分辨率模式 ---- */
    uint8_t mode_cmd = BH1750_MODE_HIGH_RES;
    HAL_I2C_Master_Transmit(&hi2c1, BH1750_I2C_ADDR, &mode_cmd, 1, 100);

    return HAL_OK;
}


/**
  * @brief  读 SHT30 温湿度
  *
  * 流程：
  *   1. 发 [0x2C][0x06] 启动单次高压精度测量
  *   2. 等 ~20ms（测量时间）
  *   3. 读回 6 字节数据（温 2B + 温 CRC + 湿 2B + 湿 CRC）
  *   4. 两次 CRC8 校验
  *   5. 转换为 °C 和 %RH
  */
HAL_StatusTypeDef sht30_read(float *temp_out, float *hum_out)
{
    /* 1. 发单次高压精度测量命令 */
    uint8_t cmd[2] = { SHT30_CMD_SINGLE_HIGH };
    if (HAL_I2C_Master_Transmit(&hi2c1, SHT30_I2C_ADDR, cmd, 2, 100) != HAL_OK) {
        return HAL_ERROR;
    }

    /* 2. 等待测量完成 —— SHT30 高压精度需要 ~15ms */
    HAL_Delay(20);

    /* 3. 读 6 字节数据 */
    uint8_t rx[6];
    if (HAL_I2C_Master_Receive(&hi2c1, SHT30_I2C_ADDR, rx, 6, 100) != HAL_OK) {
        return HAL_ERROR;
    }

    /* 4. CRC8 校验（温度前 2B + 湿度前 2B 各一个 CRC） */
    if (!sht30_crc8_check(rx, 2, rx[2]) || !sht30_crc8_check(rx + 3, 2, rx[5])) {
        return HAL_ERROR;   /* CRC 错误 —— 通信丢包或传感器损坏 */
    }

    /* 5. 转换为物理量
     *   raw_temp = rx[0]<<8 | rx[1]  （大端，STM32 内部要转成 uint16_t）
     *   raw_rh   = rx[3]<<8 | rx[4]
     *   t = -45 + 175 * raw_temp / 65535
     *   rh = -6 + 125 * raw_rh / 65535
     */
    uint16_t raw_temp = ((uint16_t)rx[0] << 8) | rx[1];
    uint16_t raw_rh   = ((uint16_t)rx[3] << 8) | rx[4];

    *temp_out = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *hum_out  = -6.0f  + 125.0f * ((float)raw_rh   / 65535.0f);

    /* 湿度做一个 clamp —— 物理上 RH 不会 <0 或 >100 */
    if (*hum_out < 0.0f)  *hum_out = 0.0f;
    if (*hum_out > 100.0f) *hum_out = 100.0f;

    return HAL_OK;
}


/**
  * @brief  读 BH1750 光照强度
  *
  * BH1750 连续模式：不需要发命令触发，直接读就行。
  * 高分辨率模式 → lux = raw / 1.2
  */
HAL_StatusTypeDef bh1750_read(uint16_t *lux_out)
{
    uint8_t rx[2];
    if (HAL_I2C_Master_Receive(&hi2c1, BH1750_I2C_ADDR, rx, 2, 100) != HAL_OK) {
        return HAL_ERROR;
    }

    uint16_t raw = ((uint16_t)rx[0] << 8) | rx[1];

    /* 高分辨率模式换算：lux = raw / 1.2 */
    *lux_out = (uint16_t)((float)raw / 1.2f);

    return HAL_OK;
}

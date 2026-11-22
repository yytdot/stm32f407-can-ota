/**
  ******************************************************************************
  * @file    adc_voltage.c
  * @brief   片内 ADC 电压采集实现
  *
  * ADC1 通道 10 = PC0
  * 单次转换 + 软件多次采样平均
  ******************************************************************************
  */

#include "adc_voltage.h"

/* ADC1 句柄：定义在 main.c（CubeMX 生成） */
extern ADC_HandleTypeDef hadc1;


/**
  * @brief  初始化 ADC1
  */
HAL_StatusTypeDef adc_voltage_init(void)
{
    /* 时钟、PA0 模拟模式、通道 0 配置已由 CubeMX 的
     * MX_ADC1_Init() + HAL_ADC_MspInit() 完成。
     * 注意：原稿用的是 PC0/ADC 通道 10，但本工程 PC0 已经分配给 LED1，
     * 硬件上电压采样走 PA0（ADC1_IN0），所以这里沿用 CubeMX 的通道配置。 */

    return HAL_OK;
}


/**
  * @brief  读平均后电压值
  */
HAL_StatusTypeDef adc_voltage_read(float *voltage_v_out)
{
    uint32_t sum = 0;

    for (int i = 0; i < ADC_SAMPLE_AVG_COUNT; i++) {
        /* 启动一次 ADC 转换 */
        HAL_ADC_Start(&hadc1);

        /* 等转换完成（轮询 EOC 标志） */
        if (HAL_ADC_PollForConversion(&hadc1, 50) != HAL_OK) {
            HAL_ADC_Stop(&hadc1);
            return HAL_ERROR;
        }

        sum += HAL_ADC_GetValue(&hadc1);
    }

    /* 转成电压
     * VADC  = (sum / N) / 4095 * 3.3
     * VIN   = VADC * VOLTAGE_DIV_RATIO
     */
    float avg_raw = (float)(sum / ADC_SAMPLE_AVG_COUNT);
    float v_adc = avg_raw / (float)ADC_RESOLUTION * ADC_VREF_VOLTAGE_V;
    *voltage_v_out = v_adc * VOLTAGE_DIV_RATIO;

    return HAL_OK;
}

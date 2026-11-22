/**
  ******************************************************************************
  * @file    adc_voltage.h
  * @brief   片内 ADC 采集电压（分压电阻）头文件
  *
  * 硬件方案：
  *   - PC0 接分压电阻中点（分压电阻 R上/R下 = 固定值，比如 100k/10k）
  *   - ADC1 通道 10 = PC0
  *   - 输入电压 VIN = 待测量，经 R上+R下 分压后中点电压 VADC = VIN * R下 / (R上+R下)
  *   - 片内 VDDA = 3.3V，ADC 最大输入不能超过 VDDA
  *   - 所以 VIN 可以高于 3.3V（比如 12V 电源电压），通过分压降到 ADC 量程内
  *
  * 分压电阻比：硬件上固定（焊好就不变），这里用宏定义
  *   VOLTAGE_DIV_RATIO = (R上 + R下) / R下 = 实际 VIN / VADC
  *   例如 R上=100k, R下=10k → 比值 = 11
  *   实际 VIN = VADC * VOLTAGE_DIV_RATIO
  *
  * 多采集平均：
  *   单次 ADC 采样噪声较大，这里做 10 次采样取平均，
  *   相当于降噪 3 倍多（sqrt(10)）。
  ******************************************************************************
  */

#ifndef __ADC_VOLTAGE_H
#define __ADC_VOLTAGE_H

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 分压电阻比 —— 根据实际硬件修改
 * ==========================================================================*/

/**
  * @brief  分压电阻比值 = (R上 + R下) / R下
  *
  * 举例：
  *   如果硬件焊的是 R上=100kΩ, R下=10kΩ → 比值 = (100+10)/10 = 11
  *   如果硬件焊的是 R上=470kΩ, R下=10kΩ → 比值 = (470+10)/10 = 48
  *
  * 面试时说清楚：
  *   "分压电阻比决定了我们能测多大的电压。
  *    3.3V 片内 ADC，比值 11 就能测最高 3.63V * 11 ≈ 40V。"
  */
#ifndef VOLTAGE_DIV_RATIO
#define VOLTAGE_DIV_RATIO           11.0f
#endif

/** @brief ADC 参考电压（理论值，实际通过校准更准） */
#define ADC_VREF_VOLTAGE_V          3.3f

/** @brief ADC 分辨率（12bit → 0~4095） */
#define ADC_RESOLUTION              4095

/** @brief 单次 ADC 采样后做 N 次平均 */
#define ADC_SAMPLE_AVG_COUNT        10


/* ============================================================================
 * API
 * ==========================================================================*/

/**
  * @brief  初始化 ADC1 通道 10 (PC0)
  *
  * 配置：
  *   - 12bit 分辨率
  *   - 单次转换模式（不连续）
  *   - 通道 10 (PC0)，采样时间 28 cycles（中值，兼顾精度和速度）
  */
HAL_StatusTypeDef adc_voltage_init(void);

/**
  * @brief  读一次平均后的电压值（V）
  * @param  voltage_v_out  输出参数，实际被测电压（已经除以分压比）
  * @return HAL_OK 成功
  *
  * 内部做 N 次采样取平均，然后换算成 VIN = VADC * VOLTAGE_DIV_RATIO
  */
HAL_StatusTypeDef adc_voltage_read(float *voltage_v_out);

#ifdef __cplusplus
}
#endif

#endif /* __ADC_VOLTAGE_H */

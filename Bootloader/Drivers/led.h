/**
  ******************************************************************************
  * @file    led.h
  * @brief   双 LED 指示灯驱动头文件
  *
  * 硬件：
  *   - LED_RUN    → 运行指示灯（正常状态下 1Hz 呼吸/闪烁）
  *   - LED_UPGRADE → 升级指示灯（OTA 过程中快闪，升级完成后长亮）
  *
  * 引脚（可按需修改）：
  *   PB0 - LED_RUN
  *   PB1 - LED_UPGRADE
  *
  * 低电平有效（共阳极接法：LED 正极接 VCC，负极接 IO，低电平导通）
  * 本驱动的 ON/OFF 宏已经处理了极性，上层不用关心。
  ******************************************************************************
  */

#ifndef __LED_H
#define __LED_H

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 引脚按本工程实际硬件对齐（与 CubeMX 的 main.h 一致）：
 *   PC0 = LED1（运行灯）  PC1 = LED2（升级指示灯） */
#define LED_RUN_PIN         GPIO_PIN_0
#define LED_RUN_PORT        GPIOC
#define LED_UPGRADE_PIN     GPIO_PIN_1
#define LED_UPGRADE_PORT    GPIOC

/** @brief 初始化 LED GPIO（推挽输出，上拉，初始熄灭） */
void led_init(void);

/** @brief 运行灯开（低电平有效，内部已反转） */
#define LED_RUN_ON()        HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_RESET)
/** @brief 运行灯关 */
#define LED_RUN_OFF()       HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_SET)
/** @brief 运行灯翻转 */
#define LED_RUN_TOGGLE()    HAL_GPIO_TogglePin(LED_RUN_PORT, LED_RUN_PIN)

#define LED_UPGRADE_ON()    HAL_GPIO_WritePin(LED_UPGRADE_PORT, LED_UPGRADE_PIN, GPIO_PIN_RESET)
#define LED_UPGRADE_OFF()   HAL_GPIO_WritePin(LED_UPGRADE_PORT, LED_UPGRADE_PIN, GPIO_PIN_SET)
#define LED_UPGRADE_TOGGLE() HAL_GPIO_TogglePin(LED_UPGRADE_PORT, LED_UPGRADE_PIN)

#ifdef __cplusplus
}
#endif

#endif /* __LED_H */

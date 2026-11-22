/**
  ******************************************************************************
  * @file    task_key_led.h
  * @brief   按键 & LED 任务头文件 —— 最低优先级
  *
  * 功能：
  *   1. 按键检测（短按 = 无操作，长按 3 秒 = 触发 OTA 升级）
  *   2. LED 状态指示：
  *      - 正常运行：运行灯 2Hz 呼吸闪烁
  *      - 异常（传感器全部失败）：运行灯快闪 5Hz
  *
  * 优先级：最低 (0) —— 让所有业务任务先跑
  ******************************************************************************
  */

#ifndef __TASK_KEY_LED_H
#define __TASK_KEY_LED_H

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_KEY_LED_STACK_SIZE     256
#define TASK_KEY_LED_PRIORITY       0

/** @brief 长按触发升级的阈值（ms） */
#define KEY_LONG_PRESS_MS           3000

BaseType_t task_key_led_create(void);
void task_key_led_main(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* __TASK_KEY_LED_H */

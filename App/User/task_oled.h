/**
  ******************************************************************************
  * @file    task_oled.h
  * @brief   OLED 显示任务头文件
  *
  * FreeRTOS 任务优先级：低 (1) —— 和传感器采集同级
  * 周期：由消息队列驱动（有数据就显示，没数据就阻塞等）
  * 功能：
  *   从传感器数据队列 pop 一帧，格式化成 OLED 显示内容并刷新屏幕
  ******************************************************************************
  */

#ifndef __TASK_OLED_H
#define __TASK_OLED_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_OLED_STACK_SIZE        512
#define TASK_OLED_PRIORITY          1

BaseType_t task_oled_create(QueueHandle_t sensor_queue);
void task_oled_main(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* __TASK_OLED_H */

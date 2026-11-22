/**
  ******************************************************************************
  * @file    task_sensor.h
  * @brief   传感器采集任务头文件
  *
  * FreeRTOS 任务优先级：低 (1)
  * 周期：500ms
  * 功能：
  *   1. I2C 读取 SHT30（温湿度）+ BH1750（光照）
  *   2. ADC 采集分压电阻电压
  *   3. 把所有数据打包成 sensor_data_t，push 到任务间消息队列
  *
  * 任务间数据流向：
  *   传感器采集任务 ──(消息队列)──► OLED 显示任务 + CAN 通信任务
  *
  * 为什么用消息队列？
  *   - 生产者-消费者模式，天然解耦
  *   - FreeRTOS 消息队列是线程安全的
  *   - 队列满时采集任务会阻塞等待（xQueueSend 返回 errQUEUE_FULL）
  ******************************************************************************
  */

#ifndef __TASK_SENSOR_H
#define __TASK_SENSOR_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "../Drivers/i2c_sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 传感器采集任务栈大小（words，不是 bytes！FreeRTOS 以 StackType_t 为单位） */
#define TASK_SENSOR_STACK_SIZE      512

/** @brief 传感器采集任务优先级 —— 低 */
#define TASK_SENSOR_PRIORITY        1

/** @brief 采集周期（ms） */
#define TASK_SENSOR_PERIOD_MS       500

/**
  * @brief  创建传感器采集任务
  * @param  sensor_queue  消息队列句柄（由 main 创建后传入）
  * @return pdPASS 成功
  */
BaseType_t task_sensor_create(QueueHandle_t sensor_queue);

/**
  * @brief  FreeRTOS 任务入口函数（static 在 .c 里实现）
  *         签名必须是：void func(void *pvParameters)
  */
void task_sensor_main(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* __TASK_SENSOR_H */

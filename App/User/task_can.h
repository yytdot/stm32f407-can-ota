/**
  ******************************************************************************
  * @file    task_can.h
  * @brief   CAN 通信任务头文件 —— 高优先级
  *
  * 功能：
  *   1. 常规：定时把最新传感器数据广播到 CAN 总线
  *   2. 识别 OTA 指令：
  *      收到上位机发来的 OTA 启动命令 (CMD_HANDSHAKE 或专门的升级触发命令)
  *      → 写升级标记到 W25Q64 → NVIC_SystemReset() → 进入 Bootloader
  *
  * 优先级：高 (3)
  *   —— OTA 指令处理要快，不能被低优先级任务长时间阻塞
  *   —— 传感器数据上传也是周期性操作，但响应 OTA 指令必须及时
  *
  * 任务间数据流：
  *   sensor_queue (pop 最新数据) ──► CAN 任务 ──► CAN 总线 (push)
  *   CAN 总线 (OTA 指令) ──► can_app_rx_queue (ISR push) ──► CAN 任务 (pop)
  ******************************************************************************
  */

#ifndef __TASK_CAN_H
#define __TASK_CAN_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_CAN_STACK_SIZE         512
#define TASK_CAN_PRIORITY           3

/** @brief 传感器数据 CAN 广播周期（ms） */
#define CAN_BROADCAST_PERIOD_MS     1000

BaseType_t task_can_create(QueueHandle_t sensor_queue);
void task_can_main(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* __TASK_CAN_H */

/**
  ******************************************************************************
  * @file    can_app.h
  * @brief   App 侧 CAN1 驱动头文件（FreeRTOS 版本）
  *
  * 和 Bootloader 的 can_boot 对比：
  *   - Bootloader 是裸机，用环形缓冲区 + 主循环取帧
  *   - App 侧用 FreeRTOS：
  *     ISR 里把 CAN 帧 push 到 FreeRTOS 消息队列
  *     CAN 通信任务从队列里取帧处理
  *   - 同时支持：业务数据上传（传感器数据广播）+ OTA 指令接收
  *
  * 硬件完全一样：PA11=RX, PA12=TX, 500kbps
  * 过滤器配置：放更多 ID（业务数据接收 + OTA 接收 + 可能的调试命令）
  ******************************************************************************
  */

#ifndef __CAN_APP_H
#define __CAN_APP_H

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <stdint.h>
#include "../../Common/ota_protocol.h"
#include "../Drivers/i2c_sensors.h"   /* 用到 sensor_data_t */

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================================
 * CAN 接收队列句柄 —— 任务间共享
 *
 * ISR 里调用 xQueueSendToBackFromISR() push 一帧
 * CAN 任务调用 xQueueReceive() pop 一帧
 *
 * 队列大小：CAN_RX_QUEUE_SIZE 帧，每帧结构见 can_app_frame_t
 */
#define CAN_RX_QUEUE_SIZE           8


/**
  * @brief  App 侧 CAN 帧结构体 —— 比 Bootloader 版多一个时间戳
  */
typedef struct {
    uint32_t ext_id;
    uint8_t  dlc;
    uint8_t  data[8];
    uint32_t rx_tick;   /**< 接收时刻的 FreeRTOS tick，用于超时判断 */
} can_app_frame_t;


/* ============================================================================
 * API
 * ==========================================================================*/

/** @brief 初始化 CAN1（GPIO + 参数 + 过滤器 + 中断 + 创建接收队列） */
HAL_StatusTypeDef can_app_init(void);

/**
  * @brief  发送传感器数据到 CAN 总线（业务数据帧）
  *
  * 格式：
  *   ID  = OTA_CAN_ID_APP_DATA (0x0CF00001)
  *   DLC = 8（8 字节有效载荷）
  *   data[0..3] = 温度（float，小端）
  *   data[4..5] = 光照（uint16，小端）
  *   data[6]    = 电压（0.1V 为单位，即 uint16(voltage_v * 10) 存 data[6..7]）
  *   data[7]    = 帧计数（每次 +1）
  */
HAL_StatusTypeDef can_app_send_sensor(const sensor_data_t *sensor);

/** @brief 发送通用扩展帧（OTA ACK 等） */
HAL_StatusTypeDef can_app_send_ext(uint32_t ext_id, const uint8_t *data, uint8_t dlc);

/**
  * @brief  从接收队列取一帧（CAN 任务调用，blocking）
  * @param  frame_out  输出
  * @param  timeout    阻塞等待时长，portMAX_DELAY 表示一直等
  */
BaseType_t can_app_receive(can_app_frame_t *frame_out, TickType_t timeout);

/**
  * @brief  HAL CAN RX 回调（被 stm32f4xx_it.c → HAL_CAN_IRQHandler → 这里调用）
  *         把 ISR 收到的 CAN 帧 push 到 FreeRTOS 接收队列
  */
void can_app_rx0_callback(void);

#ifdef __cplusplus
}
#endif

#endif /* __CAN_APP_H */

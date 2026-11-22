/**
  ******************************************************************************
  * @file    task_can.c
  * @brief   CAN 通信任务实现
  *
  * 一个任务同时干两件事：
  *   1. 定时广播传感器数据
  *   2. 监听 OTA 指令，收到后触发升级
  *
  * 实现策略：用"非阻塞接收 + 周期性发送"组合
  *
  * 主循环结构：
  *   while(1) {
  *       1. 尝试从 CAN 接收队列 pop 一帧（短超时，比如 50ms）
  *          - 有帧 → 解析处理（可能是 OTA 触发指令）
  *       2. 检查"距离上次广播是否 >= 1000ms"
  *          - 到时间了 → 从 sensor_queue pop 最新数据 → CAN 发送
  *   }
  ******************************************************************************
  */

#include "task_can.h"
#include "../Drivers/can_app.h"
#include "../Drivers/i2c_sensors.h"
#include "../User/app_upgrade_trigger.h"
#include "../../Common/ota_protocol.h"

/* 传感器数据队列（广播用） */
static QueueHandle_t s_sensor_queue;


BaseType_t task_can_create(QueueHandle_t sensor_queue)
{
    return xTaskCreate(
        task_can_main,
        "CanTask",
        TASK_CAN_STACK_SIZE,
        (void*)sensor_queue,
        TASK_CAN_PRIORITY,
        NULL
    );
}


void task_can_main(void *pvParameters)
{
    s_sensor_queue = (QueueHandle_t)pvParameters;

    /* 上次广播时间戳 —— 用 tick 计数 */
    TickType_t last_broadcast_tick = xTaskGetTickCount();

    while (1) {

        /* ==============================================================
         * 第一步：检查 CAN 接收队列，有没有 OTA 指令或其他命令
         * ==============================================================
         *
         * 用 pdMS_TO_TICKS(50) 短超时 —— 最多等 50ms，
         * 这样不会因为等 CAN 消息而耽误定时广播。
         * 如果超时返回 pdFALSE，说明这段时间没 CAN 消息，
         * 直接跳到第二步做广播。
         */
        can_app_frame_t rx_frame;
        while (can_app_receive(&rx_frame, pdMS_TO_TICKS(10)) == pdTRUE) {

            /* 只处理 OTA_CAN_ID_HOST_TX 的帧（上位机发的） */
            if (rx_frame.ext_id != OTA_CAN_ID_HOST_TX) {
                continue;   /* 不是我们要的，跳过（不丢，队列就这么用） */
            }

            /* ---- 识别 OTA 指令 ----
             *
             * OTA 触发条件：收到 OTA_CAN_ID_HOST_TX 的 CMD_HANDSHAKE (0x01)
             * 且 DLC == 1（严格判定，避免数据帧首字节刚好是 0x01 时误触发）。
             *
             * 设计决策：
             *   收到 CMD_HANDSHAKE 说明上位机想升级了，
             *   我们 App 侧触发 → 写标记 + 软复位 → 进 Bootloader。
             *   Bootloader 读到升级标记 + 收到同一个 CMD_HANDSHAKE，
             *   就会回 ACK_HANDSHAKE 并进入 WAIT_HEADER 状态。
             *
             *   这样 App 和 Bootloader 之间通过"W25Q64 升级标记"
             *   做了一个"握手接力"：App 触发 → Bootloader 接手。
             *   这条 CAN 消息不用 App 再回复——Bootloader 会回。
             *
             * ⚠️ 升级触发后永不返回！NVIC_SystemReset 会复位 CPU。
             */
            if (rx_frame.dlc == 1 && rx_frame.data[0] == CMD_HANDSHAKE) {
                /* 回复上位机一个 ACK，让它知道"我们收到了，马上进 Bootloader" */
                uint8_t ack = ACK_HANDSHAKE;
                can_app_send_ext(OTA_CAN_ID_DEV_TX, &ack, 1);

                /* 稍微等一下让 ACK 发出去（HAL 内部会自动等邮箱空闲） */

                /* 触发升级！永不返回 */
                upgrade_trigger_request(UPGRADE_FLAG_TRIGGER);
            }

            /* 其他 CAN 指令可以在这里扩展（比如远程查询版本号、远程控制 LED 等）
             * 目前只处理 OTA 触发 */
        }


        /* ==============================================================
         * 第二步：定时广播传感器数据
         * ==============================================================*/
        TickType_t now = xTaskGetTickCount();

        if ((now - last_broadcast_tick) >= pdMS_TO_TICKS(CAN_BROADCAST_PERIOD_MS)) {
            last_broadcast_tick = now;

            /* 从 sensor_queue pop 一帧最新数据
             * 这里不阻塞（timeout = 0）——如果队列里没数据，这次就跳过
             * 因为传感器采集任务是 500ms 周期、CAN 广播是 1000ms 周期，
             * 理论上队列里肯定有数据。超时 0 是非阻塞 pop。
             */
            sensor_data_t latest_data;
            if (xQueueReceive(s_sensor_queue, &latest_data, 0) == pdTRUE) {

                /* 广播到 CAN 总线 */
                can_app_send_sensor(&latest_data);
            }
        }


        /* ==============================================================
         * 第三步：让出 CPU（短暂延时）
         * ==============================================================
         *
         * vTaskDelay(10ms) 让其他低优先级任务（OLED、按键LED、传感器采集）
         * 有机会运行。CAN 任务是最高优先级，如果不 delay，其他任务可能饿死。
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

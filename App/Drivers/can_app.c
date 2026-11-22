/**
  ******************************************************************************
  * @file    can_app.c
  * @brief   App 侧 CAN 驱动实现（FreeRTOS 版本）
  *
  * ISR → FreeRTOS 队列 → CAN 任务解耦
  *
  * FreeRTOS 安全规则：
  *   - ISR 里只能调用 FromISR 结尾的 API：
  *     xQueueSendToBackFromISR, portYIELD_FROM_ISR 等
  *   - ISR 里绝对不能调 printf / malloc / 非 FromISR 的队列 API
  *   - 必须用 BaseType_t xHigherPriorityTaskWoken 变量判断要不要触发上下文切换
  ******************************************************************************
  */

#include "can_app.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* CAN1 句柄：定义在 main.c（CubeMX 生成），
 * 这样 CAN1_RX0_IRQHandler 里的 HAL_CAN_IRQHandler(&hcan1) 和这里操作的是同一个句柄 */
extern CAN_HandleTypeDef hcan1;

/* FreeRTOS 接收队列 —— 全局句柄，ISR 和任务都要访问 */
QueueHandle_t s_can_rx_queue;


/**
  * @brief  App 侧 CAN1 初始化
  *
  * 和 Bootloader 版的区别：
  *   1. 过滤器列表里加 OTA 接收 ID + 业务接收 ID
  *   2. 额外创建 FreeRTOS 接收队列
  *   3. 波特率配置一模一样（同一根 CAN 总线）
  */
HAL_StatusTypeDef can_app_init(void)
{
    /* 时钟、PA11/PA12 复用、位时序参数（500kbps）已由 CubeMX 的
     * MX_CAN1_Init() + HAL_CAN_MspInit() 完成，这里不重复初始化。
     * 但 HAL_CAN_Init 不会配过滤器——不配过滤器 FIFO0 收不到任何帧，
     * 所以下面这一段必须保留。 */

    /* 过滤器 0：列表模式，只收 OTA_CAN_ID_HOST_TX (0x18F00001) */
    CAN_FilterTypeDef filter = {0};
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDLIST;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = (uint16_t)((OTA_CAN_ID_HOST_TX >> 13) & 0xFFFF);
    filter.FilterIdLow          = (uint16_t)((OTA_CAN_ID_HOST_TX & 0x1FFF) | 0x0400);
    filter.FilterMaskIdHigh     = 0;
    filter.FilterMaskIdLow      = 0;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation     = ENABLE;
    filter.SlaveStartFilterBank = 14;
    HAL_CAN_ConfigFilter(&hcan1, &filter);

    /* 启动 CAN + 开中断 */
    HAL_CAN_Start(&hcan1);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 5, 0);   /* FreeRTOS 下优先级不能太高（>=5 才行） */
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);

    /* 创建 FreeRTOS 接收队列
     * 队列大小 = CAN_RX_QUEUE_SIZE 帧
     * 每帧大小 = can_app_frame_t
     * 这一步之前不能开中断——否则 ISR 里访问空队列句柄会 HardFault
     */
    s_can_rx_queue = xQueueCreate(CAN_RX_QUEUE_SIZE, sizeof(can_app_frame_t));
    if (s_can_rx_queue == NULL) {
        return HAL_ERROR;   /* 堆空间不够，FreeRTOS 配置问题 */
    }

    return HAL_OK;
}


/**
  * @brief  ISR 回调：把 HAL 收到的 CAN 帧 push 到 FreeRTOS 队列
  *
  * 注意：ISR 里必须用 xQueueSendToBackFromISR，
  * 不能用 xQueueSendToBack（普通 API 会阻塞）。
  * 还要用 portYIELD_FROM_ISR 判断要不要触发上下文切换——
  * 如果接收队列里的 CAN 帧把 CAN 任务唤醒了，而它的优先级又刚好
  * 比当前被中断的任务优先级高，那么 ISR 退出时就应该立刻切换过去。
  */
void can_app_rx0_callback(void)
{
    CAN_RxHeaderTypeDef rx_header = {0};
    uint8_t rx_data[8];

    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx_header, rx_data) == HAL_OK) {

        can_app_frame_t frame = {0};
        frame.ext_id   = rx_header.ExtId;
        frame.dlc      = rx_header.DLC;
        frame.rx_tick  = xTaskGetTickCountFromISR();
        memcpy(frame.data, rx_data, rx_header.DLC);

        /* push 到队列 */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendToBackFromISR(s_can_rx_queue, &frame, &xHigherPriorityTaskWoken);

        /* 如果有高优先级任务被唤醒，触发上下文切换 */
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}


/**
  * @brief  普通任务从 FreeRTOS 队列取一帧（阻塞等待）
  *
  * @param  frame_out  输出
  * @param  timeout    超时，portMAX_DELAY 表示永远等
  * @return pdTRUE 成功，pdFALSE 超时
  */
BaseType_t can_app_receive(can_app_frame_t *frame_out, TickType_t timeout)
{
    return xQueueReceive(s_can_rx_queue, frame_out, timeout);
}


/**
  * @brief  发送业务数据帧（传感器数据广播）
  */
HAL_StatusTypeDef can_app_send_sensor(const sensor_data_t *sensor)
{
    CAN_TxHeaderTypeDef tx = {0};
    tx.ExtId = OTA_CAN_ID_APP_DATA;
    tx.IDE   = CAN_ID_EXT;
    tx.RTR   = CAN_RTR_DATA;
    tx.DLC   = 8;
    tx.TransmitGlobalTime = DISABLE;

    /* 组装 8 字节 payload */
    uint8_t data[8];
    uint32_t temp_raw;
    memcpy(&temp_raw, &sensor->temperature_c, 4);   /* float → uint32_t 小端 */
    data[0] = (uint8_t)(temp_raw & 0xFF);
    data[1] = (uint8_t)((temp_raw >> 8) & 0xFF);
    data[2] = (uint8_t)((temp_raw >> 16) & 0xFF);
    data[3] = (uint8_t)((temp_raw >> 24) & 0xFF);
    data[4] = (uint8_t)(sensor->light_lux & 0xFF);
    data[5] = (uint8_t)((sensor->light_lux >> 8) & 0xFF);
    /* 电压放大 10 倍存 uint16 */
    uint16_t v_raw = (uint16_t)(sensor->voltage_v * 10.0f);
    data[6] = (uint8_t)(v_raw & 0xFF);
    data[7] = (uint8_t)((v_raw >> 8) & 0xFF);

    uint32_t mailbox_id = 0;
    return HAL_CAN_AddTxMessage(&hcan1, &tx, data, &mailbox_id);
}


/**
  * @brief  发送任意扩展帧（OTA ACK 等）
  */
HAL_StatusTypeDef can_app_send_ext(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    CAN_TxHeaderTypeDef tx = {0};
    tx.ExtId = ext_id;
    tx.IDE   = CAN_ID_EXT;
    tx.RTR   = CAN_RTR_DATA;
    tx.DLC   = dlc;
    tx.TransmitGlobalTime = DISABLE;

    uint32_t mailbox_id = 0;
    return HAL_CAN_AddTxMessage(&hcan1, &tx, (uint8_t*)data, &mailbox_id);
}

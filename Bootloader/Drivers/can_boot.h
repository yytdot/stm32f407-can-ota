/**
  ******************************************************************************
  * @file    can_boot.h
  * @brief   Bootloader 侧裸机 CAN 驱动头文件
  *
  * Bootloader 是裸机（无 FreeRTOS），所以 CAN 接收用中断 + 环形缓冲区：
  *   - CAN1_RX0_IRQHandler 里把收到的 CAN 帧塞进环形缓冲区
  *   - 主循环（状态机）里从环形缓冲区取出帧，做 OTA 协议解析
  *
  * ⚠️ 和 App 侧的 CAN 驱动（can_app.c/h）职责不同：
  *   - App 侧用 FreeRTOS 队列 + 任务解耦，支持业务数据上传
  *   - Bootloader 侧只收 OTA 协议帧，简单高效
  *
  * 硬件连线：
  *   STM32F407 CAN1 默认引脚：
  *     PA11 - CAN1_RX (AF9)
  *     PA12 - CAN1_TX (AF9)
  *   TJA1050 连接：
  *     TXD ← PA12
  *     RXD → PA11
  *     VCC = 3.3V, GND = GND
  *     CANH / CANL 接总线，两端 120Ω 终端电阻
  *
  * CAN 波特率配置：500kbps（常用值，距离远可选 125kbps）
  * ==========================================================================*/

#ifndef __CAN_BOOT_H
#define __CAN_BOOT_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"
#include "../../Common/ota_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CAN 帧结构体 —— 环形缓冲区存的就是这个
 * ==========================================================================*/

/**
  * @brief  简化版 CAN 帧（只存 Bootloader OTA 需要的字段）
  *
  * 环形缓冲区里存的就是这个结构体，中断里直接 memcpy 进去。
  *
  * 为什么不用 HAL 的 CAN_RxHeaderTypeDef + 单独 data buf？
  *   —— HAL 是分开的（header 一个 struct，data 单独数组），
  *      这样在环形缓冲区里存不方便，还要做两次 memcpy。
  *   —— 我们把两者合并成一个紧凑的结构体，中断里一次 memcpy 就搞定。
  */
typedef struct {
    uint32_t std_id;       /**< 标准帧 ID（我们用扩展帧，这里实际填 ext_id 高 11bit） */
    uint32_t ext_id;       /**< 扩展帧 29-bit ID */
    bool     is_extended;  /**< true=扩展帧，false=标准帧 */
    uint8_t  dlc;          /**< 数据长度 0~8 */
    uint8_t  data[8];      /**< 数据区，最多 8 字节（标准 CAN 2.0B） */
} can_frame_t;


/* ============================================================================
 * API
 * ==========================================================================*/

/**
  * @brief  初始化 CAN1：GPIO + CAN 参数 + 过滤器 + 中断
  *
  * 过滤器配置：只接收两个 ID 的帧
  *   - OTA_CAN_ID_HOST_TX (0x18F00001) —— 上位机发的 OTA 帧
  *   - OTA_CAN_ID_APP_DATA (0x0CF00001) —— App 业务数据（Bootloader 忽略但要放行）
  *
  * 波特率 500kbps，默认 1+12+7 T=20T，APB1=42MHz（APB1 分频后），
  *   Prescaler + TBS1 + TBS2 = 42M / 500k = 84
  *   我们设 Prescaler=6, BS1=11, BS2=3 → 6*(1+11+3) = 90... 不对
  *   实际应该 APB1 时钟是 42MHz / 2 = 21MHz（CAN 时钟源）
  *   Prescaler + BS1 + BS2 = 21M / 500k = 42
  *   Prescaler=2, BS1=25, BS2=16 → 2*(1+25+16) = 84... 还是不对
  *
  *   正确计算（STM32F4 默认 APB1 分频后 42MHz）：
  *   CAN 内核时钟 = APB1 = 42MHz
  *   TQ = Prescaler
  *   每帧 bit = 1 + BS1 + BS2
  *   Baudrate = 42MHz / (Prescaler * (1 + BS1 + BS2)) = 500kbps
  *   → Prescaler * (1 + BS1 + BS2) = 84
  *   → 选 Prescaler = 6, BS1 = 10, BS2 = 3 → 6*14 = 84 ✓
  */
HAL_StatusTypeDef can_boot_init(void);

/**
  * @brief  发送一帧 CAN 数据
  * @param  frame  要发送的帧（函数内部会把数据 copy 走，调用者可以立即释放 frame）
  * @return HAL_OK 成功，其他失败（比如没空闲邮箱）
  *
  * 发送策略：
  *   标准 CAN 有 3 个发送邮箱（MailBox 0/1/2），都空的话随便选一个。
  *   如果 3 个都被占满，就等一会儿再试（Bootloader 单次发送量不大，不会拥塞）。
  */
HAL_StatusTypeDef can_boot_send(const can_frame_t *frame);

/**
  * @brief  从环形缓冲区取一帧（主循环调用）
  * @param  frame_out  输出参数，收到的帧 copy 到这里
  * @return true 有帧可取，false 缓冲区空
  *
  * 这个函数是线程安全的——环形缓冲区用了 head/tail 指针，
  * 中断只写 head，主循环只读 tail，不需要额外互斥（因为只有一个生产者一个消费者）。
  */
bool can_boot_receive(can_frame_t *frame_out);

/**
  * @brief  CAN1 RX0 中断处理（写在 stm32f4xx_it.c 里的入口函数会调用这个）
  *         Bootloader 侧的中断回调
  */
void can_boot_rx0_irq_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __CAN_BOOT_H */

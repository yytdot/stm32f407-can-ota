/**
  ******************************************************************************
  * @file    ota_state_machine.h
  * @brief   Bootloader OTA 状态机头文件 —— 协议解析 + 状态流转
  *
  * 整个 Bootloader 的 OTA 逻辑就是一个大状态机。
  * 状态机的每个状态对应一类 CAN 协议交互，
  * 收到 CAN 帧后根据当前状态 + 帧内容决定下一个状态。
  *
  * 状态流转图（详见 ota_protocol.h 里的注释）：
  *
  *   OTA_STATE_IDLE ──(收到 CMD_HANDSHAKE)──► OTA_STATE_WAIT_HEADER
  *   OTA_STATE_WAIT_HEADER ──(收到完整固件头 + magic 正确)──► OTA_STATE_RECEIVING
  *   OTA_STATE_RECEIVING ──(全部数据收完 + 收到 CMD_VERIFY)──► OTA_STATE_VERIFY
  *   OTA_STATE_VERIFY ──(CRC32 校验通过)──► OTA_STATE_PROGRAM
  *   OTA_STATE_VERIFY ──(CRC32 校验失败)──► OTA_STATE_FAILED
  *   OTA_STATE_PROGRAM ──(搬运完成)──► OTA_STATE_DONE
  *   OTA_STATE_FAILED ──(自动)──► 清标记 + 跳转旧 App（如果有的话）
  *   OTA_STATE_DONE ──(自动)──► 写升级成功标记 + 跳转新 App
  *
  * 所有 CAN 帧解析统一走 ota_process_can_frame() 入口，
  * 内部根据 ota_state_t 分支处理，保证协议状态机不漂移。
  ******************************************************************************
  */

#ifndef __OTA_STATE_MACHINE_H
#define __OTA_STATE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../Common/ota_protocol.h"
#include "../../Common/flash_layout.h"
#include "../Drivers/can_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  OTA 状态机上下文
  *
  * 把所有 OTA 相关的全局变量集中在这里，避免散落在各处难以维护。
  * 状态机的每个函数都拿这个结构体的指针来读/写状态。
  */
typedef struct {
    ota_state_t state;         /**< 当前状态 */
    fw_header_t fw_header;     /**< 上位机发送过来的固件头（解析完存这里） */
    uint32_t    expected_seq;  /**< 下一个期望收到的数据包序号（用于排序检测） */
    uint32_t    total_written; /**< 已经写入 W25Q64 的固件字节数（用于进度显示 + 完整性检查） */
    uint32_t    header_rx_buf[FW_HEADER_SIZE / 4 + 1];   /**< 固件头接收缓冲（32bit 对齐） */
    uint8_t     header_rx_idx; /**< 固件头已经接收的字节数 */
} ota_ctx_t;

/**
  * @brief  初始化 OTA 状态机（进入 IDLE 状态，所有计数清零）
  */
void ota_state_machine_init(ota_ctx_t *ctx);

/**
  * @brief  主循环调用：从 CAN 环形缓冲区取帧，喂给状态机处理
  *
  * 这是 Bootloader main loop 的核心调用。
  * 循环体就是：
  *   while (1) {
  *       ota_state_machine_step(&ctx);
  *       led 更新;
  *   }
  */
void ota_state_machine_step(ota_ctx_t *ctx);

/**
  * @brief  从 W25Q64 参数区读升级标记
  *
  * 封装了 W25Q64 读 + 偏移计算，Bootloader main 用它判断要不要进升级模式。
  */
uint16_t ota_read_upgrade_flag(void);

/**
  * @brief  写升级标记到 W25Q64 参数区
  * @param  flag  UPGRADE_FLAG_NONE / UPGRADE_FLAG_TRIGGER / UPGRADE_FLAG_SUCCESS
  */
void ota_write_upgrade_flag(uint16_t flag);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_STATE_MACHINE_H */

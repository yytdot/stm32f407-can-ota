/**
  ******************************************************************************
  * @file    app_upgrade_trigger.h
  * @brief   App 侧 OTA 升级触发逻辑
  *
  * 工作流程：
  *   1. CAN 任务收到上位机发来的 OTA 启动指令
  *   2. CAN 任务调 upgrade_trigger_request()
  *   3. 该函数写入 W25Q64 参数区的升级标记 = UPGRADE_FLAG_TRIGGER (0x55AA)
  *   4. 调用 NVIC_SystemReset() 软复位，进入 Bootloader
  *   5. Bootloader 上电读到升级标记，停留 Bootloader 模式等固件
  *
  * 为什么不直接"App 里自己收固件"？
  *   —— App 是 FreeRTOS 业务程序，还在跑 OLED、CAN 上传、传感器采集……
  *      如果让 App 同时做 OTA 接收 + 片内 Flash 擦写，
  *      两个任务会竞争片内 Flash 访问，而且 App 本身就在被擦写的区域里！
  *   —— 正确做法：App 只负责"触发"（写标记 + 软复位），
  *      Bootloader 负责"升级"（收固件 + 校验 + 搬运 + 跳转）。
  *   —— 职责分离，防砖。
  ******************************************************************************
  */

#ifndef __APP_UPGRADE_TRIGGER_H
#define __APP_UPGRADE_TRIGGER_H

#include "stm32f4xx_hal.h"
#include "../../Common/flash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  触发 OTA 升级：写升级标记到 W25Q64 + 软复位
  *
  * 调用后永不返回（NVIC_SystemReset 导致 CPU 复位）。
  *
  * @param  flag_value  要写入的升级标记值（通常是 UPGRADE_FLAG_TRIGGER = 0x55AA）
  *
  * 为什么要传 flag_value 而不是写死 0x55AA？
  *   —— 未来可以扩展：比如 0x55BB 表示"强制升级"、0x55CC 表示"降版本"
  *   —— 预留扩展空间
  */
void upgrade_trigger_request(uint16_t flag_value);

#ifdef __cplusplus
}
#endif

#endif /* __APP_UPGRADE_TRIGGER_H */

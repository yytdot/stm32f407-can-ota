/**
  ******************************************************************************
  * @file    flash_internal.h
  * @brief   STM32F4 片内 Flash 擦写驱动 —— 关键函数标记在 RAM 中运行
  *
  * ⚠️ 为什么 Flash 擦写函数必须在 RAM 中跑？
  *
  * STM32 硬件限制：
  *   当你正在擦写片内 Flash 时，即使 CPU 能继续取指令，
  *   从同一 Flash 区域取指令会显著变慢（F4 手册说擦写期间
  *   I-Code bus 会阻塞，取一个指令要 6 个 HCLK 而不是 1 个）。
  *   更严重的是：如果你的 Bootloader 代码本身也在片内 Flash，
  *   而你又在擦写片内 Flash → 你擦的代码就是你正在执行的代码！
  *   擦到 Bootloader 自己的区域时，CPU 会因为取不到指令而锁死。
  *
  * 解决方案（STM32 Bootloader 标准做法）：
  *   1. 把 Flash 擦写相关函数用 __attribute__((section(".ram_code")))
  *      标记到 .ram_code 段
  *   2. 链接脚本把 .ram_code 段放到 SRAM（0x20000000 起始）
  *   3. 启动文件里加一行 memcpy，把 Flash 里的 .ram_code 内容
  *      拷贝到 SRAM 对应位置（类似 .data 段的处理）
  *   4. 这样调用擦写函数时，CPU 实际是在 SRAM 里取指令，
  *      不会和被擦写的片内 Flash 冲突
  *
  * 本文件的擦写函数都有 __attribute__((section(".ram_code"))) 标记，
  * 请确保你的链接脚本正确配置了 .ram_code 段！
  * ==========================================================================*/

#ifndef __FLASH_INTERNAL_H
#define __FLASH_INTERNAL_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "../../Common/flash_layout.h"   /* APP_START_ADDR / APP_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  擦除 App 区（片内 Flash 0x08020000 开始的 7 个 sector）
  *         函数体强制放在 RAM 中运行！
  *
  * @return HAL_OK 成功，其他失败
  *
  * ⚠️ 这个函数是 Bootloader 最危险的操作之一：
  *   - 一擦就是 896KB，耗时 ~2 秒（STM32F4 擦除 128KB sector 约 260ms/块）
  *   - 擦完 App 区后，设备上暂时没有可以运行的 App 了
  *   - 如果后续搬运固件失败（比如 W25Q64 读失败），设备就会卡在 Bootloader
  *     （但不会变砖，因为 Bootloader 永远在）
  *
  *   所以：只有在 CRC32 整体校验 100% 通过后才可以调用此函数！
  */
HAL_StatusTypeDef flash_erase_app(void) __attribute__((section(".ram_code")));

/**
  * @brief  向片内 Flash 的指定地址写入数据
  *         函数体强制放在 RAM 中运行！
  *
  * @param  addr   写入目标地址（必须是 half-word 对齐）
  * @param  data   源数据缓冲区
  * @param  len    写入长度（字节），内部会自动按 word 对齐补零
  * @return HAL_OK 成功，其他失败
  *
  * 写规则：
  *   - STM32F4 片内 Flash 写入粒度 = word (32bit = 4字节)
  *   - 目标地址必须 4 字节对齐（否则触发 HardFault）
  *   - 写入前必须 FLASH_Unlock()，写完必须 FLASH_Lock()
  *   - 只能把 1 写成 0（Flash 特性），所以写之前必须已经擦除过
  */
HAL_StatusTypeDef flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
    __attribute__((section(".ram_code")));

/**
  * @brief  从 W25Q64 搬运固件到片内 Flash App 区
  *         函数体强制放在 RAM 中运行！
  *
  * 内部会分块（FW_TRANSFER_CHUNK_SIZE = 4KB）循环：
  *   读 W25Q64 → 写片内 Flash → 读 W25Q64 → 写片内 Flash → ...
  *
  * @param  fw_size  实际要搬运的固件长度（来自 fw_header.fw_size）
  * @return HAL_OK 成功，其他失败
  *
  * 经验教训（来自过往项目）：
  *   ❌ 不要写死 APP_SIZE 作为拷贝长度 —— 固件头里的 fw_size 才是真实长度
  *   ✅ 拷贝长度 = min(fw_size, APP_SIZE) —— 先校验 fw_size <= APP_SIZE，这里直接用 fw_size
  */
HAL_StatusTypeDef flash_program_firmware(uint32_t fw_size)
    __attribute__((section(".ram_code")));

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_INTERNAL_H */

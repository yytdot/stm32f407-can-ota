/**
  ******************************************************************************
  * @file    flash_internal.c
  * @brief   STM32F4 片内 Flash 擦写实现 —— 所有函数在 RAM 中运行
  *
  * 本文件所有函数都有 __attribute__((section(".ram_code"))) 标记。
  * 这意味着：
  *   1. 编译后这部分代码会被放到 .ram_code 段
  *   2. 链接脚本 .sct/ld 文件必须把 .ram_code 段放到 SRAM 地址范围
  *   3. 启动文件 (startup_stm32f407xx.s) 里必须有一段 memcpy，
  *      把 Flash 里的 .ram_code 加载地址拷贝到 SRAM 里的运行地址
  *
  *   如果第 3 步缺失，CPU 会硬执行擦写函数的同时从 Flash 取指令，
  *   会卡在那里或 HardFault —— 这是 STM32 Bootloader 开发最常见的坑。
  ******************************************************************************
  */

#include "flash_internal.h"
#include "w25q64.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/*
 * 说明：
 *   STM32F407 的 Flash sector 分布是：
 *     Sector 0  : 0x08000000 - 0x08003FFF (16KB)
 *     Sector 1  : 0x08004000 - 0x08007FFF (16KB)
 *     Sector 2  : 0x08008000 - 0x0800BFFF (16KB)
 *     Sector 3  : 0x0800C000 - 0x0800FFFF (16KB)
 *     Sector 4  : 0x08010000 - 0x0801FFFF (64KB)
 *     Sector 5  : 0x08020000 - 0x0803FFFF (128KB)  ← App 从这里开始
 *     Sector 6  : 0x08040000 - 0x0805FFFF (128KB)
 *     Sector 7  : 0x08060000 - 0x0807FFFF (128KB)
 *     Sector 8  : 0x08080000 - 0x0809FFFF (128KB)
 *     Sector 9  : 0x080A0000 - 0x080BFFFF (128KB)
 *     Sector 10 : 0x080C0000 - 0x080DFFFF (128KB)
 *     Sector 11 : 0x080E0000 - 0x080FFFFF (128KB)
 *
 *   App 区占 Sector 5~11，共 7 个 sector，每个 128KB。
 *   用 FLASH_SECTOR_5 ~ FLASH_SECTOR_11 宏来标识。
 */

/**
  * @brief  擦除整个 App 区
  *
  * HAL 库提供 FLASH_Erase 函数，可以指定 sector 范围。
  * 我们循环擦 Sector 5 到 Sector 11。
  *
  * 注意：FLASH_Unlock() / FLASH_Lock() 必须成对出现。
  * 每次擦除操作前解锁，擦完锁回去。
  * STM32F4 的 Flash 寄存器默认为读保护锁定状态，
  * 不解锁直接写会触发 HardFault。
  */
HAL_StatusTypeDef flash_erase_app(void)
{
    HAL_StatusTypeDef status;

    /* 1. 解锁 Flash */
    HAL_FLASH_Unlock();

    /* 2. 配置擦除参数：Sector 5 到 Sector 11，全擦 */
    FLASH_EraseInitTypeDef erase_init = {0};
    erase_init.TypeErase   = FLASH_TYPEERASE_SECTORS;   /* 按 sector 擦 */
    erase_init.Sector      = FLASH_SECTOR_5;            /* 起始 sector */
    erase_init.NbSectors   = 7;                         /* Sector 5~11 共 7 个 */
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;    /* 3.3V~3.6V，VCC 正常 3.3V 选这个 */

    /* 3. 执行擦除 */
    uint32_t sector_err = 0;   /* 擦除出错的 sector 号，调试用 */
    status = HAL_FLASHEx_Erase(&erase_init, &sector_err);

    /* 4. 锁回 Flash */
    HAL_FLASH_Lock();

    return status;
}

/**
  * @brief  向片内 Flash 指定地址写入数据
  *
  * 原理：
  *   STM32F4 Flash 写入粒度 = 32bit word (4字节)
  *   调用 HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data)
  *   就可以写一个 word。多次调用就能写 len 字节。
  *
  * 地址必须 4 字节对齐，否则触发对齐错误 HardFault。
  * len 不是 4 的倍数时最后几字节按 0xFF 补齐写入（Flash 特性：只能把 1→0，
  * 补 0xFF 不会改变任何内容，因为擦除后本来就是 0xFF）。
  */
HAL_StatusTypeDef flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint32_t idx = 0;

    /* 地址必须 4 字节对齐，防御性检查 */
    if ((addr & 0x3) != 0) {
        return HAL_ERROR;
    }

    /* 1. 解锁 Flash */
    HAL_FLASH_Unlock();

    /* 2. 按 word (4字节) 循环写入 */
    while (idx < len) {
        /* 组装一个 32bit word（小端，STM32 是小端 CPU） */
        uint32_t word_data = 0xFFFFFFFF;   /* 默认全 0xFF（擦除后的值） */

        /* 填充 word_data 的 4 个字节 */
        for (int byte_in_word = 0; byte_in_word < 4 && idx < len; byte_in_word++) {
            word_data &= ~(0xFFu << (byte_in_word * 8));       /* 先把对应字节清 0 */
            word_data |= ((uint32_t)data[idx++] & 0xFFu) << (byte_in_word * 8);   /* 填入新值 */
        }

        /* 写入一个 word */
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word_data);
        if (status != HAL_OK) {
            break;   /* 写失败，立即退出 */
        }

        addr += 4;   /* 地址前进 4 字节 */
    }

    /* 3. 锁回 Flash */
    HAL_FLASH_Lock();

    return status;
}

/**
  * @brief  从 W25Q64 搬运固件到片内 Flash App 区
  *
  * 分块搬运策略（每块 FW_TRANSFER_CHUNK_SIZE = 4KB）：
  *   1. 从 W25Q64_FW_CACHE_OFFSET + offset 读一个 chunk 到 RAM buffer
  *   2. 把这个 chunk 写入 APP_START_ADDR + offset（片内 Flash）
  *   3. 循环，直到 fw_size 字节全部搬完
  *
  * ⚠️ RAM buffer 大小 = FW_TRANSFER_CHUNK_SIZE = 4KB。
  *   Bootloader 的 RAM 有 128KB，完全够用。
  *   经验教训：不要把 buffer 做成全局变量——全局变量在 .bss 段，
  *   如果 .bss 段碰巧和 .ram_code 段重叠，会互相覆盖。
  *   用局部变量 + static 让它在 .bss 段单独分配，就没问题了。
  */
HAL_StatusTypeDef flash_program_firmware(uint32_t fw_size)
{
    HAL_StatusTypeDef status = HAL_OK;

    /* 分块 buffer，4KB。static 放在 .bss，不会和 .ram_code 冲突 */
    static uint8_t chunk_buf[FW_TRANSFER_CHUNK_SIZE];

    uint32_t total_written = 0;

    while (total_written < fw_size) {
        /* 计算本轮要处理的字节数：min(剩余字节数, chunk 大小) */
        uint32_t chunk_len = (fw_size - total_written < FW_TRANSFER_CHUNK_SIZE)
                             ? (fw_size - total_written)
                             : FW_TRANSFER_CHUNK_SIZE;

        /* 1. 从 W25Q64 读 */
        w25q64_read(W25Q64_FW_CACHE_OFFSET + total_written,
                    chunk_buf, chunk_len);

        /* 2. 写入片内 Flash */
        status = flash_write(APP_START_ADDR + total_written,
                             chunk_buf, chunk_len);
        if (status != HAL_OK) {
            break;
        }

        total_written += chunk_len;
    }

    return status;
}

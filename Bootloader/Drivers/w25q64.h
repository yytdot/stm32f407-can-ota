/**
  ******************************************************************************
  * @file    w25q64.h
  * @brief   W25Q64 SPI Flash 驱动头文件
  *
  * W25Q64 是 8MB SPI Flash，和 STM32F4 的 SPI1 连接。
  *
  * 硬件连线（默认 SPI1）：
  *   PA5  - SCK
  *   PA6  - MISO
  *   PA7  - MOSI
  *   PA4  - CS (片选)
  *
  * SPI 配置：
  *   - 模式 0（CPOL=0, CPHA=0），W25Q64 只支持模式 0 和 3，选 0 最简单
  *   - 时钟频率 <= 50MHz（W25Q64 支持，STM32F4 SPI1 最大 50MHz）
  *   - 数据位宽 8 bit
  ******************************************************************************
  */

#ifndef __W25Q64_H
#define __W25Q64_H

#include <stdint.h>
#include <stddef.h>
#include "../../Common/flash_layout.h"   /* 包含 W25Q64 分区大小定义 */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * W25Q64 内部指令集（SPI Flash 标准指令）
 * ==========================================================================*/

#define W25Q_CMD_WRITE_ENABLE    0x06   /**< 写使能：擦写前必须发 */
#define W25Q_CMD_WRITE_DISABLE   0x04   /**< 写禁止 */
#define W25Q_CMD_READ_STATUS1    0x05   /**< 读状态寄存器 1（bit0=忙标志） */
#define W25Q_CMD_READ_STATUS2    0x35   /**< 读状态寄存器 2 */
#define W25Q_CMD_READ_JEDIC_ID   0x9F   /**< 读 JEDEC ID（厂商+型号+容量） */
#define W25Q_CMD_FAST_READ       0x0B   /**< 快速读（比普通读快，加 dummy 字节） */
#define W25Q_CMD_PAGE_PROGRAM    0x02   /**< 页编程（写入），一页 256 字节 */
#define W25Q_CMD_SECTOR_ERASE    0x20   /**< 扇区擦除，4KB，擦除后全 0xFF */
#define W25Q_CMD_CHIP_ERASE      0xC7   /**< 整片擦除（8MB，很慢，一般不用） */

/**
  * @brief  W25Q64 状态寄存器 1 的"忙"标志位
  *
  * 硬件机制：
  *   - W25Q64 正在擦除/写入时，bit0 自动置 1
  *   - 操作完成后自动清 0
  *   - 外部 SPI 主机必须轮询这个 bit，直到它变 0，
  *     才能发下一个擦/写命令
  */
#define W25Q_STATUS_BUSY_FLAG    0x01   /* bit0 */

/**
  * @brief  预期的 W25Q64 JEDEC ID
  *
  * 读 JEDEC ID 返回 3 字节：厂商ID + 型号 + 容量编码
  *   Byte1: 0xEF = Winbond 厂商
  *   Byte2: 0x40 = W25Q 系列
  *   Byte3: 0x17 = 8MB (2^17 字节 = 8MB)
  *
  * 驱动初始化时会读这个值做校验，
  * 如果读出来的不是这个，说明硬件没焊对/SPI 连线有问题。
  */
#define W25Q_JEDIC_ID_EXPECTED   0xEF4017UL


/* ============================================================================
 * API 函数
 * ==========================================================================*/

/**
  * @brief  初始化 W25Q64（SPI1 + GPIO 配置）
  *         内部会读 JEDEC ID 验证芯片是否正常，
  *         如果读不到正确 ID，返回 0 表示失败。
  */
uint8_t w25q64_init(void);

/**
  * @brief  从 W25Q64 指定地址读取数据（自动按 sector/page 跨边界连续读）
  * @param  addr   起始地址（SPI Flash 内部地址，0~0x7FFFFF）
  * @param  buf    读取数据存放缓冲区
  * @param  len    要读取的字节数
  */
void w25q64_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
  * @brief  向 W25Q64 指定地址写入数据（自动处理 sector 擦除 + page 对齐写入）
  *
  * ⚠️ 注意！W25Q64 擦除粒度是 4KB sector，不能只擦一个 page！
  *   如果要更新 sector 中的部分数据，必须：
  *   1. 先把整个 sector 读出来放 RAM buffer
  *   2. 修改要更新的那几个 page
  *   3. 擦除整个 sector
  *   4. 把修改后的 sector 写回去
  *
  *   本驱动的 w25q64_write 函数内部已经帮你做了这个"读-改-擦-写"逻辑，
  *   所以调用者只需要给地址+长度+数据，不用管底层细节。
  *
  *   经验教训：如果上层代码直接调用 sector_erase + page_program，
  *   又没做"读-改-擦-写"，就会把 sector 里其他 page 的数据一起擦没。
  */
uint8_t w25q64_write(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
  * @brief  擦除一个 sector（4KB）
  * @param  addr   要擦除的 sector 起始地址（必须 4KB 对齐！）
  * @return 0 成功，1 地址未对齐或忙
  */
uint8_t w25q64_erase_sector(uint32_t addr);

/**
  * @brief  擦除固件缓存区的全部内容（OTA 升级前先清空）
  *         内部循环擦除 W25Q64_FW_CACHE_SIZE / W25Q64_SECTOR_SIZE 个 sector
  * @return 0 成功，1 中途出错
  */
uint8_t w25q64_erase_fw_cache(void);

/**
  * @brief  读 JEDEC ID，调试用
  * @return 32bit 值（高 24bit = JEDEC ID，低 8bit = 0）
  */
uint32_t w25q64_read_jedec_id(void);

#ifdef __cplusplus
}
#endif

#endif /* __W25Q64_H */

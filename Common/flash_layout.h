/**
  ******************************************************************************
  * @file    flash_layout.h
  * @brief   整个项目的 Flash 地址规划 —— Bootloader 和 App 共用此文件
  *
  * 本文件是 Bootloader 工程和 App 工程的"宪法"。
  * 所有涉及地址偏移、分区大小、魔数判断的地方，都必须引用这里的宏，
  * 绝对禁止在其他 .c/.h 里硬编码 0x08020000 这种数字。
  *
  * 设计原则：
  *   1. 片内 Flash 只放 Bootloader(128KB) 和 App(896KB)，不做固件备份
  *   2. W25Q64(8MB SPI Flash) 做固件缓存 + 参数存储
  *   3. App 收到新固件后，先完整写入 W25Q64 缓存区
  *      —— 全程校验通过，才去擦写片内 Flash
  *      —— 接收中途断电/丢包/校验失败，片内 App 完全不动，直接回滚
  *   4. 所有地址偏移都用 uint32_t，和 STM32 指针宽度对齐
  ******************************************************************************
  */

#ifndef __FLASH_LAYOUT_H
#define __FLASH_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 第一部分：STM32F407ZGT6 片内 Flash 规划
 * ============================================================================
 *
 * STM32F407 片内 Flash 总容量 = 1MB (0x00100000 字节)
 * 起始地址 = 0x08000000
 *
 * 划分策略：
 *   ┌───────────────────────────────────────┐ 0x08000000
 *   │          Bootloader 区 (128KB)         │
 *   │          裸机，只做引导 + OTA 接收       │
 *   ├───────────────────────────────────────┤ 0x08020000  ← App 起始
 *   │                                       │
 *   │          App 运行区 (896KB)            │
 *   │          FreeRTOS 业务程序              │
 *   │          传感器 + OLED + CAN 上传        │
 *   │                                       │
 *   └───────────────────────────────────────┘ 0x080FFFFFF
 *
 * 为什么 Bootloader 是 128KB？
 *   —— 纯引导逻辑 + CAN OTA 协议栈 + W25Q64 驱动 + CRC32，
 *      编译后远小于此数，留足余量。
 *   —— 选 0x20000 边界 (128KB 对齐)，方便片内 Flash 擦除操作。
 *
 * 为什么 App 不再留备份区？
 *   —— 备份职责交给 W25Q64。W25Q64 有 8MB 空间，远大于 App 的 896KB，
 *      可以把新固件完整写进去再擦写片内 App。
 *   —— 好处：片内 Flash 写入次数少，寿命更长；结构更简单；防砖。
 * ==========================================================================*/

/** @brief 片内 Flash 起始地址（STM32F4 固定） */
#define INTERNAL_FLASH_BASE_ADDR     0x08000000UL

/** @brief Bootloader 区起始地址 —— 就是片内 Flash 的起点 */
#define BOOTLOADER_START_ADDR        (INTERNAL_FLASH_BASE_ADDR + 0x00000000UL)

/** @brief Bootloader 区大小 —— 128KB (0x20000) */
#define BOOTLOADER_SIZE              (128 * 1024UL)

/**
  * @brief  App 区起始地址
  *         = Bootloader 起始 + Bootloader 大小
  *         = 0x08020000
  *
  *         ⚠️ 这是整个项目最关键的地址常量，以下模块都会用到：
  *           - Bootloader 跳转 App 前，把向量表重定向到这里
  *           - App 启动时，SCB->VTOR 设置到这里
  *           - Bootloader 升级时，把 W25Q64 的固件搬运到这里
  *           - 链接脚本必须把 .text 段放在此地址
  */
#define APP_START_ADDR               (INTERNAL_FLASH_BASE_ADDR + BOOTLOADER_SIZE)

/**
  * @brief  App 区可用大小
  *         = 片内 Flash 总容量 1MB - Bootloader 区 128KB
  *         = 896KB (0xE0000)
  *
  *         ⚠️ OTA 固件不能超过这个大小！Bootloader 校验时会检查。
  *         实际 App 编译出来大概在 100KB~300KB 之间，留了足够余量。
  */
#define APP_SIZE                     (1024 * 1024UL - BOOTLOADER_SIZE)

/** @brief 片内 Flash 每页大小 —— STM32F407ZGT6 是 128KB per sector */
#define FLASH_SECTOR_SIZE            (128 * 1024UL)

/**
  * @brief  App 区跨越多少个 sector？
  *         App 从 0x08020000 开始，到 0x080FFFFFF 结束
  *         0x08020000 是 sector 5 的起始（STM32F407 sector 0~3 = 16KB*4,
  *         sector 4 = 64KB, sector 5+ = 128KB）
  *         实际使用 sector 5 ~ sector 11，共 7 个 sector
  *         这里不直接写死数值，用 APP_SIZE / FLASH_SECTOR_SIZE 算
  */
#define APP_SECTOR_COUNT             (APP_SIZE / FLASH_SECTOR_SIZE)


/* ============================================================================
 * 第二部分：W25Q64 SPI Flash 规划
 * ============================================================================
 *
 * W25Q64 容量 = 8MB (0x800000 字节)
 * 起始地址 = 0 (SPI Flash 没有固定内存映射地址，靠 SPI 命令 + 24bit 地址访问)
 *
 * 划分策略：
 *   ┌───────────────────────────────────────┐ 0x000000
 *   │                                       │
 *   │          固件缓存区 (2MB)              │
 *   │          接收 OTA 新固件先存这里        │
 *   │                                       │
 *   ├───────────────────────────────────────┤ 0x200000
 *   │                                       │
 *   │          参数存储区 (4KB)              │
 *   │          升级标记 / 版本号 / 设备ID /   │
 *   │          传感器校准参数                │
 *   │                                       │
 *   └───────────────────────────────────────┘ 0x201000
 *   （剩下的空间留作未来扩展）
 *
 * 为什么固件缓存区是 2MB？
 *   —— App 区只有 896KB，但给 W25Q64 的缓存区多留一些，
 *      方便将来 App 功能扩展变大时不用改地址规划。
 *   —— 2MB 边界对齐，擦写按 sector (4KB) 操作更方便。
 * ==========================================================================*/

/** @brief W25Q64 固件缓存区起始偏移 */
#define W25Q64_FW_CACHE_OFFSET       0x000000UL

/** @brief W25Q64 固件缓存区大小 —— 2MB */
#define W25Q64_FW_CACHE_SIZE         (2 * 1024 * 1024UL)

/** @brief W25Q64 参数存储区起始偏移 —— 跟在缓存区后面 */
#define W25Q64_PARAM_OFFSET          (W25Q64_FW_CACHE_OFFSET + W25Q64_FW_CACHE_SIZE)

/** @brief W25Q64 参数存储区大小 —— 4KB，一个完整的 sector，擦除方便 */
#define W25Q64_PARAM_SIZE            (4 * 1024UL)


/* ============================================================================
 * 第三部分：参数存储区内部结构（按字节偏移）
 * ============================================================================
 *
 * W25Q64_PARAM_OFFSET 起始的 4KB sector 里，内容布局如下：
 *
 *   偏移    大小    字段名              含义
 *   ─────────────────────────────────────────────────────
 *   0x00    2字节   UPGRADE_FLAG        升级触发标记（魔数）
 *   0x02    4字节   FW_VERSION          当前运行固件版本号（整数）
 *   0x06    8字节   DEVICE_ID           设备唯一 ID
 *   0x0E    2字节   SHT30_TEMP_OFFSET   SHT30 温度校准偏移
 *   0x10    2字节   SHT30_HUM_OFFSET    SHT30 湿度校准偏移
 *   0x12    2字节   BH1750_GAIN         BH1750 增益校准
 *   0x14    4字节   VOLTAGE_DIV_RATIO   电压分压电阻比
 *   ...     ...     ...                 （预留空间，直到 4KB）
 *
 * 为什么每个偏移都显式定义？
 *   —— W25Q64 擦除按 sector (4KB) 操作，但写入按 page (256B) 操作。
 *      把每个字段的绝对偏移写死，读写时可以直接 SPI 命令 + 地址，
 *      不需要额外做结构体内存对齐处理，跨平台也不会出错。
 * ==========================================================================*/

/**
  * @brief  升级触发标记
  *
  * 含义：
  *   - 0x0000：无升级需求，Bootloader 正常跳转 App
  *   - 0x55AA：App 收到上位机 OTA 指令后写入此值，
  *             然后软复位。Bootloader 上电读到 0x55AA，
  *             就知道"我需要停留在 Bootloader 模式等固件"。
  *   - 0xAA55：升级完成后 Bootloader 写入此值，
  *             表示"升级刚完成"，下次启动 Bootloader 读到后
  *             也会清掉并跳转 App（不是故障标记，是状态标记）。
  *
  * 为什么用 0x55AA？
  *   —— 0x55 = 01010101，0xAA = 10101010，是经典的"反码标记"。
  *   —— 随机数据撞到这个值的概率极低（1/65536），
  *      而且 W25Q64 刚出厂是全 0xFF，擦除后也是全 0xFF，
  *      不会和 0x55AA 或 0xAA55 混淆。
  */
#define PARAM_UPGRADE_FLAG_OFFSET    0x0000
#define UPGRADE_FLAG_NONE            0x0000
#define UPGRADE_FLAG_TRIGGER         0x55AA
#define UPGRADE_FLAG_SUCCESS         0xAA55

/** @brief 固件版本号偏移（uint32_t 整数，自增） */
#define PARAM_FW_VERSION_OFFSET      0x0002

/** @brief 设备唯一 ID 偏移（8 字节，出厂烧录一次，不改） */
#define PARAM_DEVICE_ID_OFFSET       0x0006

/** @brief SHT30 温度校准偏移（int16_t，单位 0.1°C） */
#define PARAM_SHT30_TEMP_OFFSET      0x000E
/** @brief SHT30 湿度校准偏移（int16_t，单位 0.1%） */
#define PARAM_SHT30_HUM_OFFSET       0x0010
/** @brief BH1750 增益校准（uint16_t） */
#define PARAM_BH1750_GAIN            0x0012
/** @brief 电压分压电阻比（uint32_t，实际 = R上+R下 / R下，整数放大100倍存） */
#define PARAM_VOLTAGE_DIV_RATIO      0x0014


/* ============================================================================
 * 第四部分：固件头（Firmware Header）结构
 * ============================================================================
 *
 * 每一个发布的固件，开头都有这么一段固定长度的 header：
 *
 *   偏移    大小    字段名         含义
 *   ────────────────────────────────────────────
 *   0x00    4字节   magic          魔数 0x4F544131 = "OTA1"，校验有效性
 *   0x04    4字节   fw_size        固件总长度（不含 header 本身），单位字节
 *   0x08    4字节   fw_version     固件版本号
 *   0x0C    4字节   crc32          整个固件（不含 header）的 CRC32 校验值
 *   0x10    4字节   reserved       预留，未来扩展签名/加密
 *
 * 总 header 长度 = 0x14 = 20 字节
 *
 * Bootloader OTA 升级时，上位机在发送固件之前先发送这个 header，
 * Bootloader 拿到后：
 *   1. 检查 magic 是否是 "OTA1" → 拒绝非法固件
 *   2. 检查 fw_size 是否 <= APP_SIZE → 拒绝过大固件
 *   3. 把 crc32 存起来，全部收完后做整体校验
 *
 * ⚠️ 这个 header 不属于 CAN 协议层（协议层是分包），
 *    它是固件本身的格式定义，和 CAN 传输层解耦。
 * ==========================================================================*/

/** @brief 固件魔数：ASCII "OTA1" = 0x41 0x54 0x4F 0x31 → 按小端读就是 0x314F5441
 * 但这里我们存的是"网络字节序"即大端，所以直接拼 0x4F544131
 */
#define FW_HEADER_MAGIC              0x4F544131UL

/** @brief 固件头总长度（字节）—— Bootloader 升级时先收这 20 字节 */
#define FW_HEADER_SIZE               20UL

/**
  * @brief  固件头结构体（Bootloader 和 App 共用）
  *
  * 用 __attribute__((packed)) 强制紧凑排列，避免编译器插入结构体成员间隙。
  * 因为这个结构体的布局必须和上位机打包固件时一致，
  * 任何 padding 都会导致偏移错位、magic 读错、crc32 全错。
  */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /**< 魔数 FW_HEADER_MAGIC = 0x4F544131，标识固件有效性 */
    uint32_t fw_size;     /**< 固件正文长度（不含本 header），单位字节 */
    uint32_t fw_version;  /**< 固件版本号，自增整数 */
    uint32_t crc32;       /**< 固件正文整体 CRC32 校验值（多项式 0x04C11DB7） */
    uint32_t reserved;    /**< 预留，填 0，未来可加签名/加密 */
} fw_header_t;


/* ============================================================================
 * 第五部分：Flash 擦写相关常量
 * ==========================================================================*/

/**
  * @brief  W25Q64 每页大小 —— SPI Flash 写入单位
  *         W25Q64 内部按 page (256B) 组织，写入必须按 page 对齐
  */
#define W25Q64_PAGE_SIZE             256UL

/**
  * @brief  W25Q64 每个 sector 大小 —— SPI Flash 擦除单位
  *         擦除是 4KB 一个 sector，擦除后 sector 内全部变 0xFF
  */
#define W25Q64_SECTOR_SIZE           (4 * 1024UL)

/**
  * @brief  Bootloader 从 W25Q64 搬运固件到片内 Flash 时的分块大小
  *
  * 为什么是 4KB？
  *   —— 片内 Flash 写入按 half-word (2B) 或 word (4B) 操作，
  *      但擦除按 sector (128KB)，如果每次只搬几个字节效率极低。
  *   —— 4KB 和 W25Q64 的 sector 大小一致，
  *      读取时一次拿 4KB，写入时一次循环 4KB，效率高。
  *   —— 同时 4KB 也远小于栈空间，不会爆栈。
  *
  * 经验教训（来自过往项目）：
  *   不要写死 APP_SIZE 作为拷贝长度！
  *   应该用 fw_header.fw_size（实际固件长度），取 min(fw_size, APP_SIZE)。
  *   原因：固件头里的 fw_size 才是真实长度，APP_SIZE 只是上限。
  */
#define FW_TRANSFER_CHUNK_SIZE       4096UL


/* ============================================================================
 * 第六部分：RAM 中运行 Flash 擦写函数的链接段名
 * ==========================================================================*/

/**
  * @brief  把 Flash 擦写函数放到 RAM 里运行的链接段名
  *
  * STM32 有个硬件限制：当你正在擦写片内 Flash 时，
  * CPU 无法从片内 Flash 取指令（会阻塞）。
  * 所以 Bootloader 里的 Flash 擦写函数必须跑在 RAM 中。
  *
  * 使用方法：
  *   在 .c 文件中把擦写函数标注 __attribute__((section(".ram_code")))，
  *   链接脚本里把 .ram_code 段放到 RAM 的 0x20000000 区域。
  *   启动代码会自动把 .ram_code 段的内容从 Flash 拷贝到 RAM。
  *
  * 这是 STM32 Bootloader 开发的"必修课"。
  */
#define RAM_CODE_SECTION             ".ram_code"


#ifdef __cplusplus
}
#endif

#endif /* __FLASH_LAYOUT_H */

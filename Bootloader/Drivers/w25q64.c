/**
  ******************************************************************************
  * @file    w25q64.c
  * @brief   W25Q64 SPI Flash 驱动实现
  *
  * 使用 STM32 HAL 库的 SPI 接口。
  * SPI1 配置：
  *   - 模式 0 (CPOL=0, CPHA=0)
  *   - 8 bit 数据帧
  *   - 50 MHz 时钟（W25Q64 支持）
  ******************************************************************************
  */

#include "w25q64.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* ============================================================================
 * 本文件内部使用的 SPI 句柄 + CS GPIO 定义
 * ==========================================================================*/

/* SPI1 句柄 —— 在初始化里配置好，供所有 SPI 操作复用 */
/* SPI1 句柄：定义在 main.c（CubeMX 生成） */
extern SPI_HandleTypeDef hspi1;

/* CS 引脚定义 —— SPI Flash 片选，低电平有效 */
#define W25Q_CS_PORT    GPIOA
#define W25Q_CS_PIN     GPIO_PIN_4

/* CS 拉高/拉低宏 —— 每次 SPI 通信前必须 CS 拉低，通信完拉高 */
#define W25Q_CS_LOW()   HAL_GPIO_WritePin(W25Q_CS_PORT, W25Q_CS_PIN, GPIO_PIN_RESET)
#define W25Q_CS_HIGH()  HAL_GPIO_WritePin(W25Q_CS_PORT, W25Q_CS_PIN, GPIO_PIN_SET)


/* ============================================================================
 * 内部辅助函数
 * ==========================================================================*/

/**
  * @brief  等待 W25Q64 内部操作（擦除/写入）完成
  *
  * 原理：
  *   - 发 READ_STATUS1 (0x05) 指令，读回状态寄存器 1
  *   - bit0 (BUSY) 为 1 表示正在忙，需要继续等
  *   - 直到读回 0x00 才说明空闲，可以继续发下一条命令
  *
  * ⚠️ 这个函数被所有擦除/写入操作调用，是驱动的基础。
  *   如果忘了调用，下一个操作会失败，而且不会有任何错误提示。
  */
static void w25q64_wait_idle(void)
{
    uint8_t status;
    do {
        W25Q_CS_LOW();
        HAL_SPI_Transmit(&hspi1, (uint8_t*)W25Q_CMD_READ_STATUS1, 1, HAL_MAX_DELAY);
        HAL_SPI_Receive(&hspi1, &status, 1, HAL_MAX_DELAY);
        W25Q_CS_HIGH();
    } while (status & W25Q_STATUS_BUSY_FLAG);
}

/**
  * @brief  发送 W25Q64 内部地址（24 bit，SPI Flash 地址空间 0~0x7FFFFF）
  * @param  addr  24bit 地址（高 8bit 必须为 0）
  *
  * W25Q64 地址格式：MSB 优先发送
  *   Byte1 = addr[23:16]
  *   Byte2 = addr[15:8]
  *   Byte3 = addr[7:0]
  */
static void w25q64_send_addr(uint32_t addr)
{
    uint8_t addr_bytes[3] = {
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8)  & 0xFF),
        (uint8_t)( addr        & 0xFF)
    };
    HAL_SPI_Transmit(&hspi1, addr_bytes, 3, HAL_MAX_DELAY);
}

/**
  * @brief  发送写使能指令 —— 每次擦/写前必须调用！
  *
  * 硬件保护：
  *   W25Q64 默认处于"写保护"状态，任何擦除/写入命令都会被忽略。
  *   必须先发 W25Q_CMD_WRITE_ENABLE (0x06)，芯片才允许擦写。
  *   而且这个"写使能"是一次性的——每次擦写成功后自动失效，
  *   所以下次擦写前还要再发一次。
  */
static void w25q64_write_enable(void)
{
    W25Q_CS_LOW();
    HAL_SPI_Transmit(&hspi1, (uint8_t*)W25Q_CMD_WRITE_ENABLE, 1, HAL_MAX_DELAY);
    W25Q_CS_HIGH();
}


/* ============================================================================
 * 对外 API 实现
 * ==========================================================================*/

/**
  * @brief  初始化 W25Q64：配置 GPIO + SPI1 + 读 JEDEC ID 验证
  *
  * @return 1 初始化成功（JEDEC ID 匹配），0 失败
  *
  * 失败原因可能是：
  *   - SPI1 没正确配置
  *   - CS 引脚没焊好
  *   - W25Q64 芯片没焊或者焊反了
  *   - 接线时序问题
  */
uint8_t w25q64_init(void)
{
    /* ---- 1. 使能 SPI1 和 GPIOA 时钟 ---- */
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* ---- 2. 配置 SPI1 引脚 ----
     * PA4 = CS (GPIO 输出，推挽)
     * PA5 = SCK (AF5)
     * PA6 = MISO (AF5)
     * PA7 = MOSI (AF5)
     */
    GPIO_InitTypeDef gpio_init = {0};

    /* CS 引脚：推挽输出，高速 */
    gpio_init.Pin   = W25Q_CS_PIN;
    gpio_init.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull  = GPIO_PULLUP;   /* 上拉，空闲时 CS 高电平（不选中芯片） */
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(W25Q_CS_PORT, &gpio_init);
    W25Q_CS_HIGH();   /* 初始不选中 */

    /* SCK / MISO / MOSI：AF5 复用 */
    gpio_init.Pin   = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    gpio_init.Mode  = GPIO_MODE_AF_PP;
    gpio_init.Pull  = GPIO_PULLUP;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio_init);

    /* ---- 3. SPI1 参数 ----
     * 已由 CubeMX 的 MX_SPI1_Init() + HAL_SPI_MspInit() 完成
     * （PA5=SCK, PA6=MISO, PA7=MOSI, AF5 复用；CS 用软件 GPIO 控制）。
     * 这里只保留 CS 引脚的 GPIO 配置，因为它不在 SPI 复用引脚里。
     */

    /* ---- 4. 验证 JEDEC ID ---- */
    uint32_t jedic = w25q64_read_jedec_id();
    if (jedic != W25Q_JEDIC_ID_EXPECTED) {
        return 0;   /* 硬件异常 */
    }

    return 1;   /* OK */
}

/**
  * @brief  读 JEDEC ID
  * @return 高 24bit = JEDEC ID，低 8bit = 0
  */
uint32_t w25q64_read_jedec_id(void)
{
    uint8_t id_bytes[3] = {0};

    W25Q_CS_LOW();
    HAL_SPI_Transmit(&hspi1, (uint8_t*)W25Q_CMD_READ_JEDIC_ID, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(&hspi1, id_bytes, 3, HAL_MAX_DELAY);
    W25Q_CS_HIGH();

    /* 拼接 3 字节为 32bit 值：[EF][40][17][00] */
    return ((uint32_t)id_bytes[0] << 24) |
           ((uint32_t)id_bytes[1] << 16) |
           ((uint32_t)id_bytes[2] << 8);
}

/**
  * @brief  从 W25Q64 读取数据（支持跨 sector / page 连续读取）
  *
  * SPI Flash 读操作非常简单：发指令 + 24bit 地址 + dummy 字节 + 连续接收。
  * W25Q64 内部有地址自动递增机制，读完一个 page/sector 会自动跳到下一个，
  * 所以一次 HAL_SPI_Receive 就能把 len 字节全读完，不用分段。
  *
  * @param  addr  起始地址（0~0x7FFFFF）
  * @param  buf   目标缓冲区（调用者保证空间足够）
  * @param  len   要读取的字节数
  */
void w25q64_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    /* 发 FAST_READ 指令 (0x0B)，比普通 READ (0x03) 更快 */
    W25Q_CS_LOW();
    HAL_SPI_Transmit(&hspi1, (uint8_t*)W25Q_CMD_FAST_READ, 1, HAL_MAX_DELAY);
    w25q64_send_addr(addr);
    HAL_SPI_Transmit(&hspi1, (uint8_t*)&(uint8_t){0x00}, 1, HAL_MAX_DELAY); /* dummy 字节 */
    HAL_SPI_Receive(&hspi1, buf, len, HAL_MAX_DELAY);
    W25Q_CS_HIGH();
}

/**
  * @brief  擦除一个 sector（4KB）
  *
  * @param  addr  目标 sector 起始地址（必须 4KB 对齐，否则擦错！）
  * @return 0 成功，1 参数错误
  *
  * W25Q64 擦除规则：
  *   - 擦除单位 = 一个 sector = 4KB
  *   - 擦除耗时典型值 ~80ms（等待 BUSY 变 0）
  *   - 擦除后 sector 内所有字节变 0xFF
  *   - 擦除前必须写使能（W25Q_CMD_WRITE_ENABLE）
  */
uint8_t w25q64_erase_sector(uint32_t addr)
{
    /* 地址未 4KB 对齐？ */
    if (addr % W25Q64_SECTOR_SIZE != 0) {
        return 1;
    }

    w25q64_write_enable();
    W25Q_CS_LOW();
    uint8_t cmd = W25Q_CMD_SECTOR_ERASE;
    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
    w25q64_send_addr(addr);
    W25Q_CS_HIGH();

    w25q64_wait_idle();

    return 0;
}

/**
  * @brief  擦除固件缓存区（Bootloader OTA 开始前先把缓存区清干净）
  *
  * 循环擦除 W25Q64_FW_CACHE_SIZE (2MB) / W25Q64_SECTOR_SIZE (4KB) = 512 个 sector。
  * 每个 sector 擦除 ~80ms，总共 512*80ms ≈ 41 秒。
  *
  * ⚠️ 这一步比较耗时，但只在 OTA 开始时做一次，没问题。
  */
uint8_t w25q64_erase_fw_cache(void)
{
    uint32_t sector_count = W25Q64_FW_CACHE_SIZE / W25Q64_SECTOR_SIZE;
    for (uint32_t i = 0; i < sector_count; i++) {
        uint32_t sector_addr = W25Q64_FW_CACHE_OFFSET + i * W25Q64_SECTOR_SIZE;
        if (w25q64_erase_sector(sector_addr) != 0) {
            return 1;   /* 中途某个 sector 擦除失败 */
        }
    }
    return 0;
}

/**
  * @brief  向 W25Q64 写入数据（自动处理 page 对齐 + sector 边界）
  *
  * W25Q64 写入限制：
  *   1. 必须 page (256B) 对齐，不对齐时自动处理
  *   2. 只能把 1 写成 0（不能把 0 写成 1），所以写之前必须擦除
  *      —— 这个函数内部会自动判断是否需要擦 sector
  *   3. 一个 sector 内如果已有 0，不能只更新其中一部分（必须先擦全 sector）
  *
  * 写入策略：
  *   - 如果要写入的 len 刚好覆盖若干完整 sector，直接擦 sector 再 page program
  *   - 如果只更新 sector 里的部分 page，先读整个 sector 到 RAM buffer，
  *     修改要写入的那部分，擦 sector，然后把修改后的 sector 写回去
  *
  * ⚠️ 这个函数里用到了 4KB 的 RAM buffer（SECTOR_BUFFER_SIZE），
  *   Bootloader 的 RAM 有 128KB，用 4KB 没问题。
  */
uint8_t w25q64_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    /* 写指针 —— 遍历 buf */
    uint32_t write_addr = addr;
    uint32_t remaining  = len;
    const uint8_t *src  = buf;

    /* sector buffer —— 处理"只更新 sector 内部分 page"的情况 */
    uint8_t sector_buf[W25Q64_SECTOR_SIZE];

    /* ---- 循环处理，直到 remaining == 0 ---- */
    while (remaining > 0) {
        /* 计算当前 sector 的起始地址 */
        uint32_t cur_sector_start = (write_addr / W25Q64_SECTOR_SIZE) * W25Q64_SECTOR_SIZE;

        /* 当前 sector 内，我们要从哪个 page 开始写、最多写多少 */
        uint32_t offset_in_sector = write_addr - cur_sector_start;

        /*
         * 检查当前位置是否是 sector 起点 + 要写的数据完整覆盖若干 sector。
         * 如果是 → 直接擦 + 写，最快路径。
         * 如果不是 → 读-改-擦-写，处理部分更新。
         */
        if (offset_in_sector == 0 && remaining >= W25Q64_SECTOR_SIZE) {
            /* 快路径：刚好从 sector 起点开始，且至少覆盖一个完整 sector */
            w25q64_erase_sector(cur_sector_start);

            /* 一次写完整 sector（4KB / 256B = 16 次 page program） */
            uint32_t bytes_to_write = W25Q64_SECTOR_SIZE;
            for (uint32_t page_idx = 0; page_idx < (bytes_to_write / W25Q64_PAGE_SIZE); page_idx++) {
                w25q64_write_enable();
                W25Q_CS_LOW();
                uint8_t cmd = W25Q_CMD_PAGE_PROGRAM;
                HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
                w25q64_send_addr(cur_sector_start + page_idx * W25Q64_PAGE_SIZE);
                HAL_SPI_Transmit(&hspi1, (uint8_t*)(src + page_idx * W25Q64_PAGE_SIZE),
                                  W25Q64_PAGE_SIZE, HAL_MAX_DELAY);
                W25Q_CS_HIGH();
                w25q64_wait_idle();
            }

            write_addr += W25Q64_SECTOR_SIZE;
            src        += W25Q64_SECTOR_SIZE;
            remaining  -= W25Q64_SECTOR_SIZE;
        } else {
            /* 慢路径：读-改-擦-写 */

            /* 1. 读整个 sector */
            w25q64_read(cur_sector_start, sector_buf, W25Q64_SECTOR_SIZE);

            /* 2. 把新数据拷入 sector_buf 的正确位置 */
            uint32_t bytes_to_copy = (remaining < (W25Q64_SECTOR_SIZE - offset_in_sector))
                                     ? remaining
                                     : (W25Q64_SECTOR_SIZE - offset_in_sector);
            memcpy(sector_buf + offset_in_sector, src, bytes_to_copy);

            /* 3. 擦 sector（变回全 0xFF） */
            w25q64_erase_sector(cur_sector_start);

            /* 4. 把修改后的 sector_buf 全写回去 */
            for (uint32_t page_idx = 0; page_idx < (W25Q64_SECTOR_SIZE / W25Q64_PAGE_SIZE); page_idx++) {
                w25q64_write_enable();
                W25Q_CS_LOW();
                uint8_t cmd = W25Q_CMD_PAGE_PROGRAM;
                HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
                w25q64_send_addr(cur_sector_start + page_idx * W25Q64_PAGE_SIZE);
                HAL_SPI_Transmit(&hspi1, sector_buf + page_idx * W25Q64_PAGE_SIZE,
                                  W25Q64_PAGE_SIZE, HAL_MAX_DELAY);
                W25Q_CS_HIGH();
                w25q64_wait_idle();
            }

            write_addr += bytes_to_copy;
            src        += bytes_to_copy;
            remaining  -= bytes_to_copy;
        }
    }

    return 0;
}

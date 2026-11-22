/**
  ******************************************************************************
  * @file    ssd1306.h
  * @brief   SSD1306 0.96 寸 I2C OLED 显示驱动头文件
  *
  * 硬件：
  *   SSD1306-0.96" OLED（128x64 分辨率，单色）
  *   I2C 地址：0x3C
  *   和传感器共 I2C1 总线（PB6/PB7），地址不同不会冲突
  *
  * 显示缓存设计：
  *   SSD1306 内部有一个 128x8 字节的显存（GDDRAM），
  *   分为 8 个 page（每 page 8 行高），128 列。
  *
  *   我们在 STM32 内部放一个和显存同样大小的"影子缓存"（128*8=1024字节），
  *   上层代码先在影子缓存里画好内容（文字/图形/点），
  *   然后一次 DMA/阻塞 I2C 把整个缓存写进 SSD1306。
  *   这样做比每次改一个像素就写一次 I2C 快得多。
  *
  *   为什么不用 SSD1306 的自动递增地址模式？
  *   —— 自动递增模式只适合从 SSD1306 当前显示内容"增量修改"，
  *      但我们每次刷新都要全量更新（温湿度 + 光照 + 电压 + 状态），
  *      直接 memcpy 整个缓存最干脆。
  ******************************************************************************
  */

#ifndef __SSD1306_H
#define __SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_I2C_ADDR            (0x3C << 1)
#define SSD1306_WIDTH               128
#define SSD1306_HEIGHT              64
#define SSD1306_NUM_PAGES           8    /* 64/8 */


/* ============================================================================
 * API
 * ==========================================================================*/

/** @brief 初始化 SSD1306：发初始化命令序列 + 全亮 */
HAL_StatusTypeDef ssd1306_init(void);

/** @brief 清屏（影子缓存 + SSD1306 显存全置 0） */
void ssd1306_clear(void);

/**
  * @brief  把影子缓存整体写进 SSD1306（刷新屏幕）
  *         App OLED 任务每隔一段时间调一次
  *
  * 内部实现：循环 8 个 page，每个 page 128 字节，
  *   发命令 [0xB0+page] [0x00] [0x10] 设置 page + column 起始，
  *   然后发 0x40 (数据模式标志) + 128 字节像素数据。
  *   总共 8 * (3 + 1 + 128) = 1056 字节 I2C 传输。
  */
void ssd1306_update_screen(void);

/**
  * @brief  在 (x,y) 位置画一个像素
  * @param  x     0~127
  * @param  y     0~63
  * @param  color true=点亮, false=熄灭
  *
  * y 坐标要换算成 page + page 内的 bit：
  *   page = y / 8
  *   bit  = y % 8
  */
void ssd1306_draw_pixel(uint8_t x, uint8_t y, bool color);

/**
  * @brief  在 SSD1306 上显示一段 ASCII 文本（8x16 字体）
  *
  * @param  x_start   起始列（0~127）
  * @param  page       起始 page（0~7）
  * @param  str        要显示的字符串（必须是 ASCII）
  *
  * 一个字符占 8 列 x 16 行 = 2 page
  * 一行最多显示 16 个字符（128 / 8）
  */
void ssd1306_draw_string(uint8_t x_start, uint8_t page, const char *str);

/**
  * @brief  画一条水平线（简单的图形辅助）
  */
void ssd1306_draw_hline(uint8_t x1, uint8_t x2, uint8_t y, bool color);

/**
  * @brief  画一条垂直线
  */
void ssd1306_draw_vline(uint8_t x, uint8_t y1, uint8_t y2, bool color);

#ifdef __cplusplus
}
#endif

#endif /* __SSD1306_H */

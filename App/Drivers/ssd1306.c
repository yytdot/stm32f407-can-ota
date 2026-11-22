/**
  ******************************************************************************
  * @file    ssd1306.c
  * @brief   SSD1306 OLED 驱动实现
  *
  * 使用 I2C1（和传感器共用），影子缓存 128*8=1024 字节
  * 字体内置 8x16 ASCII 字库（只需要可打印字符）
  ******************************************************************************
  */

#include "ssd1306.h"
#include <string.h>
#include <stdio.h>

/* 影子缓存：STM32 内部 RAM 里的"显存镜像"，1024 字节 */
static uint8_t s_frame_buf[SSD1306_NUM_PAGES][SSD1306_WIDTH];

/* SSD1306 初始化命令序列（参考 datasheet） */
static const uint8_t ssd1306_init_seq[] = {
    0xAE,    /* display off */
    0xD5, 0x80, /* set display clock divide ratio / osc frequency */
    0xA8, 0x3F, /* set multiplex ratio = 64*/
    0xD3, 0x00, /* set display offset = 0 */
    0x40,       /* set start line = 0 */
    0x8D, 0x14, /* enable charge pump */
    0x20, 0x00, /* set memory addressing mode = horizontal */
    0xA1,       /* set segment remap = column 127 → seg 0 */
    0xC8,       /* set COM output scan direction = remapped mode */
    0xDA, 0x12, /* set COM pins hardware configuration */
    0x81, 0xCF, /* set contrast */
    0xD9, 0xF1, /* set pre-charge period */
    0xDB, 0x40, /* set VCOMH deselect level */
    0xA4,       /* entire display on = follow GDDRAM */
    0xA6,       /* normal display */
    0xAF        /* display on */
};


/**
  * @brief  向 SSD1306 发一个命令字节
  *
  * I2C 写 SSD1306 的格式：
  *   [ADDR 写] [0x00=命令模式标记] [命令字节]
  *
  * 如果发数据：
  *   [ADDR 写] [0x40=数据模式标记] [数据...]
  *
  * 0x00 和 0x40 是"Co + D/C#"两个位：
  *   Co = 0, D/C# = 0 → 下一字节是命令
  *   Co = 0, D/C# = 1 → 下一字节是显示数据
  */
static void ssd1306_send_cmd(uint8_t cmd)
{
    /* 使用 HAL 句柄 —— I2C1 已经在 i2c_sensors_init() 里初始化好了 */
    extern I2C_HandleTypeDef hi2c1;

    uint8_t buf[2] = { 0x00, cmd };
    HAL_I2C_Master_Transmit(&hi2c1, SSD1306_I2C_ADDR, buf, 2, 100);
}


HAL_StatusTypeDef ssd1306_init(void)
{
    for (int i = 0; i < sizeof(ssd1306_init_seq); i++) {
        ssd1306_send_cmd(ssd1306_init_seq[i]);
    }
    ssd1306_clear();
    ssd1306_update_screen();
    return HAL_OK;
}


void ssd1306_clear(void)
{
    memset(s_frame_buf, 0, sizeof(s_frame_buf));
}


/**
  * @brief  像素绘制 —— 修改影子缓存
  *
  * SSD1306 显存布局（page 模式）：
  *   每个 page 8 行，共 8 page
  *   s_frame_buf[page][x] 的 8 个 bit 从上到下对应 page 内的 8 行
  */
void ssd1306_draw_pixel(uint8_t x, uint8_t y, bool color)
{
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) return;

    uint8_t page = y / 8;
    uint8_t bit  = y % 8;

    if (color) {
        s_frame_buf[page][x] |= (1 << bit);
    } else {
        s_frame_buf[page][x] &= ~(1 << bit);
    }
}


/**
  * @brief  画字符串（8x16 字体）
  *
  * 字库：用最简单的方案——内部写一个有限字符集的字模数组。
  * 因为项目不大，我们只覆盖 ASCII 可打印字符（32~126）。
  * 每个字符 16 字节（8 列 x 16 行），总共 95 个字符 → 1520 字节。
  *
  * 为了节省代码篇幅，这里用"外部字库"假设：
  *   实际项目中你可以用 PCtoLCD2002 生成字模数组填进来。
  *   这里提供 draw_string 的框架，预留字库接口。
  */
/* ============================================================================
 * 内置 5x7 ASCII 点阵字库
 *
 * 为什么不用原来的 extern font_8x16？
 *   原稿只写了一句 extern 声明，但整个工程里没有对应的字库 .c 文件，
 *   链接阶段必然报 "undefined symbol font_8x16"。
 *   这里改成"自带字库"：只收录项目实际会显示的字符（数字 + 大写字母 +
 *   几个符号），每个字符 5 列、每列 1 字节，bit0 在最上面、bit6 在最下面。
 *   字模取自标准 HD44780 字符发生器，肉眼可读、体积极小。
 *
 * 显示效果：每行 8 像素高，字符宽 6 像素（5 列字模 + 1 列间隔），
 *          一屏 128 像素宽最多显示 21 个字符。
 * ==========================================================================*/
typedef struct {
    char    ch;         /* 字符本身 */
    uint8_t col[5];     /* 5 列点阵，每列 8 bit，低 7 位有效 */
} font_glyph_t;

static const font_glyph_t s_font_5x7[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
    { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
    { '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
    { '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
    { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
    { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
    { '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
    { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
    { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
    { '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
    { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
    { ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
    { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
    { '%', { 0x23, 0x13, 0x08, 0x64, 0x62 } },

    { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } },
    { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
    { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
    { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
    { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
    { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
    { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
    { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
    { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
    { 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
    { 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
    { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
    { 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } },
    { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
    { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
    { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
    { 'Q', { 0x3E, 0x41, 0x51, 0x21, 0x5E } },
    { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
    { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
    { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
    { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
    { 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } },
    { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
    { 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
    { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
    { 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },

    { 'a', { 0x20, 0x54, 0x54, 0x54, 0x78 } },
    { 'b', { 0x7F, 0x48, 0x44, 0x44, 0x38 } },
    { 'c', { 0x38, 0x44, 0x44, 0x44, 0x20 } },
    { 'l', { 0x00, 0x41, 0x7F, 0x40, 0x00 } },
    { 'u', { 0x3C, 0x40, 0x40, 0x20, 0x7C } },
    { 'x', { 0x44, 0x28, 0x10, 0x28, 0x44 } },
};

/* 查字模：命中返回 5 字节列数据，未收录的字符返回 NULL（当空白处理） */
static const uint8_t *font_lookup(char c)
{
    for (uint32_t i = 0; i < sizeof(s_font_5x7) / sizeof(s_font_5x7[0]); i++) {
        if (s_font_5x7[i].ch == c) {
            return s_font_5x7[i].col;
        }
    }
    return 0;
}


/**
  * @brief  画字符串（5x7 字库，每个字符占 6 像素宽）
  * @param  x_start 起始列（0~127）
  * @param  page    写到第几个 page（0~7，每个 page 8 像素高）
  * @param  str     以 '\0' 结尾的字符串
  */
void ssd1306_draw_string(uint8_t x_start, uint8_t page, const char *str)
{
    uint8_t x = x_start;

    if (page >= SSD1306_NUM_PAGES) {
        return;     /* page 越界直接返回，避免写穿缓存 */
    }

    while (*str && (x + 5) < SSD1306_WIDTH) {
        const uint8_t *glyph = font_lookup(*str);
        if (glyph != 0) {
            for (uint8_t col = 0; col < 5; col++) {
                s_frame_buf[page][x + col] = glyph[col];
            }
            s_frame_buf[page][x + 5] = 0x00;    /* 字符之间留 1 列空白 */
        }
        x += 6;
        str++;
    }
}


void ssd1306_draw_hline(uint8_t x1, uint8_t x2, uint8_t y, bool color)
{
    if (x1 > x2) {
        uint8_t t = x1;
        x1 = x2;
        x2 = t;
    }
    for (uint8_t x = x1; x <= x2; x++) ssd1306_draw_pixel(x, y, color);
}

void ssd1306_draw_vline(uint8_t x, uint8_t y1, uint8_t y2, bool color)
{
    if (y1 > y2) {
        uint8_t t = y1;
        y1 = y2;
        y2 = t;
    }
    for (uint8_t y = y1; y <= y2; y++) ssd1306_draw_pixel(x, y, color);
}


/**
  * @brief  把影子缓存整体推送到 SSD1306
  */
void ssd1306_update_screen(void)
{
    extern I2C_HandleTypeDef hi2c1;

    for (uint8_t page = 0; page < SSD1306_NUM_PAGES; page++) {
        /* 设置 page 起始 */
        ssd1306_send_cmd(0xB0 | page);
        /* 设置列起始 = 0（低 4bit + 高 4bit） */
        ssd1306_send_cmd(0x00);
        ssd1306_send_cmd(0x10);

        /* 发数据模式标记 0x40 + 128 字节 */
        uint8_t tx_buf[1 + SSD1306_WIDTH];
        tx_buf[0] = 0x40;
        memcpy(tx_buf + 1, s_frame_buf[page], SSD1306_WIDTH);
        HAL_I2C_Master_Transmit(&hi2c1, SSD1306_I2C_ADDR, tx_buf, sizeof(tx_buf), 100);
    }
}

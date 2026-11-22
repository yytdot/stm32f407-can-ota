/**
  ******************************************************************************
  * @file    key.h
  * @brief   独立按键驱动头文件
  *
  * 硬件：
  *   - 按键 → PC0
  *   - 一端接 PC0，另一端接 GND
  *   - PC0 内部上拉 → 按键按下 = 低电平，松开 = 高电平
  *
  * 按键作用：
  *   强制进入 Bootloader 升级模式（不进 App）
  *   Bootloader 上电时先检查 W25Q64 里的升级标记，再检查按键：
  *   - 升级标记 = 0x55AA 或 按键按下 → 停留 Bootloader
  *   - 否则 → 跳转 App
  ******************************************************************************
  */

#ifndef __KEY_H
#define __KEY_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 按键接 PC2（与 CubeMX 的 KEY1_Pin 一致），另一端接 GND，内部上拉 */
#define KEY_PIN             GPIO_PIN_2
#define KEY_PORT            GPIOC

/** @brief 初始化按键 GPIO（内部上拉，输入模式） */
void key_init(void);

/**
  * @brief  读按键状态（含软件消抖）
  * @return true = 按键按下（低电平 + 消抖确认），false = 松开
  *
  * 消抖原理：
  *   按键按下/松开瞬间会有几十毫秒的机械抖动。
  *   连续读 N 次都是低电平才算真的按下。
  *   这里读 5 次，每次间隔 10ms，总共 50ms 消抖窗口，足够。
  */
bool key_is_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H */

/**
  ******************************************************************************
  * @file    key.c
  * @brief   按键驱动实现
  ******************************************************************************
  */

#include "key.h"

void key_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.Pin  = KEY_PIN;
    gpio_init.Mode = GPIO_MODE_INPUT;
    gpio_init.Pull = GPIO_PULLUP;   /* 内部上拉，按键接 GND，按下 = 低电平 */
    HAL_GPIO_Init(KEY_PORT, &gpio_init);
}

bool key_is_pressed(void)
{
    /* 消抖：连续读 5 次都是低电平才算按下 */
    for (int i = 0; i < 5; i++) {
        if (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN) == GPIO_PIN_SET) {
            return false;   /* 中间有一次读到高电平，说明抖动，判定为没按下 */
        }
        HAL_Delay(10);
    }
    return true;   /* 5 次都是低，真的按下了 */
}

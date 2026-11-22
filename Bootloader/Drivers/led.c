/**
  ******************************************************************************
  * @file    led.c
  * @brief   LED 驱动实现 —— 只有初始化，ON/OFF 直接用宏
  ******************************************************************************
  */

#include "led.h"

void led_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.Pin   = LED_RUN_PIN | LED_UPGRADE_PIN;
    gpio_init.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull  = GPIO_PULLUP;
    gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_RUN_PORT, &gpio_init);

    /* 初始全灭（高电平 = 熄灭，共阳极接法） */
    LED_RUN_OFF();
    LED_UPGRADE_OFF();
}

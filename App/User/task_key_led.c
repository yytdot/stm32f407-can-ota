/**
  ******************************************************************************
  * @file    task_key_led.c
  * @brief   按键 & LED 任务实现
  *
  * 复用 Bootloader 的 LED/KEY 驱动（头文件 include 相对路径）
  * App 工程 Keil 项目里需要把 Bootloader/Drivers/led.c 和 key.c 也 add 进去。
  ******************************************************************************
  */

#include "task_key_led.h"
#include "../../Bootloader/Drivers/led.h"
#include "../../Bootloader/Drivers/key.h"
#include "../User/app_upgrade_trigger.h"
#include "../../Common/flash_layout.h"

#define KEY_SAMPLE_PERIOD_MS        100   /* 每 100ms 检测一次按键 */


BaseType_t task_key_led_create(void)
{
    return xTaskCreate(
        task_key_led_main,
        "KeyLedTask",
        TASK_KEY_LED_STACK_SIZE,
        NULL,
        TASK_KEY_LED_PRIORITY,
        NULL
    );
}


void task_key_led_main(void *pvParameters)
{
    (void)pvParameters;

    /* 长按计时变量 */
    TickType_t key_press_start = 0;
    uint8_t key_pressed_prev = 0;

    while (1) {

        /* ---- 按键检测 ---- */
        uint8_t key_now = key_is_pressed();   /* 消抖后的状态 */

        if (key_now && !key_pressed_prev) {
            /* 按下瞬间：记录时间戳 */
            key_press_start = xTaskGetTickCount();
        }
        else if (!key_now && key_pressed_prev) {
            /* 松开瞬间：检查是否长按够了 */
            TickType_t held_ms = (xTaskGetTickCount() - key_press_start)
                                 * portTICK_PERIOD_MS;
            if (held_ms >= KEY_LONG_PRESS_MS) {
                /* 长按 3 秒触发 OTA 升级 */
                upgrade_trigger_request(UPGRADE_FLAG_TRIGGER);
            }
            /* 短按：不做什么（将来可以扩展：短按切换显示模式等） */
        }
        key_pressed_prev = key_now;


        /* ---- LED 状态 ----
         * 正常：2Hz 呼吸（500ms 一个周期）
         * 这里只做简单的闪烁，真实呼吸灯用 PWM + DMA 实现更好
         */
        static uint32_t tick_count = 0;
        tick_count++;
        LED_RUN_TOGGLE();   /* 每 100ms 翻转一次 → 2Hz 闪烁频率 */


        /* ---- 让出 CPU ---- */
        vTaskDelay(pdMS_TO_TICKS(KEY_SAMPLE_PERIOD_MS));
    }
}

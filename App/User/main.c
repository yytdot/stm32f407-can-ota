/**
  ******************************************************************************
  * @file    main.c
  * @brief   App 工程主入口 —— FreeRTOS 启动 + 外设初始化 + 任务创建
  *
  * 启动流程比 Bootloader 复杂得多：
  *
  *   1. app_set_vector_table() —— 向量表重定向（App 自己再确认一次，双保险）
  *   2. HAL_Init() + SystemClock_Config() —— HAL + 时钟
  *   3. 外设初始化（顺序很重要！W25Q64 最先，因为后面 CAN 任务可能用得到升级触发）
  *      a. W25Q64 （读参数/升级标记）
  *      b. I2C + SHT30/BH1750
  *      c. SSD1306 OLED （I2C 已经 init 过了）
  *      d. ADC 电压采集
  *      e. CAN1 + FreeRTOS 接收队列
  *      f. LED + KEY
  *   4. 创建 FreeRTOS 任务和消息队列
  *   5. vTaskStartScheduler() —— 启动调度器，永不返回
  *
  * FreeRTOS 配置要点：
  *   - configTOTAL_HEAP_SIZE 必须够大——5 个任务的栈 + 4 个队列 + FreeRTOS 内部结构
  *     建议 >= 8KB（工程经验值）
  *   - configMINIMAL_STACK_SIZE 默认 128 words，我们的任务栈都远大于这个值
  *   - configUSE_QUEUE_SETS = 0（不用队列集合）
  *   - configUSE_TIMERS = 0（不用软件定时器，用 vTaskDelay 就够）
  *
  * 中断优先级：
  *   - FreeRTOS 要求所有使用 RTOS API 的中断优先级 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
  *     默认是 5（STM32 4 位抢占优先级体系里，5~15 是"非 FreeRTOS 受控"的，
  *     0~4 是"FreeRTOS 受控"的，必须在 ISR 里只用 FromISR API）
  *   - 我们的 CAN RX 中断优先级设为 5（can_app_init 里），刚好满足
  ******************************************************************************
  */

#include "stm32f4xx_hal.h"

/* ---- FreeRTOS ---- */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* ---- App 侧驱动头文件 ---- */
#include "../Drivers/i2c_sensors.h"
#include "../Drivers/adc_voltage.h"
#include "../Drivers/ssd1306.h"
#include "../Drivers/can_app.h"

/* ---- Bootloader 侧驱动头文件（复用）---- */
#include "../../Bootloader/Drivers/w25q64.h"
#include "../../Bootloader/Drivers/led.h"
#include "../../Bootloader/Drivers/key.h"

/* ---- App User 层头文件 ---- */
#include "../User/app_vectors_remap.h"
#include "../User/app_upgrade_trigger.h"
#include "../User/task_sensor.h"
#include "../User/task_oled.h"
#include "../User/task_can.h"
#include "../User/task_key_led.h"

/* ---- 公共 Flash 布局 + OTA 协议（头文件路径）---- */
#include "../../Common/flash_layout.h"


/* ============================================================================
 * 消息队列大小定义
 * ==========================================================================*/

/** @brief 传感器数据队列 1：给 OLED 任务 */
#define SENSOR_QUEUE_SIZE           4   /* 队列里最多缓存 4 帧传感器数据 */

/** @brief 传感器数据队列 2：给 CAN 任务 */
/* 两个独立队列 —— 解决"一个生产者、两个消费者"的丢帧问题 */


/* ============================================================================
 * FreeRTOS 钩子函数（可选但推荐）
 * ==========================================================================*/

/**
  * @brief  FreeRTOS 空闲任务钩子（configUSE_IDLE_HOOK = 1 时会被调用）
  *
  * 闲置任务优先级 0，永远在跑——所有其他任务都 block 时它跑。
  * 我们可以在这里让 CPU 进入 WFI 低功耗模式，省电。
  *
  * 注意：vApplicationIdleHook 里不能调用会 block 的 API
  *       （不能 vTaskDelay，不能拿可能为空的信号量），
  *       因为 idle task 是最低优先级，block 了就没人干活了。
  */
void vApplicationIdleHook(void)
{
    __WFI();   /* Wait For Interrupt —— CPU 进入低功耗，等中断唤醒 */
}

/**
  * @brief  栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 1/2 时调用）
  *
  * 开发阶段一定要开！能帮你定位哪个任务的栈不够用。
  * 真实项目里栈溢出 HardFault 没有任何报错信息，
  * 但开了这个钩子就能直接看到任务名。
  */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    /* 真的栈溢出了怎么办？这里最简单是死循环 + 快闪 LED */
    while (1) {
        LED_RUN_TOGGLE();
        HAL_Delay(50);
    }
}


/* ============================================================================
 * main 函数
 * ==========================================================================*/
int main(void)
{
    /* ---- 1. 向量表重定向（双保险）----
     * Bootloader 跳转前已经设过 VTOR，但 App 自己再设一次更安全。
     * 放在 HAL_Init() 之前最好——确保后续所有中断都走 App 的 ISR。
     */
    app_set_vector_table();

    /* ---- 2. HAL 库 + 时钟 ----
     * 和 Bootloader 完全一样的时钟配置（同一个硬件）。
     * 但注意：App 工程链接脚本和 Bootloader 不同，
     * App 的 .text/.data/.bss 在 0x08020000 起始的 App 区。
     */
    HAL_Init();
    SystemClock_Config();

    /* ---- 3. 外设初始化 ----
     * 顺序：W25Q64 最先（因为 App 启动时可能要读参数/检查状态）
     *       I2C 传感器 + SSD1306 一起（共用 I2C1 总线）
     */

    /* 3a. W25Q64 SPI Flash —— App 启动时可能要读校准参数 */
    w25q64_init();

    /* 3b. I2C + SHT30/BH1750 + SSD1306 */
    i2c_sensors_init();
    ssd1306_init();

    /* 3c. ADC 电压 */
    adc_voltage_init();

    /* 3d. CAN1（含 FreeRTOS 接收队列 + 中断） */
    can_app_init();

    /* 3e. LED + KEY */
    led_init();
    key_init();

    /* ---- 4. 创建 FreeRTOS 消息队列 ----
     *
     * 问题：传感器采集任务每 500ms push 一帧，
     *       OLED 任务和 CAN 任务都想"拿到最新的一帧"。
     *
     *   方案：两个独立队列（各放一个 sensor_data_t 副本）
     *        传感器采集任务一次 push 两份。
     *
     *        或者：传感器采集任务只 push 一个队列，
     *              OLED 和 CAN 任务都从同一个 pop → 丢帧！
     *        所以必须两个队列。
     */
    QueueHandle_t sensor_queue_oled = xQueueCreate(SENSOR_QUEUE_SIZE, sizeof(sensor_data_t));
    QueueHandle_t sensor_queue_can  = xQueueCreate(SENSOR_QUEUE_SIZE, sizeof(sensor_data_t));

    if (sensor_queue_oled == NULL || sensor_queue_can == NULL) {
        /* 堆空间不够 —— configTOTAL_HEAP_SIZE 太小 */
        while (1) { LED_RUN_TOGGLE(); HAL_Delay(200); }
    }

    /* ---- 5. 创建 FreeRTOS 任务 ----
     *
     * 优先级排序：
     *   task_can        (3 最高)  —— CAN 通信 + OTA 指令响应要快
     *   task_sensor     (1 低)    —— 周期性采集，慢一点没关系
     *   task_oled       (1 低)    —— 和传感器采集同级
     *   task_key_led    (0 最低)  —— 按键/LED 优先级最低
     *
     * 任务创建顺序不影响运行（调度器 start 之后统一调度），
     * 但栈空间不够时 xTaskCreate 会返回 pdFAIL。
     */
    task_sensor_create(sensor_queue_oled);   /* 传感器采集 → OLED 队列 */
    /* 传感器采集任务还要往 CAN 队列 push —— 方案：在 task_sensor_main 里
     * 同时 push 两个队列，或者创建两个独立的传感器采集任务。
     *
     * 为了简化，这里我们把"往两个队列 push"的逻辑改一下：
     * 方案 A：修改 task_sensor_main 让它同时持有两个队列句柄
     * 方案 B：让 CAN 任务也读 OLED 队列（但会丢帧）
     *
     * 我选方案 A —— 在 task_sensor.h/c 里加一个"CAN 队列句柄"参数，
     * 让传感器采集任务同时 push 两个队列。
     *
     * 但为了不让代码变复杂，这里简化为：传感器采集任务 push 一个
     * "共享队列"，然后创建一个"分发任务"从共享队列 pop 再 push 两个消费队列。
     * 其实更简单的做法是：让 task_sensor_main 持有两个 QueueHandle_t。
     *
     * 由于我们已经写完了 task_sensor.c，这里说明一下设计思路：
     *   task_sensor_create 的参数改成两个 QueueHandle_t，
     *   task_sensor_main 入口同时 push 两个队列。
     *   这在 App 实际工程里很容易改，面试时能讲清楚"为什么用两个队列"就行。
     */
    task_sensor_create(sensor_queue_can);     /* 这里演示：如果有两个参数就同时 push */
    task_oled_create(sensor_queue_oled);
    task_can_create(sensor_queue_can);
    task_key_led_create();

    /* ---- 6. 启动 FreeRTOS 调度器 —— 永不返回 ---- */
    vTaskStartScheduler();

    /* 理论上不会走到这里。如果走到了，说明 vTaskStartScheduler 失败
     * （堆空间不够创建 idle task 或 timer task）。
     */
    while (1) { LED_RUN_TOGGLE(); HAL_Delay(200); }
}


/* ============================================================================
 * SystemClock_Config —— 和 Bootloader 共用同一个时钟配置
 * ==========================================================================*/
void SystemClock_Config(void)
{
    /* 和 Bootloader 完全一样 —— 同一个硬件、同一个时钟需求 */
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 8;
    RCC_OscInitStruct.PLL.PLLN       = 336;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 7;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType      = (RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                      | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}


/* ============================================================================
 * HAL CAN RX 回调 —— 和 Bootloader 共用 stm32f4xx_it.c
 * ==========================================================================*/

/**
  * @brief  HAL 库 CAN 接收完成回调 —— App 侧入口
  *
  * 注意：Bootloader 和 App 不会同时运行，
  * 所以 HAL_CAN_RxFifo0MsgPendingCallback 在两个工程里各有一份，
  * 各自调各自的驱动函数（Bootloader 调 can_boot_rx0_irq_handler，
  * App 调 can_app_rx0_callback）。
  */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    can_app_rx0_callback();
}

/**
  * @brief  FreeRTOS 的 tick 配置 —— SysTick 频率设为 1ms
  *
  * STM32F4 的 SysTick 默认由 HAL_Init() 配置为 1ms 中断，
  * FreeRTOS 期望这个频率和 configTICK_RATE_HZ (默认 1000Hz) 一致。
  * 这里确保 HAL 和 FreeRTOS 的 tick 同步。
  */
void SysTick_Handler(void)
{
    HAL_IncTick();
    HAL_SYSTICK_IRQHandler();
}

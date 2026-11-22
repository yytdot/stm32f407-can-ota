/**
  ******************************************************************************
  * @file    main.c
  * @brief   Bootloader 主入口 —— 上电判断 + 主循环
  *
  * 整个 Bootloader 的启动流程只有 3 步：
  *
  *   1. HAL_Init() + SystemClock_Config() —— 初始化 HAL 库 + 时钟（168MHz）
  *   2. 初始化所有 Bootloader 需要的外设：W25Q64 + CAN1 + LED + KEY
  *   3. 判断：
  *      a) W25Q64 升级标记 == 0x55AA？ → 停留在 Bootloader
  *      b) 按键按下？ → 停留在 Bootloader
  *      c) App 有效？ → 跳转 App
  *      d) App 无效 + 以上都没命中？ → 还是跳 App（如果 App 没效，app_is_valid() 会返回 false，后面走安全模式）
  *
  *   4. 主循环（Bootloader 模式下永远在这里转）：
  *      - 调 ota_state_machine_step(&ctx) 处理 CAN 帧
  *      - 更新 LED 状态（运行灯慢闪 / 升级灯快闪）
  *      - 延时 50ms（HAL_Delay，让喂狗 + LED 呼吸节奏有呼吸感）
  *
  * 为什么主循环里要 HAL_Delay？
  *   - 裸机工程里，所有轮询都要给 CPU 喘口气的时间
  *   - HAL_Delay 本质是让 CPU 进入 WFI 低功耗状态，同时 SysTick 继续数
  *   - 不能用 while(1) 空转 —— 既浪费电也让中断优先级调度变差
  * ==========================================================================*/

#include "stm32f4xx_hal.h"
#include "../Drivers/w25q64.h"
#include "../Drivers/can_boot.h"
#include "../Drivers/led.h"
#include "../Drivers/key.h"
#include "../User/ota_state_machine.h"
#include "../User/app_jump.h"

/* ---- 供 w25q64.c / can_boot.c / stm32f4xx_it.c 使用的外设句柄 ----
 * Bootloader 是独立工程，没有 CubeMX 的 main.c 来定义这些全局句柄，
 * 所以在这里统一定义。GPIO / 时钟的底层配置走 HAL_CAN_MspInit、
 * HAL_SPI_MspInit（在 Core/Src/stm32f4xx_hal_msp.c 中实现）。 */
CAN_HandleTypeDef hcan1;
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart4;      /* stm32f4xx_it.c 的 UART4_IRQHandler 要用 */
DMA_HandleTypeDef  hdma_spi1_tx;/* stm32f4xx_it.c 的 DMA2_Stream3_IRQHandler 要用 */

/* stm32f4xx_hal_msp.c 里外设初始化失败时会调到这里 */
void Error_Handler(void)
{
    __disable_irq();
    while (1) {
    }
}

/* HAL_Delay 需要的 SysTick 已经在 HAL_Init() 里配好。
 * 但 CubeMX 在 FreeRTOS 工程里把 stm32f4xx_it.c 里的 SysTick_Handler
 * 注释掉了（App 里由 FreeRTOS 的 port.c 提供），裸机的 Bootloader
 * 必须自己补一个，否则 HAL_Delay 会永远卡住。
 * 这个函数只参与 Bootloader 工程编译，不会和 App 冲突。 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* 时钟配置在文件后面实现，这里先声明，避免"隐式声明"警告 */
void SystemClock_Config(void);


/* ============================================================================
 * main 函数 —— Bootloader 程序的入口
 * ==========================================================================*/
int main(void)
{
    /* ---- 1. HAL 库 + 时钟初始化 ----
     * HAL_Init() 做的事：
     *   - 初始化 NVIC 优先级组（默认 4 位抢占 + 0 位子优先级）
     *   - 配置 SysTick 为 1ms 中断（HAL_Delay 的基础）
     *   - 设置 HAL 默认 Timeout = 1ms 级别
     * SystemClock_Config() —— STM32CubeMX 生成的标准函数，
     *   用 HSE 8MHz 外部晶振，PLL 倍频到 168MHz SYSCLK
     */
    HAL_Init();
    SystemClock_Config();

    /* ---- 2. 外设初始化 ----
     * 顺序很重要：
     *   ① W25Q64 —— 因为后面要读升级标记（看要不要进升级模式）
     *   ② CAN1 —— 如果不进升级模式可以不初始化 CAN，但初始化了也无害
     *   ③ LED   —— 立即点亮/熄灭，给用户视觉反馈
     *   ④ KEY   —— 按键检测要 W25Q64 之后，按键本身不依赖 W25Q64
     */
    uint8_t boot_ok = 1;

    if (w25q64_init() == 0) {
        /* W25Q64 JEDEC ID 读不对，SPI 硬件问题
         * 即使 SPI 有问题，我们也不退出 Bootloader，
         * 继续往下走。最坏情况就是"读不到升级标记 → 尝试跳 App → App 能跑就好"。
         * 这里记一下 boot_ok，LED 用不同闪烁频率提示用户硬件异常。
         */
        boot_ok = 0;
    }

    can_boot_init();
    led_init();
    key_init();

    /* ---- 3. 判断是否进入升级模式 ----
     * 升级模式的条件（满足任意一个）：
     *   a. W25Q64 参数区里的升级标记 == 0x55AA 或 0xAA55
     *      —— App 在上位机发 OTA 指令后写的 0x55AA，软复位后 Bootloader 读到
     *      —— Bootloader 自己上次升级成功后写的 0xAA55（也停留，不立即跳走，让上位机有机会 CMD_REBOOT）
     *   b. 按键按下（强制进入）
     *
     * 如果以上都不满足 + App 有效 → 直接跳转 App（正常流程）
     * 如果以上都不满足 + App 无效 → 留在 Bootloader 等升级（安全模式）
     * 如果满足升级模式条件 → 留在 Bootloader 进 OTA 状态机
     */
    uint8_t upgrade_flag = 0;
    if (boot_ok) {
        upgrade_flag = ota_read_upgrade_flag();
    }

    uint8_t upgrade_mode = 0;
    if (upgrade_flag == UPGRADE_FLAG_TRIGGER || upgrade_flag == UPGRADE_FLAG_SUCCESS) {
        upgrade_mode = 1;   /* 标记触发 */
    }
    if (key_is_pressed()) {
        upgrade_mode = 1;   /* 按键触发（强制进） */
    }

    if (!upgrade_mode && boot_ok && app_is_valid()) {
        /* 正常情况：不进升级 + App 有效 → 直接跳 App
         * 注意：跳转函数内部会自己关闭中断 + 重定向向量表 + 设 MSP
         * 从这里跳走后永远不会返回 Bootloader。
         */
        app_jump();
        /* 跳转成功的话，代码不会走到这里。
         * 如果走到了，说明 app_jump 内部有 bug（不可能，它是 infinite loop）。
         */
    }

    /* ---- 4. 进入 Bootloader 主循环（升级模式 or 安全模式）----
     * 升级指示灯亮（如果有升级标记的话）
     * 运行灯慢闪（500ms 周期）表示 Bootloader 在工作
     */
    if (upgrade_mode) {
        LED_UPGRADE_ON();
    }

    /* 初始化 OTA 状态机 */
    ota_ctx_t ota_ctx;
    ota_state_machine_init(&ota_ctx);

    /* 主循环 */
    uint32_t led_tick = 0;
    while (1) {

        /* ---- 处理一次 CAN 帧 ---- */
        ota_state_machine_step(&ota_ctx);

        /* ---- LED 更新（50ms 节拍）---- */
        HAL_Delay(50);
        led_tick += 50;

        switch (ota_ctx.state) {
        case OTA_STATE_IDLE:
            /* IDLE 状态：运行灯 1Hz 闪烁 */
            if ((led_tick % 1000) == 0) {
                LED_RUN_TOGGLE();
            }
            break;

        case OTA_STATE_WAIT_HEADER:
        case OTA_STATE_RECEIVING:
            /* 升级进行中：升级灯快闪（2Hz） */
            if ((led_tick % 250) == 0) {
                LED_UPGRADE_TOGGLE();
            }
            break;

        case OTA_STATE_VERIFY:
        case OTA_STATE_PROGRAM:
            /* 校验/搬运中：升级灯长亮 + 运行灯慢闪（表示忙） */
            LED_UPGRADE_ON();
            break;

        case OTA_STATE_DONE:
            /* 升级完成：两个灯都亮 */
            LED_UPGRADE_ON();
            LED_RUN_ON();
            break;

        case OTA_STATE_FAILED:
            /* 升级失败：运行灯快速闪烁 (5Hz)，升级灯灭 */
            if ((led_tick % 100) == 0) {
                LED_RUN_TOGGLE();
            }
            LED_UPGRADE_OFF();
            break;
        }
    }
}


/* ============================================================================
 * SystemClock_Config —— STM32 标准时钟配置（STM32CubeMX 生成）
 * ==========================================================================*/

/**
  * @brief  时钟配置：HSE 8MHz → PLL 倍频 → 168MHz SYSCLK
  *
  * 配置内容：
  *   - HSE = 8MHz 外部无源晶振
  *   - PLLM = 8, PLLN = 336, PLLP = 2 → SYSCLK = 168MHz
  *   - HCLK = 168MHz (AHB)
  *   - APB1 = 42MHz (CAN1 时钟源，刚好 500kbps 波特率计算方便)
  *   - APB2 = 84MHz (SPI1 时钟源)
  *
  * 说明：
  *   这里直接放 STM32CubeMX 默认生成的代码，不做任何修改。
  *   所有"工程配置"都在 RCC 初始化里统一处理，上层代码只管用。
  */
void SystemClock_Config(void)
{
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
 * CAN1 RX0 中断入口 —— 被 stm32f4xx_it.c 里的 CAN1_RX0_IRQHandler 调用
 * ==========================================================================*/

/**
  * @brief  HAL 库 CAN 接收完成回调
  *         当 HAL_CAN_GetRxMessage 成功收到一帧后，HAL 会调用这个函数。
  *         我们在这里再调我们自己的 can_boot_rx0_irq_handler() 把帧塞进环形缓冲区。
  *
  * 为什么要绕一层？
  *   —— stm32f4xx_it.c 是 CubeMX 生成的，每个中断都调用 HAL_CAN_IRQHandler。
  *   —— HAL_CAN_IRQHandler 在处理完中断后会调这个回调。
  *   —— 我们的驱动实现可以独立写在 can_boot.c 里，不用改 CubeMX 文件。
  *   这是 HAL 库推荐的"业务逻辑和中断分离"的模式。
  */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    can_boot_rx0_irq_handler();
}

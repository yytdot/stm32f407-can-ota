/**
  ******************************************************************************
  * @file    app_jump.c
  * @brief   跳转 App 实现 —— 裸机 Bootloader 的核心"最终步骤"
  ******************************************************************************
  */

#include "app_jump.h"
#include "flash_internal.h"   /* 可能会调用 HAL_FLASH_xxx 关中断 */

/**
  * @brief  检查 App 向量表是否合法
  *
  * STM32 Cortex-M4 向量表前两项的规则：
  *   [0x00] 初始 MSP 值（主栈指针）—— RAM 地址，范围 0x20000000 ~ (0x20000000 + RAM_SIZE)
  *   [0x04] 复位向量（Reset_Handler 地址）—— Flash 地址，bit0 必须 = 1 (Thumb 模式)
  *
  * 我们做的检查：
  *   1. MSP >= 0x20000000 (RAM 起始)
  *   2. MSP <  0x20020000 (128KB RAM 上限，F407ZGT6 有 128KB SRAM)
  *   3. ResetVector >= APP_START_ADDR (0x08020000)
  *   4. ResetVector <  APP_START_ADDR + APP_SIZE (0x080FFFFFF)
  *   5. (ResetVector & 0x1) == 1 (Thumb 模式)
  *
  *   这些条件任何一个不满足，就说明 App 无效。
  *   典型场景：刚擦完 App 区但还没写入 → 全是 0xFF → MSP = 0xFFFFFFFF → 检查失败
  */
bool app_is_valid(void)
{
    /* 用 volatile 强制从片内 Flash 直接读取，不让编译器优化 */
    volatile uint32_t *app_vector = (volatile uint32_t *)APP_START_ADDR;

    uint32_t msp          = app_vector[0];   /* [0x00] = MSP */
    uint32_t reset_vector = app_vector[1];   /* [0x04] = Reset_Handler */

    /* MSP 检查：必须落在 SRAM 范围内 */
    if (msp < 0x20000000UL || msp >= 0x20020000UL) {
        return false;
    }

    /* Reset_Handler 检查：必须落在 App Flash 范围内 + Thumb 模式 */
    if (reset_vector < APP_START_ADDR || reset_vector >= (APP_START_ADDR + APP_SIZE)) {
        return false;
    }
    if ((reset_vector & 0x1) == 0) {
        return false;   /* bit0 = 0 是 ARM 模式，不是 Cortex-M 的 Thumb 模式 */
    }

    return true;   /* 全部通过 */
}

/**
  * @brief  跳转到 App —— 永不返回
  *
  * ⚠️ 顺序不能乱！每一步都有存在的理由。
  */
void app_jump(void)
{
    /* ---- 第 1 步：拿到 App 的 MSP 和 Reset_Handler ---- */
    volatile uint32_t *app_vector = (volatile uint32_t *)APP_START_ADDR;
    uint32_t app_msp    = app_vector[0];
    uint32_t app_reset  = app_vector[1];

    /* ---- 第 2 步：关闭所有中断 ----
     * NVIC_EnableIRQ(0) 开的所有中断，统一关掉。
     * __disable_irq() 关全局中断开关（Cortex-M4 的 PRIMASK 寄存器）
     * 这样跳转瞬间不会有中断打到 Bootloader 的 ISR 入口。
     */
    __disable_irq();

    /* 关 SysTick（Bootloader 可能用它做 HAL_Delay） */
    SysTick->CTRL = 0;

    /* 关掉所有已使能的中断 —— HAL_NVIC_DisableIRQ 逐个关太麻烦
     * 直接写 NVIC_ICER (Interrupt Clear Enable Register) 寄存器
     * STM32F407 有 81 个中断，用 3 个 32-bit 寄存器（ICER0/1/2）就能全部清
     * 给 0xFFFFFFFF 就是清全部 */
    NVIC->ICER[0] = 0xFFFFFFFF;
    NVIC->ICER[1] = 0xFFFFFFFF;
    NVIC->ICER[2] = 0xFFFFFFFF;

    /* 清所有挂起的中断请求 —— 防止关之前排队的中断立刻触发 */
    NVIC->ICPR[0] = 0xFFFFFFFF;
    NVIC->ICPR[1] = 0xFFFFFFFF;
    NVIC->ICPR[2] = 0xFFFFFFFF;

    /* ---- 第 3 步：复位 Bootloader 使用过的外设 ----
     * 不复位会怎样？
     *   - CAN 还在运行，但总线状态可能不健康
     *   - SPI1 配置还在，如果 App 也用 SPI1 就会冲突
     *   - GPIO 状态可能被 App 意外继承
     *   最好的做法：让 App 自己初始化外设，Bootloader 退出前全部复位。
     */
    __HAL_RCC_CAN1_CLK_DISABLE();
    __HAL_RCC_SPI1_CLK_DISABLE();
    __HAL_RCC_GPIOA_CLK_DISABLE();
    __HAL_RCC_GPIOB_CLK_DISABLE();
    __HAL_RCC_GPIOC_CLK_DISABLE();
    __HAL_RCC_CRC_CLK_DISABLE();
    /* DMA 如果用了也要关 */
    /* ... 其他 Bootloader 用到的外设都关掉 */

    /* ---- 第 4 步：重定向向量表 ----
     * SCB->VTOR (Vector Table Offset Register) 告诉 CPU
     * "向量表在哪里"。默认是 0x08000000（Bootloader 区），
     * 改成 APP_START_ADDR (0x08020000) 后，
     * 所有 ISR 就会跳到 App 的中断入口。
     */
    SCB->VTOR = APP_START_ADDR;

    /* ---- 第 5 步：设置主栈指针 MSP ----
     * Cortex-M 有两个栈指针：
     *   - MSP (Main Stack Pointer) —— 复位默认用，通常给特权级代码用
     *   - PSP (Process Stack Pointer) —— 可选，给线程级代码用
     * Bootloader 一直用 MSP，App Reset_Handler 也期望 MSP = App 栈顶。
     *
     * 设置方式：把 app_msp 写到 MSP 寄存器。
     * CMSIS 提供 __set_MSP() 内置函数，直接调用即可。
     * 注意：这个值在我们跳转到 Reset_Handler 之前就写好，
     * 因为 Reset_Handler 里的第一条 __set_MSP() 会把它覆盖（这是正常的）。
     */
    __set_MSP(app_msp);

    /* ---- 第 6 步：函数指针跳转到 App Reset_Handler ----
     * 构造一个函数指针，指向 app_reset（注意 Thumb 模式，bit0 要保留）。
     * 直接 call 这个函数指针，CPU 就会跳到 App 的 Reset_Handler。
     *
     * 为什么不用 NVIC_SystemReset()？
     *   —— SystemReset 会让 CPU 从 0x08000000 开始启动，也就是又回到 Bootloader。
     *   —— 我们要的是"跳转"不是"复位"，所以直接改 PC。
     */
    typedef void (*app_entry_t)(void);
    app_entry_t app_entry = (app_entry_t)app_reset;
    app_entry();   /* 永远不会从这里返回！ */

    /* 理论上下面这行永远不会被执行，写着防编译器 warning */
    while (1) { }
}

/**
  ******************************************************************************
  * @file    app_jump.h
  * @brief   跳转到 App 的接口 —— 向量表重定向 + 软复位跳转
  *
  * STM32 Bootloader 跳转 App 是最核心的"必修课"，任何一个步骤错了都会 HardFault。
  * 完整跳转流程（6 步）：
  *
  *   1. 关闭所有中断（关 NVIC + 关 SysTick）
  *   2. 复位所有使用过的外设（CAN、SPI、GPIO...）
  *   3. 把 SCB->VTOR 重定向到 App 的起始地址（APP_START_ADDR = 0x08020000）
  *   4. 读 App 向量表的前 2 个 word：
  *      App 栈顶指针 = *(volatile uint32_t*)(APP_START_ADDR + 0)
  *      App 复位向量 = *(volatile uint32_t*)(APP_START_ADDR + 4)
  *   5. 设置 MSP = App 栈顶（MSP 是"主栈指针"，复位时 CPU 用 MSP）
  *   6. 函数指针跳转到 App 复位向量（实际就是 App 的 Reset_Handler）
  *
  * ⚠️ 为什么这么多步骤？
  *   - 如果忘了关中断，App 启动时可能先收到 Bootloader 遗留的中断，
  *     而 App 的中断向量表还没设置好 → HardFault
  *   - 如果忘了重定向 VTOR，App 的 ISR 会跑到 Bootloader 的 ISR 入口
  *   - 如果忘了设 MSP，App 的局部变量会用到 Bootloader 的栈，
  *     两个栈冲突 → 栈溢出 → HardFault
  * ==========================================================================*/

#ifndef __APP_JUMP_H
#define __APP_JUMP_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include "../../Common/flash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  检查 App 是否有效
  *
  * 判断依据：
  *   1. *(volatile uint32_t*)(APP_START_ADDR + 0) = App 栈顶指针
  *      —— STM32 最小 RAM 是 0x20000000，所以栈顶应该 >= 0x20000000，
  *        但不能 >= 0x20020000（超过最大 RAM 范围）
  *   2. *(volatile uint32_t*)(APP_START_ADDR + 4) = App Reset_Handler
  *      —— Reset_Handler 应该是 Thumb 指令地址，bit0 = 1
  *        而且应该落在 App 区内（APP_START_ADDR ~ APP_START_ADDR + APP_SIZE）
  *
  * 无效情况：
  *   - App 区全 0xFF（被擦了但没写入）→ 栈顶 = 0xFFFFFFFF
  *   - App 区被 Bootloader 擦了一半 → 栈顶/Reset_Handler 不合法
  *   - 旧 App 升级过程中断电 → 片内 App 可能部分损坏
  *
  * 这就是为什么我们要在跳转前检查！
  * 如果发现 App 无效，Bootloader 不跳，而是进入"安全模式"（等按键/等 OTA）。
  *
  * @return true App 有效可跳转，false App 无效
  */
bool app_is_valid(void);

/**
  * @brief  执行跳转 App —— 永不返回
  *
  * 调用前必须先调 app_is_valid() 确认 App 有效！
  * 本函数会：
  *   1. 关闭所有中断（NVIC + SysTick）
  *   2. 复位 Bootloader 使用过的外设
  *   3. 设置 SCB->VTOR = APP_START_ADDR
  *   4. 设置 MSP = App 栈顶
  *   5. 跳转到 App Reset_Handler
  *
  * 跳转后 Bootloader 的代码栈、全局变量都不再被使用，
  * CPU 直接在 App 的栈和代码里运行。
  */
void app_jump(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_JUMP_H */

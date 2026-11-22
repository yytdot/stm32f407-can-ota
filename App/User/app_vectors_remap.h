/**
  ******************************************************************************
  * @file    app_vectors_remap.h
  * @brief   App 启动时设置 SCB->VTOR 重定向向量表
  *
  * 为什么 App 自己还要做这步？
  *   Bootloader 跳转 App 之前已经在 app_jump() 里设置过 SCB->VTOR = APP_START_ADDR
  *   但 App 的 Reset_Handler 启动代码里（startup_stm32f407xx.s），
  *   main() 还没跑之前，可能 VTOR 已经被复位到默认值 0x08000000
  *   （这取决于启动文件的实现方式）。
  *   最安全的做法是：在 App 的 main() 最开头再设一次。
  *
  * 这样即便 Bootloader 那边漏了，App 自己也不会 HardFault。
  *   （这是工程上"双保险"的做法）
  ******************************************************************************
  */

#ifndef __APP_VECTORS_REMAP_H
#define __APP_VECTORS_REMAP_H

#include "../../Common/flash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  设置向量表到 App 区
  *
  * 必须在 HAL_Init() 之前或之后都行，
  * 只要在第一个可能触发中断（HAL 内部可能会触发）的代码之前。
  * 推荐在 main() 最开头，第一行就调。
  */
static inline void app_set_vector_table(void)
{
    /* 强制内存屏障（确保所有内存写操作完成） */
    __DSB();
    __ISB();
    /* 重定向 VTOR */
    SCB->VTOR = APP_START_ADDR;
    __DSB();
    __ISB();
}

#ifdef __cplusplus
}
#endif

#endif /* __APP_VECTORS_REMAP_H */

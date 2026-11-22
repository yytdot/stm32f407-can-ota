/**
  ******************************************************************************
  * @file    app_upgrade_trigger.c
  * @brief   App 侧 OTA 升级触发逻辑实现
  ******************************************************************************
  */

#include "app_upgrade_trigger.h"

/* W25Q64 驱动 —— App 工程里需要把 Bootloader/Drivers/w25q64.c 也 add 进去 */
/* 头文件用相对路径 include Bootloader 侧的 */
#include "../../Bootloader/Drivers/w25q64.h"

void upgrade_trigger_request(uint16_t flag_value)
{
    /* 1. 确保 W25Q64 初始化过了（App main 里最先初始化 W25Q64） */
    /* 如果还没 init，这里再 init 一次（init 函数本身是幂等的——二次调用无害） */
    w25q64_init();

    /* 2. 写升级标记到 W25Q64 参数区
     *    偏移 = W25Q64_PARAM_OFFSET + PARAM_UPGRADE_FLAG_OFFSET
     *    flag_value 是 2 字节 uint16_t，按小端存储
     */
    uint8_t flag_bytes[2] = { (uint8_t)(flag_value & 0xFF), (uint8_t)((flag_value >> 8) & 0xFF) };
    w25q64_write(W25Q64_PARAM_OFFSET + PARAM_UPGRADE_FLAG_OFFSET, flag_bytes, 2);

    /* 3. 稍微延时一下让 W25Q64 写完（虽然 w25q64_write 内部已经等待 BUSY 变 0，
     *    这里再多等一下保险） */
    HAL_Delay(50);

    /* 4. 软复位 —— CPU 会从 Reset_Handler 重新执行，即 Bootloader 入口
     *    NVIC_SystemReset() 就是给 SCB->AIRCR 写复位请求位，
     *    整个芯片（除了备份域和 RTC）全部复位。
     */
    NVIC_SystemReset();

    /* 理论上 NVIC_SystemReset 永不返回，这里加个 while(1) 防 warning */
    while (1) { }
}

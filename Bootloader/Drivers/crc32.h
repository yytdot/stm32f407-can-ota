/**
  ******************************************************************************
  * @file    crc32.h
  * @brief   CRC32 计算函数头文件（多项式 0x04C11DB7，IEEE-802.3）
  ******************************************************************************
  */

#ifndef __CRC32_H
#define __CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  对数据做 CRC32 校验
  * @param  data    数据指针
  * @param  length  数据长度（字节）
  * @param  init    初始值，固件校验时应传 0xFFFFFFFF
  * @return 标准 CRC32 值（已做 final xor 0xFFFFFFFF）
  */
uint32_t crc32_calculate(const uint8_t *data, uint32_t length, uint32_t init);

#ifdef __cplusplus
}
#endif

#endif /* __CRC32_H */

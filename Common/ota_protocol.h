/**
  ******************************************************************************
  * @file    ota_protocol.h
  * @brief   CAN 总线 OTA 固件传输层协议定义 —— Bootloader 和 App 共用
  *
  * 本文件定义了 Bootloader 和上位机（CAN 分析仪 + PC 软件）之间
  * 通过 CAN 总线传输固件时使用的自定义协议。
  *
  * ⚠️ 重要区分：
  *   - 本文件的协议 = CAN 传输层（分包怎么传、ACK 怎么回）
  *   - flash_layout.h 里的 fw_header_t = 固件本身的格式
  *     两者解耦，CAN 协议只负责可靠传输，不关心固件内部结构。
  *
  * 设计原则：
  *   1. 一帧 CAN 数据帧最大 8 字节（标准 CAN 2.0B），所以数据要分包
  *   2. 每包带序号 + CRC，丢包能检测 + 重传
  *   3. 全链路 ACK：发包等 ACK，超时重传（最多 3 次），还不行就放弃
  *   4. 控制帧和数据帧严格区分（避免数据帧首字节刚好是命令字时误触发）
  * ==========================================================================*/

#ifndef __OTA_PROTOCOL_H
#define __OTA_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================================
 * 第一部分：CAN ID 分配
 * ==========================================================================*/

/**
  * @brief  上位机（PC 侧软件）发送 OTA 帧的 CAN ID
  *         Bootloader 侧接收过滤器配置成只收这个 ID + 业务数据 ID
  */
#define OTA_CAN_ID_HOST_TX           0x18F00001UL   /* 扩展帧，29-bit ID */

/** @brief 设备（Bootloader/App）回复 OTA 帧的 CAN ID */
#define OTA_CAN_ID_DEV_TX            0x18F00002UL

/**
  * @brief  App 正常业务数据上传用的 CAN ID
  *         Bootloader 模式下不处理这个 ID 的帧
  */
#define OTA_CAN_ID_APP_DATA          0x0CF00001UL


/* ============================================================================
 * 第二部分：OTA 命令字定义（CAN 帧数据区第一个字节）
 * ============================================================================
 *
 * 所有 OTA 帧（无论控制帧还是数据帧），第一个字节都是命令字。
 * 接收端判断逻辑（非常重要，避免误判）：
 *
 *   if (DLC == 1 && data[0] == CMD_XXX) {
 *       // 这是控制帧
 *   }
 *   if (DLC >= 2 && data[0] == CMD_DATA) {
 *       // 这是数据帧
 *   }
 *
 * 为什么要判断 DLC？
 *   —— 过往项目教训：如果只看第一个字节，假设某数据包的 data[0]
 *      刚好等于 CMD_START (0x01)，就会被误判为控制帧。
 *   —— 控制帧固定 DLC=1（只有一个命令字节），
 *      数据帧 DLC=2~8（命令字 + 序号 + 数据），
 *      这样就能严格区分两种帧。
 * ==========================================================================*/

/** @brief 上位机 → 设备：OTA 握手请求。设备回复 ACK_HANDSHAKE */
#define CMD_HANDSHAKE                0x01

/** @brief 设备 → 上位机：握手应答 */
#define ACK_HANDSHAKE                0x81

/**
  * @brief  上位机 → 设备：发送固件头。
  *         DLC = 8（固件头 20 字节，分 3 发，前 2 发 8 字节，最后 1 发 4 字节）
  *         每发带序号
  */
#define CMD_HEADER                   0x02

/** @brief 设备 → 上位机：固件头接收完成应答（带 magic 校验结果） */
#define ACK_HEADER                   0x82

/**
  * @brief  上位机 → 设备：数据包（OTA 固件正文分包）
  *         DLC = 8（最后一包可能不满 8 字节）
  *         格式见下方 OTA_DATA_FRAME 宏
  */
#define CMD_DATA                     0x03

/**
  * @brief  设备 → 上位机：数据包 ACK
  *         DLC = 1，data[0] = 应答序号（即刚收到的那一包的序号）
  *         上位机收到 ACK 后发下一包；超时未收到则重传
  */
#define ACK_DATA                     0x83

/** @brief 设备 → 上位机：请求重传指定序号的包 */
#define CMD_RETRANSMIT               0x04

/** @brief 上位机 → 设备：固件发送完毕，请求做 CRC 整体校验 */
#define CMD_VERIFY                   0x05

/** @brief 设备 → 上位机：CRC 校验结果（0x00=通过，0xFF=失败） */
#define ACK_VERIFY                   0x85

/** @brief 上位机 → 设备：升级完成，请求重启跳转 App */
#define CMD_REBOOT                   0x06

/** @brief 设备 → 上位机：升级过程中通用错误（如 Flash 写失败、协议错误等） */
#define ACK_ERROR                    0xFE


/* ============================================================================
 * 第三部分：数据帧格式
 * ==========================================================================*/

/**
  * @brief  CAN 数据帧中，一个完整数据包（命令字 + 序号 + 数据 + 包 CRC）的布局
  *
  * CAN 帧数据区（最多 8 字节）内：
  *   [0]     命令字 = CMD_DATA (0x03)
  *   [1]     包序号，1~N（从 1 开始，0 保留给"非数据包"）
  *   [2..6]  数据 payload（最多 5 字节，因为第 7 字节要留给 CRC）
  *   [7]     payload 低 5 字节的 CRC8（多项式 0x07，初值 0x00）
  *
  * 为什么数据区只有 5 字节？
  *   —— 一帧 CAN 2.0B 标准帧数据区 = 8 字节
  *   —— 命令字(1) + 序号(1) + CRC(1) = 3 字节开销
  *   —— 剩余 5 字节 payload
  *   —— 上位机发包前把固件按 5 字节切分，每包一个序号
  *
  * 单包 payload = 5 字节是否太少？
  *   —— 固件头是 20 字节，分 4 发（每发 5 字节）就够了
  *   —— 固件正文 896KB / 5 ≈ 183,500 包... 哦，这确实多了
  *
  * 优化方案（实际项目里会做）：
  *   —— 如果用 CAN FD，数据区 64 字节，payload 就能到 61 字节
  *   —— 但题目里用的是 TJA1050（经典 CAN），所以还是按 5 字节走
  *   —— 面试时说清楚："标准 CAN 每帧 8 字节，有效 payload 5 字节，
  *      固件 896KB 需要约 18 万包，实际传输时 CAN 500kbps 可以做到
  *      约 40 分钟发完，测试固件（100KB）约 4 分钟"
  */

/** @brief 单个数据帧的 payload 字节数（标准 CAN 2.0B） */
#define OTA_PAYLOAD_PER_FRAME        5

/** @brief 一包数据在 CAN 帧中的总长度（命令字+序号+payload+CRC8） = 8 字节 */
#define OTA_FRAME_TOTAL_SIZE         8

/** @brief 重传最大次数 */
#define OTA_MAX_RETRY                3

/**
  * @brief  ACK 等待超时时间（毫秒）
  *         上位机发完一包后，等设备 ACK 的最大时间
  *         超过则重传，超过 OTA_MAX_RETRY 次则放弃整个升级
  */
#define OTA_ACK_TIMEOUT_MS           1000

/** @brief 全局最大包序号（用于序号防溢出校验）—— 2^32 足够 */
#define OTA_MAX_PACKET_SEQ           0xFFFFFFFFUL


/* ============================================================================
 * 第四部分：辅助函数（CRC8 计算）
 * ==========================================================================*/

/**
  * @brief  计算 CAN 数据帧 payload 的 CRC8
  *
  * 使用 CRC-8/IEEE 多项式：x^8 + x^2 + x + 1 = 0x07
  * 初始值：0x00
  *
  * 这个 CRC8 只覆盖 payload 的那 5 个字节，不包含命令字和序号。
  * 为什么？
  *   —— 命令字和序号本身由接收端显式判断（DLC 匹配 + 命令字匹配），
  *      payload 才是需要防篡改的部分。
  *   —— 而且 CRC8 只有 8bit，覆盖 5 字节足够检测传输错误。
  *
  * @param  data    指向 payload 的指针
  * @param  len     payload 长度（最多 OTA_PAYLOAD_PER_FRAME = 5）
  * @return CRC8 值（uint8_t）
  */
static inline uint8_t ota_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}


/* ============================================================================
 * 第五部分：Bootloader 侧 OTA 状态机状态定义
 * ==========================================================================*/

/**
  * @brief  Bootloader OTA 状态机的所有状态
  *
  * 状态机流转：
  *   OTA_STATE_IDLE          ← 上电初始状态
  *      │
  *      │ 收到 CMD_HANDSHAKE
  *      ▼
  *   OTA_STATE_WAIT_HEADER   ← 等上位机发固件头
  *      │
  *      │ 收到完整固件头 + magic 校验通过
  *      ▼
  *   OTA_STATE_RECEIVING     ← 一包一包收数据、写 W25Q64
  *      │
  *      │ 全部数据收完，收到 CMD_VERIFY
  *      ▼
  *   OTA_STATE_VERIFY        ← 读取 W25Q64 完整固件，做整体 CRC32 校验
  *      │
  *      ├── 校验通过 → OTA_STATE_PROGRAM
  *      │                 ← 擦片内 App + W25Q64 → 片内搬运
  *      │                    │
  *      │                    ▼
  *      │              OTA_STATE_DONE
  *      │                    ← 写升级成功标记，跳转新 App
  *      │
  *      └── 校验失败 → OTA_STATE_FAILED
  *                        ← 清 W25Q64 缓存 + 清升级标记
  *                           直接跳转旧 App（回滚）
  *
  *   OTA_STATE_FAILED        ← 任何环节失败都进这里
  *   OTA_STATE_DONE          ← 升级成功
  */
typedef enum {
    OTA_STATE_IDLE        = 0,   /**< 空闲：等待上位机握手，或跳转 App */
    OTA_STATE_WAIT_HEADER = 1,   /**< 已握手，等待固件头 */
    OTA_STATE_RECEIVING   = 2,   /**< 正在接收数据包、写入 W25Q64 */
    OTA_STATE_VERIFY      = 3,   /**< 全部收完，正在做整体 CRC32 校验 */
    OTA_STATE_PROGRAM     = 4,   /**< 校验通过，正在擦写片内 Flash */
    OTA_STATE_DONE        = 5,   /**< 升级完成 */
    OTA_STATE_FAILED      = 6    /**< 升级失败，将回滚到旧 App */
} ota_state_t;


#ifdef __cplusplus
}
#endif

#endif /* __OTA_PROTOCOL_H */

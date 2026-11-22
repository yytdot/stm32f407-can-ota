/**
  ******************************************************************************
  * @file    ota_state_machine.c
  * @brief   Bootloader OTA 状态机完整实现
  *
  * 核心设计思路：
  *   1. ota_state_machine_step() 在主循环里被反复调用，每次尝试从 CAN 缓冲区
  *      取一帧，如果有帧就解析并触发状态流转。
  *   2. 每个状态下只处理"应该出现"的帧类型。
  *      例如 WAIT_HEADER 状态下只收 CMD_HEADER 帧，收别的忽略（但不丢弃，
  *      继续留在缓冲区等后续被处理）。
  *   3. 所有 ACK/重传请求都通过 can_boot_send() 回复上位机。
  * ==========================================================================*/

#include "ota_state_machine.h"
#include "can_boot.h"
#include "w25q64.h"
#include "crc32.h"
#include "flash_internal.h"
#include "app_jump.h"
#include "led.h"
#include <string.h>

/* W25Q64 读写参数需要知道头文件里的 w25q64_write / w25q64_read —— 我们已经 include 了 */


/* ============================================================================
 * 辅助函数（本文件内部使用，static 修饰）
 * ==========================================================================*/

/**
  * @brief  发送一个 1 字节的 CAN 回复帧
  *
  * Bootloader 给上位机发的大部分是 ACK/错误帧，都是 1 字节 DLC。
  * 封装一下避免每次都手动构造 can_frame_t。
  *
  * @param  cmd   要发送的命令字节（ACK_HANDSHAKE / ACK_DATA / ACK_ERROR ...）
  */
static void ota_send_ack(uint8_t cmd)
{
    can_frame_t reply = {0};
    reply.ext_id      = OTA_CAN_ID_DEV_TX;
    reply.is_extended = true;
    reply.dlc         = 1;
    reply.data[0]     = cmd;
    can_boot_send(&reply);
}


/* ============================================================================
 * 对外 API 实现
 * ==========================================================================*/

/**
  * @brief  读 W25Q64 参数区的升级标记
  * @return 0x55AA = 触发升级，0xAA55 = 升级刚成功，0x0000 = 无标记
  */
uint16_t ota_read_upgrade_flag(void)
{
    uint8_t buf[2] = {0};
    /* 从 W25Q64 参数区 + PARAM_UPGRADE_FLAG_OFFSET 读 2 字节 */
    w25q64_read(W25Q64_PARAM_OFFSET + PARAM_UPGRADE_FLAG_OFFSET, buf, 2);
    /* 小端拼接 */
    return (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
}

/**
  * @brief  写升级标记
  * @param  flag  升级标记值
  */
void ota_write_upgrade_flag(uint16_t flag)
{
    uint8_t buf[2] = { (uint8_t)(flag & 0xFF), (uint8_t)((flag >> 8) & 0xFF) };
    w25q64_write(W25Q64_PARAM_OFFSET + PARAM_UPGRADE_FLAG_OFFSET, buf, 2);
}


/**
  * @brief  初始化 OTA 状态机
  */
void ota_state_machine_init(ota_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(ota_ctx_t));
    ctx->state        = OTA_STATE_IDLE;
    ctx->expected_seq = 1;   /* 第一个数据包序号是 1 */
    ctx->header_rx_idx = 0;
    ctx->total_written = 0;
}


/**
  * @brief  OTA 状态机主循环 step —— 每次取一帧 CAN 数据来处理
  *
  * 这是 Bootloader OTA 逻辑的核心入口，main loop 里反复调它。
  */
void ota_state_machine_step(ota_ctx_t *ctx)
{
    can_frame_t frame;

    /* 从环形缓冲区取一帧，没有就直接返回（这一次 step 不处理任何事） */
    if (!can_boot_receive(&frame)) {
        return;
    }

    /* 只处理上位机发给设备的 OTA ID，其他忽略 */
    if (frame.is_extended && frame.ext_id != OTA_CAN_ID_HOST_TX) {
        return;
    }

    /* 根据当前状态 + CAN 帧内容，分别处理 */
    switch (ctx->state) {

    /* =====================================================================
     * 状态 1：IDLE —— 等待上位机发握手
     *   可能收到：CMD_HANDSHAKE (DLC=1)
     *   处理：回复 ACK_HANDSHAKE，进入 WAIT_HEADER 状态
     * ====================================================================*/
    case OTA_STATE_IDLE:
        if (frame.dlc == 1 && frame.data[0] == CMD_HANDSHAKE) {
            ota_send_ack(ACK_HANDSHAKE);
            ctx->state = OTA_STATE_WAIT_HEADER;
            /* 升级指示灯亮，表示进入升级模式 */
            LED_UPGRADE_ON();
        }
        /* 其他帧在 IDLE 状态下一律忽略 */
        break;


    /* =====================================================================
     * 状态 2：WAIT_HEADER —— 等上位机分批发固件头（20 字节）
     *   可能收到：CMD_HEADER (DLC=8，每批最多 5 字节有效数据)
     *   处理：累计收到的字节数，收满 20 字节后解析固件头
     *         检查 magic / fw_size 合法性，合法才进 RECEIVING
     * ====================================================================*/
    case OTA_STATE_WAIT_HEADER:
        if (frame.dlc >= 2 && frame.data[0] == CMD_HEADER) {
            uint8_t batch_len = frame.dlc - 1;   /* 第 0 字节是 CMD_HEADER，剩下的才是固件头数据 */
            uint8_t *header_dst = (uint8_t*)&ctx->fw_header + ctx->header_rx_idx;
            memcpy(header_dst, &frame.data[1], batch_len);
            ctx->header_rx_idx += batch_len;

            /* 收够了 20 字节？ */
            if (ctx->header_rx_idx >= FW_HEADER_SIZE) {

                /* ---- 验证固件头合法性 ----
                 * 检查项：
                 *   1. magic = FW_HEADER_MAGIC (0x4F544131)
                 *   2. fw_size > 0 && fw_size <= APP_SIZE（防止固件过大）
                 */
                if (ctx->fw_header.magic != FW_HEADER_MAGIC ||
                    ctx->fw_header.fw_size == 0 ||
                    ctx->fw_header.fw_size > APP_SIZE) {
                    /* 固件头非法，升级失败 */
                    ota_send_ack(ACK_ERROR);
                    ctx->state = OTA_STATE_FAILED;
                    break;
                }

                /* ---- 擦除 W25Q64 固件缓存区（全新开始接收） ----
                 * 这步必须放在固件头验证通过之后，
                 * 否则固件头是假的我们就白擦一个 2MB 区域（41秒）。
                 */
                w25q64_erase_fw_cache();

                /* 回复 ACK_HEADER，把 magic 也带回去让上位机确认 */
                can_frame_t reply = {0};
                reply.ext_id      = OTA_CAN_ID_DEV_TX;
                reply.is_extended = true;
                reply.dlc         = 4;
                reply.data[0]     = ACK_HEADER;
                /* 把 magic 的最低字节回传（让上位机确认我们收到的头是对的） */
                reply.data[1]     = (uint8_t)(ctx->fw_header.magic & 0xFF);
                can_boot_send(&reply);

                /* 状态切换 */
                ctx->state         = OTA_STATE_RECEIVING;
                ctx->expected_seq  = 1;
                ctx->total_written = 0;

                /* 升级指示灯快闪（在 main loop 里实现） */
            }
        }
        break;


    /* =====================================================================
     * 状态 3：RECEIVING —— 收数据包、写 W25Q64
     *   可能收到：CMD_DATA (DLC=2~8)
     *   每包 payload 最多 5 字节，带序号
     *
     *   处理逻辑：
     *     1. 检查序号是否等于 expected_seq（连续顺序）
     *        —— 如果不对，回复 CMD_RETRANSMIT 请求上位机重发
     *     2. 验证 payload 的 CRC8（和帧尾第 8 字节比对）
     *        —— CRC 不对也重发
     *     3. 写 payload 到 W25Q64
     *     4. 回复 ACK_DATA（带刚收到的序号）
     *     5. expected_seq++
     *     6. 如果 total_written >= fw_header.fw_size → 收完了
     * ====================================================================*/
    case OTA_STATE_RECEIVING:
        if (frame.dlc >= 2 && frame.data[0] == CMD_DATA) {
            uint8_t seq       = frame.data[1];                     /* 包序号 */
            uint8_t pay_len   = frame.dlc - 2;                     /* payload 字节数 */
            uint8_t *payload  = &frame.data[2];                    /* payload 起点 */
            uint8_t crc8_rx   = frame.data[frame.dlc - 1];         /* 帧尾 CRC8 */

            /* 序号不对？请求重发 expected_seq */
            if (seq != ctx->expected_seq) {
                can_frame_t reply = {0};
                reply.ext_id      = OTA_CAN_ID_DEV_TX;
                reply.is_extended = true;
                reply.dlc         = 2;
                reply.data[0]     = CMD_RETRANSMIT;
                reply.data[1]     = (uint8_t)(ctx->expected_seq & 0xFF);
                can_boot_send(&reply);
                break;   /* 不处理这包，等重发 */
            }

            /* CRC8 验证（只覆盖 payload） */
            uint8_t crc8_calc = ota_crc8(payload, pay_len);
            if (crc8_calc != crc8_rx) {
                /* CRC 错了，请求重发 */
                can_frame_t reply = {0};
                reply.ext_id      = OTA_CAN_ID_DEV_TX;
                reply.is_extended = true;
                reply.dlc         = 2;
                reply.data[0]     = CMD_RETRANSMIT;
                reply.data[1]     = seq;
                can_boot_send(&reply);
                break;
            }

            /* ---- 写入 W25Q64 ----
             * 写入地址 = W25Q64_FW_CACHE_OFFSET + total_written
             * 写完后 total_written 前进 pay_len
             * 注意：最后一包可能不满 OTA_PAYLOAD_PER_FRAME (5字节)
             * 但 W25Q64_write 已经内部处理了对齐问题，直接传就行。
             */
            w25q64_write(W25Q64_FW_CACHE_OFFSET + ctx->total_written,
                         payload, pay_len);
            ctx->total_written += pay_len;

            /* 回复 ACK_DATA（带刚收到的序号，上位机据此知道我们收到了） */
            can_frame_t reply = {0};
            reply.ext_id      = OTA_CAN_ID_DEV_TX;
            reply.is_extended = true;
            reply.dlc         = 2;
            reply.data[0]     = ACK_DATA;
            reply.data[1]     = seq;
            can_boot_send(&reply);

            /* 期望下一包的序号前进 */
            ctx->expected_seq++;

            /* 收完所有数据了？ */
            if (ctx->total_written >= ctx->fw_header.fw_size) {
                /* 等上位机发 CMD_VERIFY 再进 VERIFY 状态，
                 * 这里不自动进，保持 RECEIVING 状态等上位机触发 */
            }
        }
        else if (frame.dlc == 1 && frame.data[0] == CMD_VERIFY) {
            /* 上位机主动发 CMD_VERIFY，进入 CRC32 校验 */
            ctx->state = OTA_STATE_VERIFY;
        }
        break;


    /* =====================================================================
     * 状态 4：VERIFY —— 对 W25Q64 里的完整固件做 CRC32 校验
     *   处理：读取 W25Q64 固件缓存区，分块做 CRC32
     *   通过 → PROGRAM 状态；失败 → FAILED 状态
     * ====================================================================*/
    case OTA_STATE_VERIFY:
        /* ---- 分块读 W25Q64 + 计算 CRC32 ----
         * 策略：每 FW_TRANSFER_CHUNK_SIZE (4KB) 读一次，
         * crc32_calculate 支持增量（传入上一次的 CRC 作为 init），
         * 所以不需要把整个固件读到 RAM 里！
         * 这对 896KB 固件来说省了一大块 RAM。
         */
        {
            uint32_t fw_size  = ctx->fw_header.fw_size;
            uint32_t offset   = 0;
            uint32_t crc_init = 0xFFFFFFFF;   /* CRC32 初始值 */

            static uint8_t verify_buf[FW_TRANSFER_CHUNK_SIZE];

            while (offset < fw_size) {
                uint32_t chunk = (fw_size - offset < FW_TRANSFER_CHUNK_SIZE)
                                 ? (fw_size - offset) : FW_TRANSFER_CHUNK_SIZE;

                w25q64_read(W25Q64_FW_CACHE_OFFSET + offset, verify_buf, chunk);
                crc_init = crc32_calculate(verify_buf, chunk, crc_init);

                offset += chunk;
            }

            uint32_t crc_final = crc_init;   /* crc32_calculate 已经做了 final xor */

            /* ---- 对比固件头里的 crc32 ---- */
            if (crc_final == ctx->fw_header.crc32) {
                /* 校验通过，进入 PROGRAM 状态 */
                ota_send_ack(ACK_VERIFY);   /* 回复上位机校验通过 */
                ctx->state = OTA_STATE_PROGRAM;
            } else {
                /* 校验失败，进入 FAILED 状态 */
                ota_send_ack(ACK_ERROR);
                ctx->state = OTA_STATE_FAILED;
            }
        }
        break;


    /* =====================================================================
     * 状态 5：PROGRAM —— 擦片内 App + W25Q64→片内搬运
     *   这步是 Bootloader 最危险的操作（擦除 App 区），
     *   但也是最安全的操作（前面已经通过 CRC32 校验了）。
     *   完成后跳转到 DONE 状态。
     * ====================================================================*/
    case OTA_STATE_PROGRAM:
        /* ---- 1. 擦除片内 App 区 ----
         * flash_erase_app() 函数体在 RAM 中运行（已加 __attribute__((section(".ram_code")))）
         * 规避了擦写 Flash 期间取指阻塞问题。
         * 擦除耗时约 2 秒，期间跑主循环（HAL_Delay + 中断都没事）。
         */
        if (flash_erase_app() != HAL_OK) {
            ota_send_ack(ACK_ERROR);
            ctx->state = OTA_STATE_FAILED;
            break;
        }

        /* ---- 2. 从 W25Q64 搬运到片内 App 区 ----
         * flash_program_firmware 内部已经分块 (4KB) 循环，
         * 内部调用 flash_write（也在 RAM 中运行）。
         *
         * 经验教训（来自过往项目）：
         *   ❌ 不要写死 APP_SIZE 作为拷贝长度
         *   ✅ 用 fw_header.fw_size —— 固件头里才是真实长度
         *   这里 fw_size 已经 <= APP_SIZE（之前 firmware header 校验过）
         */
        if (flash_program_firmware(ctx->fw_header.fw_size) != HAL_OK) {
            ota_send_ack(ACK_ERROR);
            ctx->state = OTA_STATE_FAILED;
            break;
        }

        /* ---- 3. 写升级成功标记 ----
         * 不是必须，但写了以后 App 侧启动时可以检查"这是刚升级的版本"，
         * 做一些升级后处理（比如清除故障记录）。
         */
        ota_write_upgrade_flag(UPGRADE_FLAG_SUCCESS);

        /* ---- 4. 状态切换到 DONE ---- */
        ctx->state = OTA_STATE_DONE;
        break;


    /* =====================================================================
     * 状态 6：DONE —— 升级完成，应该跳转 App 了
     *   收到 CMD_REBOOT 后执行跳转（或直接在主循环里检测 DONE 状态跳转）
     * ====================================================================*/
    case OTA_STATE_DONE:
        /* 上位机发 CMD_REBOOT 就跳转 */
        if (frame.dlc == 1 && frame.data[0] == CMD_REBOOT) {
            ota_send_ack(ACK_HANDSHAKE);   /* 回个任意 ACK（只是让上位机知道我们收到了） */
            /* 跳转前关闭升级指示灯，打开运行指示灯 */
            LED_UPGRADE_OFF();
            LED_RUN_ON();

            /* 先清 W25Q64 里的固件缓存（可选，释放空间给参数区扩展用） */
            w25q64_erase_fw_cache();

            /* 最后写升级标记为 NONE（让 App 下次启动正常跑，别进升级模式） */
            ota_write_upgrade_flag(UPGRADE_FLAG_NONE);

            /* 执行跳转 —— 永不返回 */
            app_jump();
        }
        break;


    /* =====================================================================
     * 状态 7：FAILED —— 升级失败
     *   任何环节失败都进来：固件头非法、序号错乱、CRC8 错、
     *   CRC32 校验失败、Flash 擦写失败...
     *
     *   处理：清 W25Q64 缓存 + 清升级标记 → 尝试跳转旧 App
     *         如果旧 App 也损坏了 → 留在 Bootloader 等按键
     * ====================================================================*/
    case OTA_STATE_FAILED:
        /* 清 W25Q64 固件缓存（让下次升级从干净的状态开始） */
        w25q64_erase_fw_cache();
        /* 清升级标记 */
        ota_write_upgrade_flag(UPGRADE_FLAG_NONE);

        /* 尝试跳转旧 App（回滚） */
        if (app_is_valid()) {
            LED_UPGRADE_OFF();
            LED_RUN_ON();
            app_jump();   /* 跳回旧 App —— 这是"安全回滚"，防砖 */
        }
        /* 如果旧 App 也坏了 → 不跳，main loop 会继续跑
         * 主循环里应该处理 FAILED 状态：LED 快闪，等用户按键
         */
        break;


    default:
        /* 未知状态，当作 FAILED 处理 */
        ctx->state = OTA_STATE_FAILED;
        break;
    }
}

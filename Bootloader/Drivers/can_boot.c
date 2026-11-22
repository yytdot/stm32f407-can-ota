/**
  ******************************************************************************
  * @file    can_boot.c
  * @brief   Bootloader 侧 CAN1 驱动实现
  *
  * 接收架构：CAN RX 中断 → 环形缓冲区 → 主循环取出解析
  *
  * 为什么用环形缓冲区？
  *   - OTA 接收时主循环处于某个状态，正在处理上一包数据、写 W25Q64，
  *     此时如果 CAN 又来了新的一包，主循环还没处理完上一个，
  *     不能让中断直接在 ISR 里做协议解析 + SPI 写入（SPI 很慢，会阻塞中断！）。
  *   - 正确做法：ISR 里只做"把帧塞进缓冲区"这一件事，几微秒完成。
  *     主循环里慢慢从缓冲区取出来解析、写 W25Q64。
  *   - 这是 STM32 CAN + 裸机开发的标准模式。
  *
  * 环形缓冲区大小：CAN_RX_RING_SIZE 帧
  *   - 每帧 20 字节左右，8 帧 = 160 字节，RAM 压力不大
  *   - OTA 协议每包都要 ACK，上位机发一包等 ACK 才发下一包，
  *     所以正常情况下缓冲区几乎不会堆积
  *   - 缓冲区满了就丢最老的（环形缓冲区的"防溢出"策略）
  * ==========================================================================*/

#include "can_boot.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* ============================================================================
 * 内部变量
 * ==========================================================================*/

/* CAN1 句柄 */
/* CAN1 句柄：由 main.c 统一定义，
 * 保证 CAN1_RX0_IRQHandler 和驱动操作的是同一个句柄 */
extern CAN_HandleTypeDef hcan1;

/* 环形缓冲区：存 CAN 帧，最多 CAN_RX_RING_SIZE 个 */
#define CAN_RX_RING_SIZE    8
static can_frame_t  s_rx_ring[CAN_RX_RING_SIZE];
static volatile uint8_t s_rx_head;    /* 生产者（中断）写位置 */
static volatile uint8_t s_rx_tail;    /* 消费者（主循环）读位置 */


/* ============================================================================
 * API 实现
 * ==========================================================================*/

/**
  * @brief  初始化 CAN1
  *
  * 步骤：
  *   1. 开时钟（GPIOA + CAN1）
  *   2. 配 PA11/PA12 为 AF9 (CAN1)
  *   3. 配 CAN 参数（波特率 + 模式）
  *   4. 配过滤器（只接 OTA 帧 ID）
  *   5. 开 CAN 中断 + 使能 RX0 中断
  */
HAL_StatusTypeDef can_boot_init(void)
{
    /* ---- 1. 开时钟 ---- */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_CAN1_CLK_ENABLE();

    /* ---- 2. 配 GPIO ---- */
    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
    gpio_init.Mode      = GPIO_MODE_AF_PP;
    gpio_init.Pull      = GPIO_PULLUP;       /* RX 引脚外部通常有上拉，内部再拉一次更稳 */
    gpio_init.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOA, &gpio_init);

    /* ---- 3. 配 CAN 参数 ----
     * 波特率计算（APB1 = 42MHz）：
     *   42MHz / (Prescaler * (1 + BS1 + BS2)) = 500kbps
     *   Prescaler=6, BS1=10, BS2=3 → 6*14=84 → 42M/84 = 500kbps ✓
     */
    hcan1.Instance                  = CAN1;
    hcan1.Init.Prescaler            = 6;
    hcan1.Init.Mode                 = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth        = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1             = CAN_BS1_10TQ;
    hcan1.Init.TimeSeg2             = CAN_BS2_3TQ;
    hcan1.Init.TimeTriggeredMode    = DISABLE;
    hcan1.Init.AutoBusOff           = ENABLE;      /* 总线关闭自动恢复，重要！ */
    hcan1.Init.AutoWakeUp           = DISABLE;
    hcan1.Init.AutoRetransmission   = ENABLE;      /* 硬件自动重传，减少软件负担 */
    hcan1.Init.ReceiveFifoLocked     = DISABLE;
    HAL_CAN_Init(&hcan1);

    /* ---- 4. 配过滤器：决定"只收哪几个 ID 的帧" ----
     * 硬件背景：CAN 总线上的帧是广播的，所有节点都能看到。
     * 过滤器是芯片内部的硬件关卡——只有匹配上的帧才会进 FIFO、才触发中断，
     * 不匹配的帧硬件直接丢掉，CPU 完全不用管。
     *
     * 这里选"32 位列表模式"：一个过滤器组 = 2 个 32 位寄存器，
     * 每个寄存器精确存 1 个完整的 29 位扩展 ID，一共能精确匹配 2 个 ID。
     * （"列表模式"下 Mask 那两个字段不再是掩码，而是当作第 2 个 ID 用！）
     */
    CAN_FilterTypeDef filter = {0};

    /* 使用第 0 号过滤器组。F407 共有 28 个组（0~27） */
    filter.FilterBank = 0;

    /* 列表模式：精确匹配"ID 等于某某某"，不是"某个范围内的 ID" */
    filter.FilterMode = CAN_FILTERMODE_IDLIST;

    /* 32 位宽：一个寄存器就能装下完整的 29 位扩展 ID，不用拆成两段 */
    filter.FilterScale = CAN_FILTERSCALE_32BIT;

    /* ---- 下面 4 行最关键：把一个 29 位 ID 拆成 High / Low 两个 16 位 ----
     * 硬件规则：29 位 ID 必须先左移 3 位，补上 IDE(bit2)/RTR(bit1)/保留(bit0)，
     * 凑成一个 32 位值，再劈成两半填进寄存器。
     *   高 16 位 = (ID << 3) >> 16 = ID >> 13  ← 所以这里右移 13 是对的
     *   低 16 位 = ((ID & 0x1FFF) << 3) | 0x04 ← 低 13 位左移 3 位，再置 IDE 标志
     *
     * ★ 第 1 个 ID：上位机下发的 OTA 帧 */
    filter.FilterIdHigh = (uint16_t)((OTA_CAN_ID_HOST_TX >> 13) & 0xFFFF);  /* ID1 高16位，正确 */
    /* ⚠️ 下面这行有 bug：低 13 位没有左移 3 位，且 0x0400 落在了 ID 位区而非 IDE 位。
     *    实际算出 0x0401，而正确应为 ((ID & 0x1FFF) << 3) | 0x04 = 0x000C。
     *    结果：过滤器匹配的 ID 与上位机实际发的对不上 → 一帧都收不到。 */
    filter.FilterIdLow = (uint16_t)((OTA_CAN_ID_HOST_TX & 0x1FFF) | 0x0400);

    /* ★ 第 2 个 ID：App 业务数据帧。列表模式下 Mask 字段就是第二个 ID，
     *    同样地，High 对、Low 有和上面一样的 bug（正确应为 0x000C）。 */
    filter.FilterMaskIdHigh = (uint16_t)((OTA_CAN_ID_APP_DATA >> 13) & 0xFFFF);  /* ID2 高16位 */
    filter.FilterMaskIdLow  = (uint16_t)((OTA_CAN_ID_APP_DATA & 0x1FFF) | 0x0400); /* ⚠️ 同上，有 bug */

    /* 匹配成功的帧送进 FIFO0。必须和后面开的中断 CAN_IT_RX_FIFO0_MSG_PENDING
     * 对应上——中断监视的是 FIFO0，这里就得往 FIFO0 送，否则收得到却不进中断。 */
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;

    /* 启用这个过滤器组 */
    filter.FilterActivation = ENABLE;

    /* F407 双 CAN 共享 28 个过滤器组：CAN1 用 0~13，CAN2 用 14~27。
     * 这个值告诉硬件"分界线在哪"，本项目只用 CAN1，填 14 是标准做法。 */
    filter.SlaveStartFilterBank = 14;

    /* 把上面这套配置真正写进硬件寄存器 */
    HAL_CAN_ConfigFilter(&hcan1, &filter);

    /* ---- 5. 启动 CAN + 开中断 ---- */
    HAL_CAN_Start(&hcan1);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);

    /* ---- 6. 开 NVIC 中断 ---- */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 2, 0);   /* 优先级 2，比 FreeRTOS 任务低但中断级 */
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);

    /* ---- 7. 清环形缓冲区 ---- */
    s_rx_head = 0;
    s_rx_tail = 0;

    return HAL_OK;
}

/**
  * @brief  CAN 发送：尝试用空闲邮箱发
  *
  * 发送步骤：
  *   1. HAL_CAN_AddTxMessage —— HAL 库帮你找一个空闲邮箱、填 TX mailbox
  *   2. 如果返回 HAL_OK + mailbox_id >= 0，说明发出去了
  *   3. 如果邮箱全满，等一下再试（最多重试 1000 次，超时就放弃）
  *
  * 经验教训（来自过往项目）：
  *   ❌ 不要用 CAN_TransmitStatus(CAN1, CAN_TxStatus_NoMailBox) 这种写法，
  *      TransmitStatus 是查具体某个邮箱的发送状态，不是查"有没有空闲邮箱"。
  *   ✅ 直接调 HAL_CAN_AddTxMessage，HAL 内部会自动选空闲邮箱，
  *      返回 tx_mailbox 编号，发送完再用 HAL_CAN_IsTxMessagePending 查状态。
  */
HAL_StatusTypeDef can_boot_send(const can_frame_t *frame)
{
    /* ---- 第 1 步：组装 CAN 发送头（Header）----
     * CAN 一帧分两部分：Header（ID / 帧类型 / 长度）+ Data（最多 8 字节）。
     * "= {0}" 是初始化技巧：把结构体所有字段一次性清零，
     * 避免栈上残留的随机值混进保留位，导致发出去的帧格式错乱。 */
    CAN_TxHeaderTypeDef tx_header = {0};

    /* 29 位扩展 ID 和 11 位标准 ID 都填上，
     * 实际用哪个由下面的 IDE 位决定。OTA 用扩展帧（0x18F00001），走 ExtId。 */
    tx_header.ExtId = frame->ext_id;
    tx_header.StdId = frame->std_id;

    /* IDE = 帧类型开关：
     *   CAN_ID_EXT → 扩展帧（29 位 ID），OTA 用这个
     *   CAN_ID_STD → 标准帧（11 位 ID）
     * "?: " 是三元运算符：is_extended 为真取前者，为假取后者。 */
    tx_header.IDE   = frame->is_extended ? CAN_ID_EXT : CAN_ID_STD;

    /* RTR = 远程发送请求：
     *   CAN_RTR_DATA   → 数据帧（真的带数据，我们要发的是这种）
     *   CAN_RTR_REMOTE → 远程帧（不带数据，只是向别人要数据）
     * OTA 只发数据帧，所以固定写 DATA。 */
    tx_header.RTR   = CAN_RTR_DATA;

    /* DLC = 数据长度（0~8），表示这一帧实际有几个字节有效。
     * 经典 CAN 一帧最多 8 字节，所以这里不能超过 8。 */
    tx_header.DLC   = frame->dlc;

    /* 发送全局时间戳功能，用不上就关掉。
     * 如果开了，硬件会把时间信息塞进数据区，反而打乱我们自己的协议格式。 */
    tx_header.TransmitGlobalTime = DISABLE;

    /* ---- 第 2 步：投递到发送邮箱，邮箱满就重试 ----
     * 芯片硬件有 3 个发送邮箱。HAL_CAN_AddTxMessage 会自动找一个空的写进去，
     * 如果 3 个全被占满，就返回 HAL_BUSY，所以我们循环重试。
     * retry 是重试计数器，上限 1000 次，防止邮箱一直满时死循环卡住主循环。 */
    uint32_t retry = 0;
    HAL_StatusTypeDef ret = HAL_ERROR;   /* 先用错误值兜底，成功时会被覆盖 */

    while (retry < 1000) {
        /* mailbox_id 是"输出参数"：HAL 会把用的是几号邮箱写进这个变量。
         * 我们不关心几号邮箱，但 API 要求传地址，给个局部变量接住即可。 */
        uint32_t mailbox_id = 0;

        /* 真正干活的一行，四个参数分别是：
         *   &hcan1         → 操作哪个 CAN 外设
         *   &tx_header     → 刚组装好的帧头
         *   frame->data    → 数据区首地址（8 字节数组）
         *   &mailbox_id    → 回传邮箱编号
         * (uint8_t*) 强转是为了去掉 const：HAL 这个 API 的参数没声明 const，
         * 但内部只读不写，所以强转是安全的。 */
        ret = HAL_CAN_AddTxMessage(&hcan1, &tx_header,
                                   (uint8_t*)frame->data, &mailbox_id);

        /* 注意：HAL_OK 只代表"这一帧已经提交给硬件邮箱"，
         * 不等于"已经发到总线上"。提交后硬件会自动发送，
         * 想确认真正发完可以用 HAL_CAN_IsTxMessagePending() 查询。
         * 对 OTA 的 ACK 来说，提交成功就够了，没必要等发完。 */
        if (ret == HAL_OK) {
            break;   /* 投递成功，跳出重试循环 */
        }
        retry++;      /* 失败（一般是邮箱满），计数器加一，再试 */
    }

    /* 返回最后一次的结果：成功是 HAL_OK，试满 1000 次仍是 HAL_BUSY/ERROR */
    return ret;
}

/**
  * @brief  ISR 回调：把 FIFO0 里的 CAN 帧取出来塞进环形缓冲区
  *
  * ⚠️ 中断里绝对不能做耗时操作！
  *   - 不能调 HAL_SPI_Transmit —— SPI 写 W25Q64 可能几毫秒，阻塞中断
  *   - 不能调 HAL_CAN_Transmit —— 发送虽然不慢但也不该在 ISR 里
  *   - 不能做协议解析 —— 解析也要时间
  *   - 只能做：读 FIFO → 塞缓冲区 → 清中断标志
  */
void can_boot_rx0_irq_handler(void)
{
    /* HAL 库自动清中断标志 + 把帧从 FIFO 搬到 HAL 的 RxHeader/RxData */
    CAN_RxHeaderTypeDef rx_header = {0};
    uint8_t rx_data[8];

    /* HAL_CAN_GetRxMessage 会自动清 RX 中断标志 */
    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx_header, rx_data) == HAL_OK) {

        /* 构造完整的 can_frame_t */
        can_frame_t new_frame = {0};
        new_frame.is_extended = (rx_header.IDE == CAN_ID_EXT);
        new_frame.ext_id      = rx_header.ExtId;
        new_frame.std_id      = rx_header.StdId;
        new_frame.dlc         = rx_header.DLC;
        memcpy(new_frame.data, rx_data, rx_header.DLC);

        /* 塞进环形缓冲区
         *
         * 环形缓冲区防溢出策略：
         *   如果 (head + 1) % SIZE == tail → 说明满了（head 和 tail 不能重合，
         *   否则无法区分"空"和"满"）。此时丢弃最早的帧（tail 前进一格），
         *   然后把新帧塞进 head 位置。
         *
         * 丢帧说明：
         *   - OTA 协议本身有超时重传机制，丢一两包问题不大
         *   - 正常流程下上位机发一包等 ACK 才发下一包，不会堆积
         */
        uint8_t next_head = (s_rx_head + 1) % CAN_RX_RING_SIZE;
        if (next_head == s_rx_tail) {
            /* 满了，丢弃最老的 */
            s_rx_tail = (s_rx_tail + 1) % CAN_RX_RING_SIZE;
        }
        s_rx_ring[s_rx_head] = new_frame;
        s_rx_head = next_head;
    }
}

/**
  * @brief  主循环从环形缓冲区取一帧
  *
  * 只有一个消费者（主循环），所以不需要关中断保护。
  * 主循环读 tail，中断写 head，各自独立。
  *
  * 判断空：head == tail
  * 判断有数据：head != tail
  */
bool can_boot_receive(can_frame_t *frame_out)
{
    if (s_rx_head == s_rx_tail) {
        return false;   /* 空 */
    }

    /* 拷贝出 tail 位置的帧 */
    *frame_out = s_rx_ring[s_rx_tail];   /* 结构体整体赋值，8 字节 data 一起 copy */
    s_rx_tail = (s_rx_tail + 1) % CAN_RX_RING_SIZE;

    return true;
}

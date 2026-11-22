# 基于 STM32F407 的 CAN 总线 OTA 远程升级系统

这是一个我边学边写的嵌入式项目：让一块 STM32F407 开发板可以通过 CAN 总线给自己升级固件。

板子装好之后不用拆外壳、不用插 ST-Link，上位机（电脑或另一块板子）把新固件通过 CAN 发过来，设备先存到外挂的 W25Q64 里，校验没问题再搬到片内 Flash，重启后就是新程序。

整个工程分两个 Keil 工程：

- `MDK-ARM/Bootloader.uvprojx` —— 引导程序，裸机，占片内 Flash 前 128KB
- `MDK-ARM/jianlixiangmu.uvprojx` —— 主程序（App），带 FreeRTOS，从第 128KB 开始

两个工程共用 `Common/` 下的分区表和协议头文件，地址只在一个地方定义，改一处两边都跟着变。

---

## 一、为什么做这个（当初的痛点）

做完才发现，这个项目真正麻烦的不是「把数据写进 Flash」，而是下面这几件事：

1. **现场升级太费劲。** 设备装到机柜里以后，要升级就得拆壳、插下载器、点下载。一台十几分钟，几十台就是一整天。所以想做成：一根 CAN 线接上去就能升级。

2. **F407 的 RAM 只有 192KB，塞不下整包固件。** 一开始想的是「先全收到 RAM，再一次性写 Flash」，但 App 一动就上百 KB，加上栈和堆直接爆了。后来改成先存到外挂 SPI Flash（W25Q64），再分块往片内搬。

3. **擦写 Flash 的时候程序会跑飞。** 代码本身就在 Flash 里，擦除 App 区的时候 CPU 还要从 Flash 取指令，擦除期间取指就会出问题。解决办法是把擦写相关的函数放到 SRAM 里执行（`.ram_code` 段）。

4. **升级一半断电，设备就变砖。** 旧程序已经被擦了，新程序还没写完，是最尴尬的时候。所以用了「先缓存到 W25Q64 → CRC32 校验通过 → 再搬进片内」的顺序，并且 App 区只在真正开始搬运时才擦，搬运之前断电都还能用旧程序。

5. **两个工程的地址必须严丝合缝。** Bootloader 认为 App 在 `0x08020000`，App 自己的 IROM1 起始地址、向量表偏移也得是 `0x08020000`，错一个字节跳过去就是 HardFault。所以我单独写了 `Common/flash_layout.h`，两边都只 include 它，不写死数字。

6. **CAN 上不只要传数据，还要传「控制」。** 握手、固件头、数据、重传、校验、重启，每帧都要能区分，还要有应答，不然发出去不知道对方收没收到。

---

## 二、用到的硬件

| 器件 | 作用 |
|---|---|
| STM32F407ZGT6 开发板 | 主控，168MHz，1MB 片内 Flash |
| W25Q64（SPI Flash，8MB） | 存固件缓存和升级标记 |
| CAN 收发器（TJA1050 / SN65HVD230） | STM32 的 CAN 引脚不能直接上总线，必须经收发器 |
| SSD1306 OLED（I2C） | 显示温湿度、光照、电压和传感器状态 |
| SHT30 / BH1750 | 温湿度 / 光照传感器 |
| 按键 ×1、LED ×2 | 长按 3 秒触发升级；运行灯、升级灯 |
| USB-CAN 或另一块 STM32F103 | 上位机，负责发固件 |

指示灯引脚：运行灯 PC0、升级灯 PC1，低电平点亮（见 `Bootloader/Drivers/led.h`）。

---

## 三、片内 Flash 怎么分的

STM32F407ZGT6 是 1MB Flash，按 Sector 擦除，Sector 0~4 是 16KB/64KB，Sector 5~11 是 128KB。

| 区域 | 起始地址 | 大小 | Sector |
|---|---|---|---|
| Bootloader | 0x08000000 | 128KB | 0 ~ 4 |
| App | 0x08020000 | 896KB | 5 ~ 11 |

W25Q64 那边：

| 区域 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| 固件缓存 | 0x000000 | 2MB | 收到的 App 固件先放这里 |
| 参数区 | 0x200000 | 4KB | 升级标记（0x55AA 触发 / 0xAA55 成功） |

这些宏全部在 `Common/flash_layout.h` 里。

---

## 四、目录结构

```
jianlixiangmu/
├── Common/
│   ├── flash_layout.h        # Flash 分区、固件头结构、升级标记（全工程唯一地址来源）
│   └── ota_protocol.h        # CAN OTA 命令字、CRC8、状态枚举
├── Bootloader/
│   ├── User/
│   │   ├── main.c            # 上电判断：进升级模式 还是 跳 App
│   │   ├── ota_state_machine.c  # 7 状态机
│   │   └── app_jump.c        # 跳转 App（重定向向量表、设 MSP）
│   └── Drivers/
│       ├── w25q64.c/.h       # SPI Flash 驱动（CS 手动控制、忙标志轮询）
│       ├── can_boot.c/.h     # Bootloader 侧 CAN 收发
│       ├── flash_internal.c  # 片内 Flash 擦除 / 写入（跑在 RAM）
│       ├── led.c / key.c
├── App/
│   ├── Core/Src/main.c       # App 入口（CubeMX 生成）
│   ├── User/                 # task_sensor / task_oled / task_can / task_key_led
│   └── Drivers/              # ssd1306 / i2c_sensors / can_app / adc_voltage
├── Drivers/                  # STM32 HAL 库 + CMSIS
└── MDK-ARM/                  # 两个 Keil 工程文件
```

---

## 五、怎么编译和烧录

1. 用 Keil 打开 `MDK-ARM/Bootloader.uvprojx`，编译，烧到 `0x08000000`。
2. 用 Keil 打开 `MDK-ARM/jianlixiangmu.uvprojx`，确认 IROM1 起始地址是 `0x08020000`，编译，烧录。
3. 上电（不按键），应该直接进 App，OLED 亮起来。

> F407 上电先跑 Bootloader，Bootloader 判断「升级标记不是 0x55AA」「按键没按下」「App 有效」三个条件都满足，就跳到 App，人眼几乎看不出停顿。

---

## 六、升级流程（协议）

CAN 用扩展帧，两个 ID 分开方向：

- `0x18F00001`：上位机 → 设备
- `0x18F00002`：设备 → 上位机

| 步骤 | 上位机发 | 设备回 | 设备在干什么 |
|---|---|---|---|
| 1. 触发 | 按键长按 3 秒，或发 `CMD_HANDSHAKE(0x01)`（DLC=1） | `ACK_HANDSHAKE(0x81)` | 写升级标记 0x55AA，软复位进 Bootloader |
| 2. 固件头 | `CMD_HEADER(0x02)` + 20 字节头 | `ACK_HEADER(0x82)` | magic / 长度 / 版本 / CRC32 校验 |
| 3. 数据 | `CMD_DATA(0x03)` + 序号 + 5 字节数据 + CRC8 | `ACK_DATA(0x83)` | 写进 W25Q64 缓存区 |
| 4. 校验 | `CMD_VERIFY(0x05)` | `ACK_VERIFY(0x85)` | 对缓存里整个固件算 CRC32 |
| 5. 搬运 | 自动 | — | 擦 Sector 5~11，4KB 一块搬到片内 |
| 6. 重启 | `CMD_REBOOT(0x06)` | — | 写 0xAA55，跳转新 App |

失败会回 `ACK_ERROR(0xFE)`，状态机进 FAILED，旧 App 不受影响。

> 每帧只能带 5 字节数据，100KB 的固件大概两万帧，加上应答和超时，实际要跑几十秒到一两分钟，不是卡住了。

---

## 七、做完之后的现象

**正常运行时**

- OLED 四行：标题、温湿度、光照+电压、`S:V B:V A:V`（三个传感器是否读成功）
- 运行灯每 100ms 翻转一次
- CAN 每 1 秒广播一帧 `0x0CF00001`，8 字节：温度 float + 光照 uint16 + 电压 uint16 + 帧计数

**长按按键 3 秒**

- 板子复位，OLED 黑屏，运行灯变慢闪，升级灯点亮 —— 已经进 Bootloader 了

**上位机开始发固件**

- 升级灯 2Hz 快闪，上位机能收到一串 ACK
- 校验、搬运阶段升级灯长亮 2~3 秒（在擦 Flash，不响应，正常）
- 最后两个灯全亮，跳转新 App：OLED 重新亮起，CAN 广播恢复

**中途出问题**

- 传数据阶段断电：旧 App 还在，重新上电还进 Bootloader 等重发
- 搬运阶段断电：App 被擦了，上电停在 Bootloader 安全模式，等重新升级（不会变砖，但必须重刷）
- CRC 不对：进 FAILED，运行灯 5Hz 快闪，旧 App 照常启动
- W25Q64 读不到 ID：不跳 App，停在 Bootloader

---

## 八、开发过程中踩的坑

这些在 `docs/开发过程.md` 里写了详细的过程，这里只列结论。

1. **跳转 App 后直接 HardFault** —— 向量表没重定向、MSP 没重设。跳转前必须 `SCB->VTOR = APP_START_ADDR`，再用汇编把 MSP 设成 App 向量表第一个字。
2. **擦 Flash 时程序死机** —— 擦写函数没放 RAM。加了 `.ram_code` 段，并且在启动文件里把这段从 Flash 拷到 SRAM。
3. **`flash_write` 按 4 字节对齐写的时候，最后几个字节写坏** —— 拼 word 时的初值必须是 `0xFFFFFFFF` 而不是 0。Flash 只能 1 变 0，初值写 0 等于把不该动的位清掉了，再也回不来。
4. **Keil 里 F12 跳不到 HAL 函数** —— 勾了 Browse Information 也没用，必须 Rebuild All 才会生成索引。
5. **Bootloader 里 SPI 引脚没人配** —— Bootloader 是独立工程，没有 CubeMX 生成的 `stm32f4xx_hal_msp.c`，也没有 `MX_SPI1_Init()`，所有 GPIO 复用都得自己在 `w25q64_init()` 里写。
6. **CAN 发出去没有应答** —— 少了 120Ω 终端电阻，或者两块板没共地，或者波特率不一致（这里是 500kbps）。
7. **W25Q64 读 ID 一直是 0xFF** —— CS 没有手动拉低，SPI 模式设成了硬件 NSS。

---

## 九、还没做完 / 可以继续改的

- PC 端上位机程序还没写（现在协议是手写帧在调）
- 升级过程中 OLED 没有进度显示，只能看灯
- 断电续传没做，断了要从头发
- App 界面上没有版本号，升完级看不出是不是新的
- 数据帧每帧只有 5 字节，效率偏低，可以改成 6 或 7 字节

---

## 十、说明

这个仓库是我准备简历时整理的，代码有一部分是跟着教程和手册一点点写的，注释写得比较多，方便自己回头看。HAL 库和 CMSIS 是 ST 官方提供的，`Drivers/` 下面那部分是原样保留的，不是我自己写的。

如果这份代码对你有帮助，欢迎参考；有写得不合理的地方也欢迎指出来。

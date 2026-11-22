/**
  ******************************************************************************
  * @file    task_oled.c
  * @brief   OLED 显示任务实现
  *
  * 显示布局（128x64，8x16 字体 → 每页 2 行，共 8 page）：
  *
  *   Page 0-1 (row 0-15):   "Temp: 25.3C" + "Humi: 48.2%"
  *   Page 2-3 (row 16-31):  "Light: 125 lux" + "Volt: 3.30V"
  *   Page 4-5 (row 32-47):  状态行（传感器 OK 标志 + 固件版本号）
  *   Page 6-7 (row 48-63):  装饰线 / 时间戳
  *
  * 任务间数据关系：
  *   OLED 任务和传感器采集任务共享同一个消息队列——
  *   队列里的每帧 sensor_data_t 被第一个 pop 到的任务取走。
  *
  *   但这里有个问题：传感器采集任务 push 一帧，CAN 任务和 OLED 任务
  *   都想 pop，但队列里只有一帧——会导致其中一个丢帧。
  *
  *   解决方案（实际 FreeRTOS 工程里）：
  *   - 方案 A：用两个独立队列（一个给 OLED，一个给 CAN）
  *   - 方案 B：用 FreeRTOS 数据分发（xQueueSendToBack 多队列副本）
  *
  *   为了简化，我们这里用方案 A：main 创建两个相同内容的队列，
  *   传感器采集任务依次 push 到两个队列（或在 main 里创建一个"中间分发"任务）。
  *   代码里 task_sensor_main 已经考虑了这个问题——
  *   实际上我们会改成两个队列。详见 App main.c。
  ******************************************************************************
  */

#include "task_oled.h"
#include "../Drivers/ssd1306.h"
#include "../Drivers/i2c_sensors.h"
#include <stdio.h>
#include <string.h>

/* 消息队列句柄 */
static QueueHandle_t s_oled_queue;

/* 格式化字符串临时缓冲区 —— 栈上分配，32 字节足够一行 */
static char s_line_buf[32];


/**
  * @brief  格式化一个传感器状态字符串
  *
  * 这个函数内部用 snprintf 拼字符串，SSD1306 的 draw_string 需要 char*。
  * 格式：
  *   "T:25.3C H:48.2% L:125 V:3.30"
  */
static void format_sensor_line(const sensor_data_t *data, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "T:%4.1fC H:%4.1f%%", data->temperature_c, data->humidity_rh);
}

static void format_sensor_line2(const sensor_data_t *data, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "L:%u V:%4.2fV", data->light_lux, data->voltage_v);
}


void task_oled_main(void *pvParameters)
{
    s_oled_queue = (QueueHandle_t)pvParameters;

    /* 主循环：阻塞等队列里有数据 */
    while (1) {
        sensor_data_t data;

        /* 阻塞等——没有数据就睡 */
        if (xQueueReceive(s_oled_queue, &data, portMAX_DELAY) == pdTRUE) {

            /* 1. 清影子缓存 */
            ssd1306_clear();

            /* 2. 格式化 + 绘制 */
            ssd1306_draw_string(0, 0, "=== Sensor Node ===");   /* 标题行 page 0 */

            format_sensor_line(&data, s_line_buf, sizeof(s_line_buf));
            ssd1306_draw_string(0, 2, s_line_buf);              /* 温湿度 page 2 */

            format_sensor_line2(&data, s_line_buf, sizeof(s_line_buf));
            ssd1306_draw_string(0, 4, s_line_buf);              /* 光照+电压 page 4 */

            /* 状态行：显示传感器 OK 标志（每个 OK 一个勾） */
            /* bit0=SHT30, bit1=BH1750, bit2=ADC */
            char status_buf[32];
            snprintf(status_buf, sizeof(status_buf),
                     "S:%c B:%c A:%c",
                     (data.sensor_ok & 1) ? 'V' : 'X',
                     (data.sensor_ok & 2) ? 'V' : 'X',
                     (data.sensor_ok & 4) ? 'V' : 'X');
            ssd1306_draw_string(0, 6, status_buf);              /* 状态行 page 6 */

            /* 3. 刷新（把影子缓存推到 SSD1306） */
            ssd1306_update_screen();
        }
    }
}


BaseType_t task_oled_create(QueueHandle_t sensor_queue)
{
    return xTaskCreate(
        task_oled_main,
        "OledTask",
        TASK_OLED_STACK_SIZE,
        (void*)sensor_queue,
        TASK_OLED_PRIORITY,
        NULL
    );
}

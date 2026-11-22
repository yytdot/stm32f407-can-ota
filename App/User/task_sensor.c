/**
  ******************************************************************************
  * @file    task_sensor.c
  * @brief   传感器采集任务实现
  ******************************************************************************
  */

#include "task_sensor.h"
#include "../Drivers/adc_voltage.h"

/* 队列句柄（任务入口函数的 pvParameters 会传进来） */
static QueueHandle_t s_sensor_queue;


void task_sensor_main(void *pvParameters)
{
    /* FreeRTOS 任务入口函数，pvParameters 就是我们传进来的 sensor_queue */
    s_sensor_queue = (QueueHandle_t)pvParameters;

    /* 任务主循环 —— FreeRTOS 任务是无限循环，退出后任务终止 */
    while (1) {
        /* 1. 构造一个 sensor_data_t */
        sensor_data_t data = {0};

        /* 2. 读 SHT30 */
        if (sht30_read(&data.temperature_c, &data.humidity_rh) == HAL_OK) {
            data.sensor_ok |= (1 << 0);   /* bit0 = SHT30 OK */
        }

        /* 3. 读 BH1750 */
        if (bh1750_read(&data.light_lux) == HAL_OK) {
            data.sensor_ok |= (1 << 1);   /* bit1 = BH1750 OK */
        }

        /* 4. 读 ADC 电压 */
        if (adc_voltage_read(&data.voltage_v) == HAL_OK) {
            data.sensor_ok |= (1 << 2);   /* bit2 = ADC OK */
        }

        /* 5. 时间戳 —— FreeRTOS tick（1ms 递增） */
        data.timestamp_ms = xTaskGetTickCount();

        /* 6. push 到消息队列
         *    队列满时阻塞 100ms 等待空位，还满就跳过（丢一帧没关系，下周期再采）
         *    因为我们每 500ms 采一次，队列大小 8，正常不会满
         */
        xQueueSend(s_sensor_queue, &data, pdMS_TO_TICKS(100));

        /* 7. 延时一个采集周期 —— vTaskDelay 让任务让出 CPU，不浪费算力
         *    注意是 vTaskDelay 不是 HAL_Delay —— HAL_Delay 是忙等，
         *    vTaskDelay 会真正让任务进入 Blocked 状态，被 FreeRTOS 调度出去
         */
        vTaskDelay(pdMS_TO_TICKS(TASK_SENSOR_PERIOD_MS));
    }
}


BaseType_t task_sensor_create(QueueHandle_t sensor_queue)
{
    return xTaskCreate(
        task_sensor_main,           /* 入口函数 */
        "SensorTask",                /* 任务名（调试用，FreeRTOS 任务列表里看到） */
        TASK_SENSOR_STACK_SIZE,     /* 栈大小（words） */
        (void*)sensor_queue,        /* 传给 pvParameters 的参数 */
        TASK_SENSOR_PRIORITY,       /* 优先级 */
        NULL                        /* 任务句柄（不需要） */
    );
}

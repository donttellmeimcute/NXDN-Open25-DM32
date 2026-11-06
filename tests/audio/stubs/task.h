/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*fn)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelay(TickType_t);
uint32_t ulTaskNotifyTake(BaseType_t,TickType_t);
void xTaskNotifyGive(TaskHandle_t);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
const char *pcTaskGetName(TaskHandle_t);
uint32_t ulTaskGetRunTimeCounter(TaskHandle_t);

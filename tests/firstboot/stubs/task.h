/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test double only. */
typedef void *TaskHandle_t;
#define taskSCHEDULER_RUNNING 1
int xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t);

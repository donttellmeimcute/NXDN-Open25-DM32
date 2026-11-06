/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUDIO_TEST_FREERTOS_H
#define AUDIO_TEST_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
void audio_test_enter(void);
void audio_test_exit(void);
#define taskENTER_CRITICAL() audio_test_enter()
#define taskEXIT_CRITICAL() audio_test_exit()
#endif

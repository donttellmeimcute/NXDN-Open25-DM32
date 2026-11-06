/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu_profile.h"
#include "config.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned suspended;
void vTaskSuspendAll(void) { assert(!suspended);suspended=1; }
BaseType_t xTaskResumeAll(void) { assert(suspended);suspended=0;return 0; }
void *task_stack_handle(unsigned i) { assert(suspended);return i<2?(void *)(uintptr_t)(i+1):NULL; }
uint32_t hal_micros(void) { assert(suspended);return 1234500; }
const char *pcTaskGetName(TaskHandle_t t) { assert(suspended);return t==(void *)1?"IDLE":"p25-lab"; }
uint32_t ulTaskGetRunTimeCounter(TaskHandle_t t) { assert(suspended);return t==(void *)1?700000:200000; }
int main(void)
{
    char buf[1024]={0};dl_text_t t;dl_text_init(&t,buf,sizeof buf);p25_cpu_report(&t);
#if CONFIG_CPU_PROFILE
    assert(strstr(buf,"cpu.available=1\n") && strstr(buf,"cpu.us=1234500\n"));
    assert(strstr(buf,"cpu.task.IDLE=700000\n") && strstr(buf,"cpu.task.p25-lab=200000\n"));
#else
    assert(!strcmp(buf,"cpu.available=0\n"));
#endif
    assert(!suspended && !t.full);
    puts("CPU accounting: fixed handle table, raw units, scheduler exclusion and profile-off PASS");
}

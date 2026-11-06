/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu_profile.h"
#include "config.h"
#if CONFIG_CPU_PROFILE
#include "hal_time.h"
#include "app/task_stack.h"
#include "FreeRTOS.h"
#include "task.h"
#endif
void p25_cpu_report(dl_text_t *t)
{
    dl_text(t,"cpu.available=%u\n",(unsigned)CONFIG_CPU_PROFILE);
#if CONFIG_CPU_PROFILE
    /* Fixed existing handle table; no heap, stack walk, or large task array.
     * Kernel accounting includes IRQ time in the interrupted task's time. */
    vTaskSuspendAll();
    dl_text(t,"cpu.us=%u\n",(unsigned)hal_micros());
    for(unsigned i=0;i<TASK_STACK_MAX;i++) {
        TaskHandle_t h=task_stack_handle(i);
        if(h)dl_text(t,"cpu.task.%s=%u\n",pcTaskGetName(h),(unsigned)ulTaskGetRunTimeCounter(h));
    }
    (void)xTaskResumeAll();
#endif
}

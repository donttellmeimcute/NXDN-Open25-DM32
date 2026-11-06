/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HOST TEST DOUBLE ONLY. Never used as target/firmware headers. */
#ifndef HOST_NATIVE_FREERTOS_H
#define HOST_NATIVE_FREERTOS_H
void mock_enter(void);
void mock_exit(void);
#define taskENTER_CRITICAL() mock_enter()
#define taskEXIT_CRITICAL() mock_exit()
#endif

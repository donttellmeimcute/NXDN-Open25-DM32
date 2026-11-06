/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HRC_NATIVE_PORT_H
#define HRC_NATIVE_PORT_H
#include "hrc_native_chip.h"
int hn_port_on(hn_chip *, nt_test *);
void hn_port_quiet(void);
void hn_port_restore(void);
#endif

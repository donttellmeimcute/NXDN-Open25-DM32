/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_FIRSTBOOT_H
#define P25_FIRSTBOOT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum firstboot_event {
    FB_BOARD_TX, FB_PA_KEY, FB_RF_SWITCH, FB_PA_GPIO, FB_FLASH_WRITE,
    FB_FLASH_ERASE, FB_NOR_WRITE_ENABLE, FB_FD_TX, FB_STORAGE_CLAIM,
    FB_DEBUG_COMMAND, FB_NATIVE_TX, FB_EVENT_COUNT
};
#define FIRSTBOOT_VERSION 1u
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
#define FIRSTBOOT_FLAGS 47u /* Runtime updater plus bounded native TX; ordinary PTT/write guards remain. */
#elif defined(CONFIG_DEV_UPDATE) && CONFIG_DEV_UPDATE
#define FIRSTBOOT_FLAGS 15u /* Same TX/write guards; approved RAM helper entry enabled. */
#else
#define FIRSTBOOT_FLAGS 31u /* TX, data writes, cable filter, PA GPIO, RAM loader blocked */
#endif
#define FIRSTBOOT_WORDS (2u + FB_EVENT_COUNT)
/* These restrictions are build-time only. There is no enable or counter reset. */
bool firstboot_deny(enum firstboot_event event);
bool firstboot_debug_allowed(uint8_t command);
bool firstboot_reboot_allowed(const uint8_t *,uint16_t);
size_t firstboot_command(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
uint32_t firstboot_irq_save(void);
void firstboot_irq_restore(uint32_t irq);
/* Board implements the protected output pins; platform GPIO applies this last. */
bool board_firstboot_pin_level(uint8_t pin, bool requested);
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
struct nt_config;
struct nt_test;
bool firstboot_native_tx_config(const struct nt_config *);
bool firstboot_native_tx_open(struct nt_test *);
void firstboot_native_tx_close(void);
bool firstboot_tx_allowed(enum firstboot_event);
#else
static inline bool firstboot_tx_allowed(enum firstboot_event event) { (void)event;return false; }
static inline void firstboot_native_tx_close(void) { }
#endif
#if defined(CONFIG_DEV_UPDATE) && CONFIG_DEV_UPDATE
bool firstboot_update_busy(void);
void firstboot_update_cancel(void);
bool firstboot_update_request(const uint8_t *,uint16_t);
bool firstboot_stub_image_ok(const uint8_t *,uint32_t);
#else
static inline bool firstboot_update_busy(void) { return false; }
static inline void firstboot_update_cancel(void) { }
#endif
#endif

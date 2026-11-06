/* SPDX-License-Identifier: GPL-3.0-or-later
 * C7000 RAM probe. Hardware-unverified. Run only after complete stable backups.
 * UART0 address/USR flag verified against pinned community helper at 0x10870.
 * UART configuration is inherited from startup loader. No clock/pinmux changes.
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
_Static_assert(sizeof(u32) == 4 && sizeof(u16) == 2, "C-SKY ABI widths");

static volatile u32 *const uart0 = (volatile u32 *)0x14030000u;
static int putc_bounded(u8 value)
{
    for (u32 wait = 0; wait < 1000000u; ++wait) {
        if (uart0[0x7c / 4] & 2u) {
            uart0[0] = value;
            return 1;
        }
    }
    return 0;
}

static int text(const char *s)
{
    while (*s) if (!putc_bounded((u8)*s++)) return 0;
    return 1;
}

static int hex(u32 value)
{
    const char *digits = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        if (!putc_bounded((u8)digits[(value >> shift) & 15u])) return 0;
    return 1;
}

static u16 crc_selftest(void)
{
    const u8 *s = (const u8 *)"123456789";
    u32 crc = 0;
    while (*s) {
        crc ^= (u32)*s++ << 8;
        for (unsigned int i = 0; i < 8; ++i)
            crc = (crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u);
        crc &= 0xffffu;
    }
    return (u16)crc;
}

void probe_main(void)
{
    volatile union { u32 word; u8 bytes[4]; } endian;
    endian.word = 0x12345678u;
    if (!text("DM32-RAM-PROBE/1\r\ncrc16=")) return;
    if (!hex(crc_selftest()) || !text("\r\nbyte0=")) return;
    if (!hex(endian.bytes[0]) || !text("\r\nwork_mode=")) return;
    /* Documented non-FIFO configuration register. Read only, no state changes. */
    u32 mode = *(volatile u32 *)0x11000100u;
    if (!hex(mode) || !text("\r\nDONE; power-cycle to restore normal boot\r\n")) return;
}

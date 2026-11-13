#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproducible, local GPL-compatible RadioFoundry P25 laboratory derivative.

Never opens a serial port, flashes a radio or modifies the pristine checkout.
Includes bounded native-PHY RF experiments, not an operational P25 voice mode.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import codec_cache
UP = ROOT / "research/upstream/radiofoundry"
PIN = "dfbaf20c400c73e2ccf6edb80af7d1d255bee418"
DEST = ROOT / "build/radiofoundry-p25"
OWNED_FILE = DEST / ".p25-owned.json"
OWNED = json.loads(OWNED_FILE.read_text()) if OWNED_FILE.exists() else {}


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


# Generated content is staged first: transforms, copied sources, then the
# patch series in integration/radiofoundry/patches (applied in name order to
# the staged text, or to the pristine file). flush() writes everything once.
PATCHES = ROOT / "integration/radiofoundry/patches"
STAGE = {}


def write_owned(path, content, original=None):
    key = str(path.relative_to(DEST))
    previous = STAGE.get(key)
    STAGE[key] = (content, previous[1] if previous else original)


def staged(path):
    return STAGE[str(path.relative_to(DEST))][0]


def patch_targets(patch):
    names = []
    for line in patch.read_text().splitlines():
        if line.startswith("+++ "):
            name = line[4:].split("\t")[0]
            if name != "/dev/null":
                names.append(name[2:] if name.startswith("b/") else name)
    return names


def apply_patches(limit=None):
    """Apply the series (names sorted; stop before `limit`) to the staged tree."""
    import tempfile
    for patch in sorted(PATCHES.glob("*.patch")):
        if limit is not None and patch.name >= limit:
            break
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            targets = patch_targets(patch)
            for key in targets:
                base = STAGE[key][0] if key in STAGE else (UP / key).read_bytes() if (UP / key).exists() else None
                if base is not None:
                    (tmp / key).parent.mkdir(parents=True, exist_ok=True)
                    (tmp / key).write_bytes(base)
            result = subprocess.run(["patch", "-p1", "-s", "-f", "--no-backup-if-mismatch", "-d", str(tmp), "-i", str(patch)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise SystemExit(f"Patch does not apply: {patch.name}\n{result.stdout}{result.stderr}")
            for key in targets:
                original = STAGE[key][1] if key in STAGE else (UP / key).read_bytes() if (UP / key).exists() else None
                STAGE[key] = ((tmp / key).read_bytes(), original)


def flush():
    for key, (content, original) in sorted(STAGE.items()):
        _write_file(DEST / key, content, original)


def _write_file(path, content, original=None):
    key = str(path.relative_to(DEST))
    if (path.exists() and path.read_bytes() not in (content, original) and
        hashlib.sha256(path.read_bytes()).hexdigest() != OWNED.get(key)):
        raise SystemExit(f"Preserving unexpected local changes: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != content:
        path.write_bytes(content)
    OWNED[key] = hashlib.sha256(content).hexdigest()


def transform(relative, replacements):
    original = (UP / relative).read_bytes()
    content = original.decode()
    for old, new in replacements:
        if content.count(old) != 1:
            raise SystemExit(f"Pinned patch mismatch: {relative}: {old}")
        content = content.replace(old, new)
    write_owned(DEST / relative, content.encode(), original)


def prepare(write=True, apply_series=True, limit=None):
    if not UP.exists():
        run(["git", "clone", "--no-checkout", "https://github.com/RadioFoundry/RadioFoundry", UP])
        run(["git", "-C", UP, "checkout", "--detach", PIN])
    if git(UP, "rev-parse", "HEAD") != PIN or git(UP, "status", "--porcelain", "--untracked-files=no"):
        raise SystemExit("Refusing a changed pristine RadioFoundry pin")
    if not DEST.exists():
        run(["git", "clone", "--shared", "--no-hardlinks", UP, DEST])
    if git(DEST, "rev-parse", "HEAD") != PIN:
        raise SystemExit("Refusing incompatible derivative HEAD")
    run(["git", "-C", DEST, "submodule", "update", "--init", "third_party/FreeRTOS-Kernel"])
    transform("config/features.mk", [("# Dependencies\n", "CONFIG_P25 ?= n\nCONFIG_P25_NATIVE ?= n\nCONFIG_NXDN ?= n\nCONFIG_P25_TRUNK ?= n\nCONFIG_P25_TRUNK_TX ?= n\nCONFIG_P25_PHASE2 ?= n\nCONFIG_FIRSTBOOT ?= n\nCONFIG_P25_BENCH ?= y\nCONFIG_CPU_PROFILE ?= n\nCONFIG_DEV_UPDATE ?= n\nCONFIG_DEV_TX ?= n\nFEATURES += P25 P25_NATIVE NXDN P25_TRUNK P25_TRUNK_TX P25_PHASE2 FIRSTBOOT P25_BENCH CPU_PROFILE DEV_UPDATE DEV_TX\nifeq ($(CONFIG_P25_TRUNK),y)\nifneq ($(CONFIG_P25_NATIVE),y)\n$(error P25_TRUNK needs P25_NATIVE=y (the control channel and voice ride the P25 path))\nendif\nendif\nifeq ($(CONFIG_P25_TRUNK_TX),y)\nifneq ($(CONFIG_P25_TRUNK),y)\n$(error P25_TRUNK_TX needs P25_TRUNK=y)\nendif\nendif\nifeq ($(CONFIG_P25_PHASE2),y)\nifneq ($(CONFIG_P25_TRUNK),y)\n$(error P25_PHASE2 needs P25_TRUNK=y)\nendif\nendif\nifeq ($(CONFIG_NXDN),y)\nifneq ($(CONFIG_DMR),y)\n$(error NXDN needs DMR=y (the AMBE+2 vocoder and voice bridge))\nendif\nendif\nifeq ($(CONFIG_DEV_TX),y)\nifneq ($(CONFIG_FIRSTBOOT)$(CONFIG_DEV_UPDATE)$(CONFIG_P25_NATIVE),yyy)\n$(error DEV_TX requires FIRSTBOOT=y DEV_UPDATE=y P25_NATIVE=y)\nendif\nendif\nifeq ($(CONFIG_DEV_UPDATE),y)\nifneq ($(CONFIG_FIRSTBOOT)$(CONFIG_STUB_ENTRY),yy)\n$(error DEV_UPDATE requires FIRSTBOOT=y STUB_ENTRY=y)\nendif\nendif\nifeq ($(CONFIG_FIRSTBOOT),y)\nifneq ($(CONFIG_CPS_OPENFW)$(CONFIG_CPS_FAST),nn)\n$(error FIRSTBOOT requires CPS_OPENFW=n CPS_FAST=n)\nendif\nifneq ($(CONFIG_STUB_ENTRY),$(CONFIG_DEV_UPDATE))\n$(error FIRSTBOOT requires STUB_ENTRY to match DEV_UPDATE)\nendif\nendif\nifeq ($(CONFIG_P25_NATIVE),y)\nifneq ($(CONFIG_P25)$(CONFIG_DMR),yy)\n$(error P25_NATIVE needs P25=y and DMR=y)\nendif\nendif\n\n# Dependencies\n")])
    transform("core/config.h", [("#define RFOUNDRY_CONFIG_H\n", "#define RFOUNDRY_CONFIG_H\n#ifndef CONFIG_P25\n#define CONFIG_P25 0\n#endif\n#ifndef CONFIG_P25_NATIVE\n#define CONFIG_P25_NATIVE 0\n#endif\n#ifndef CONFIG_NXDN\n#define CONFIG_NXDN 0\n#endif\n#ifndef CONFIG_P25_TRUNK\n#define CONFIG_P25_TRUNK 0\n#endif\n#ifndef CONFIG_P25_TRUNK_TX\n#define CONFIG_P25_TRUNK_TX 0\n#endif\n#ifndef CONFIG_P25_PHASE2\n#define CONFIG_P25_PHASE2 0\n#endif\n#ifndef CONFIG_P25_BENCH\n#define CONFIG_P25_BENCH 1\n#endif\n#ifndef CONFIG_DEV_UPDATE\n#define CONFIG_DEV_UPDATE 0\n#endif\n#ifndef CONFIG_DEV_TX\n#define CONFIG_DEV_TX 0\n#endif\n#ifndef CONFIG_CPU_PROFILE\n#define CONFIG_CPU_PROFILE 0\n#endif\n#ifndef CONFIG_FIRSTBOOT\n#define CONFIG_FIRSTBOOT 0\n#endif\n#if CONFIG_FIRSTBOOT\n#include \"firstboot/firstboot.h\"\n#endif\n")])
    transform("Makefile", [
        ("APP_DIRS := $(CORE_DIRS)", "ifeq ($(CONFIG_P25),y)\nCORE_DIRS += core/p25\nP25_LIB := core/p25/libdm32_imbe.a\nendif\nifeq ($(CONFIG_P25_NATIVE),y)\nCORE_DIRS += core/native_modem\nendif\nifneq ($(filter y,$(CONFIG_P25_NATIVE) $(CONFIG_NXDN)),)\nCORE_DIRS += core/dv\nendif\nifeq ($(CONFIG_NXDN),y)\nCORE_DIRS += core/nxdn\nendif\nifeq ($(CONFIG_P25_TRUNK),y)\nCORE_DIRS += core/p25t\nendif\nifeq ($(CONFIG_P25_PHASE2),y)\nCORE_DIRS += core/p25p2\nendif\nifeq ($(CONFIG_FIRSTBOOT),y)\nCORE_DIRS += core/firstboot\nendif\nAPP_DIRS := $(CORE_DIRS)"),
        ("INCLUDES := -Ihal", "INCLUDES := -Icore/p25 -Icore/native_modem -Icore/dv -Icore/nxdn -Icore/p25t -Icore/p25p2 -Ihal"),
        ("$(TARGET).elf: $(OBJ) $(LDSCRIPT)", "$(TARGET).elf: $(OBJ) $(P25_LIB) $(LDSCRIPT)"),
        ("-o $@ $(OBJ) -lm", "-o $@ $(OBJ) $(P25_LIB) -lm"),
    ])
    transform("core/app/app.c", [
        ('#include "config.h"', '#include "config.h"\n#if CONFIG_P25\n#include "p25/p25_lab.h"\n#endif'),
        ("\tradio_core_start();", "\tradio_core_start();\n#if CONFIG_P25\n\tp25_lab_start();\n#endif"),
    ])
    transform("core/debuglink/debuglink.c", [
        ('#include "config.h"', '#include "config.h"\n#if CONFIG_P25\n#include "p25/p25_lab.h"\n#endif\n#if CONFIG_P25_NATIVE\n#include "native_modem/native_lab.h"\n#endif'),
        ("\tswitch (cmd) {", "#if CONFIG_FIRSTBOOT\n\tif (!firstboot_debug_allowed(cmd)) return status_only(out, DL_ST_REFUSED);\n\tif (cmd==DL_CMD_REBOOT && !firstboot_reboot_allowed(req,len)) return status_only(out, DL_ST_REFUSED);\n#endif\n\tswitch (cmd) {"),
        ("case DL_CMD_PING:", "case 0x22: /* private derivative: immutable first-boot restrictions */\n#if CONFIG_FIRSTBOOT\n\t\treturn firstboot_command(req, len, out, cap);\n#else\n\t\treturn status_only(out, DL_ST_UNAVAILABLE);\n#endif\n\tcase 0x21: /* private derivative: bounded native PHY experiment */\n#if CONFIG_P25_NATIVE\n\t\treturn native_lab_command(req, len, out, cap);\n#else\n\t\treturn status_only(out, DL_ST_UNAVAILABLE);\n#endif\n\tcase 0x20: /* private derivative: offline P25 laboratory */\n#if CONFIG_P25\n\t\treturn p25_lab_command(req, len, out, cap);\n#else\n\t\treturn status_only(out, DL_ST_UNAVAILABLE);\n#endif\n\tcase DL_CMD_PING:"),
    ])
    transform("core/stubentry/stub_entry.c", [
        ('#include "config.h"', '#include "config.h"\n#if CONFIG_DEV_UPDATE\n#include "native_modem/native_lab.h"\n#include "p25/p25_lab.h"\n#endif'),
        ('\tconst hal_system_t *s = sys();\n\n\tif (s == NULL)', '#if CONFIG_DEV_UPDATE\n\tif (cap < STUB_REPLY_MAX) { out[0]=DL_ST_BAD_ARG;return 1; }\n\tif (!firstboot_update_busy() && (native_lab_reserved() || p25_lab_busy())) { out[0]=DL_ST_BUSY;return 1; }\n\tif (!firstboot_update_request(req,len)) { out[0]=DL_ST_REFUSED;return 1; }\n#endif\n\tconst hal_system_t *s = sys();\n\n\tif (s == NULL)'),
        ('\tif (!ready) {', '#if CONFIG_DEV_UPDATE\n\tif (req && len && req[0]==STUB_OP_BEGIN) { uint32_t size=0;(void)s->stub_region(&size); }\n#endif\n\tif (!ready) {'),
        ('\tif (on_air()) {\t\t\t\t// PTT went down after the ENTER was accepted', '#if CONFIG_DEV_UPDATE\n\tif (!firstboot_update_busy())return;\n#endif\n\tif (on_air()) {\t\t\t\t// PTT went down after the ENTER was accepted'),
    ])
    transform("platform/hrc7000/system.c", [
        ('\t.enter_stub = enter_stub,', '\t.enter_stub = enter_stub,\n#if CONFIG_DEV_UPDATE\n\t.stub_image_ok = firstboot_stub_image_ok,\n#endif'),
    ])
    transform("platform/hrc7000/FreeRTOSConfig.h", [
        ("#define configUSE_STATS_FORMATTING_FUNCTIONS\t0", "#define configUSE_STATS_FORMATTING_FUNCTIONS\t0\n#define configGENERATE_RUN_TIME_STATS CONFIG_CPU_PROFILE\n#if CONFIG_CPU_PROFILE && !defined(__ASSEMBLER__)\nuint32_t hrc_micros(void);\n#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() ((void)0)\n#define portGET_RUN_TIME_COUNTER_VALUE() hrc_micros()\n#endif"),
    ])
    transform("hal/board.h", [("const hal_radio_t *radio;", "const struct nt_ops *native_modem; /* optional derivative native PHY test backend */\n\tvoid (*native_mic_route)(bool on);\n\tconst hal_radio_t *radio;")])
    transform("hal/hal_audio.h", [("bool (*spk_pending)(void);", "bool (*spk_pending)(void);\n\t/* Optional laboratory playback pacing and fixed read-only diagnostics. */\n\tvoid (*playback_pace)(bool on);\n\tvoid (*playback_stats)(uint32_t out[16]);")])
    transform("platform/hrc7000/audio_pcm.c", [
        ('static void gate_check(void);', 'static void gate_check(void);\n#include "pcm_playback.inc"'),
        ('hrc7000_prompt_push(modem, &spk_q[spk_tail][spk_pos])', 'playback_push(&spk_q[spk_tail][spk_pos])'),
        ('\t\t\tspk_tail = (uint8_t)((spk_tail + 1u) % SPK_BLOCKS);\n\t\t}\n\t}', '\t\t\tspk_tail = (uint8_t)((spk_tail + 1u) % SPK_BLOCKS);\n\t\t}\n#if CONFIG_P25\n\t\tif (play_paced) break;\n#endif\n\t}'),
        ('\t\t\tcb(mic, discard, block_ctx);', '#if CONFIG_P25\n\t\t\tplay_diag[8]++;\n#endif\n\t\t\tcb(mic, discard, block_ctx);'),
        ('\t\t\t\tspk_head = next;\n\t\t\t}', '\t\t\t\tspk_head = next;\n\t\t\t}\n#if CONFIG_P25\n\t\t\telse play_diag[9]++;\n#endif'),
        ('\trunning = false;', '\trunning = false;\n#if CONFIG_P25\n\tplay_paced = false;\n#endif'),
        ('\t.spk_pending = hrc_audio_pcm_spk_pending,', '\t.spk_pending = hrc_audio_pcm_spk_pending,\n#if CONFIG_P25\n\t.playback_pace = playback_pace,\n\t.playback_stats = playback_stats,\n#endif'),
    ])
    write_owned(DEST / "platform/hrc7000/pcm_playback.inc", (ROOT / "integration/radiofoundry/pcm_playback.inc").read_bytes())
    transform("boards/dm32/board.c", [(".radio = &dm32_radio,", ".radio = &dm32_radio,\n#if CONFIG_P25_NATIVE\n\t.native_modem = &dm32_native_ops,\n#endif\n#if CONFIG_P25_NATIVE\n\t.native_mic_route = dm32_native_mic_route,\n#endif\n#if CONFIG_NXDN\n\t.audio_tap = &dm32_audio_tap,\n#endif"),
        ('#include "pins.h"', '#include "pins.h"\n#include "dm32_firstboot.inc"')])
    transform("boards/dm32/dm32.h", [("#include <stdbool.h>", "#include <stdbool.h>\n#if CONFIG_P25_NATIVE\n#include \"native_test.h\"\nextern nt_ops dm32_native_ops;\n#endif\n#if CONFIG_P25_NATIVE\nvoid dm32_native_mic_route(bool on);\n#endif\n#if CONFIG_NXDN\n#include \"hal_audio_tap.h\"\nextern const hal_audio_tap_t dm32_audio_tap;\n#endif")])
    transform("boards/dm32/radio_dm32.c", [
        ("static void pa_key(void)\n{", "static void pa_key(void)\n{\n#if CONFIG_FIRSTBOOT\n\tif (!firstboot_tx_allowed(FB_PA_KEY)) { firstboot_deny(FB_PA_KEY); board_rf_safe(); return; }\n#endif"),
        ("static void dmr_rf_switch(bool tx)\n{", "static void dmr_rf_switch(bool tx)\n{\n#if CONFIG_FIRSTBOOT\n\tif (tx && !firstboot_tx_allowed(FB_RF_SWITCH)) { firstboot_deny(FB_RF_SWITCH); board_rf_safe(); return; }\n#endif"),
        ("static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)\n{", "static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)\n{\n#if CONFIG_FIRSTBOOT\n\tif (!firstboot_tx_allowed(FB_BOARD_TX) || hz!=145500000u || bw!=RADIO_BW_12K5 || power!=0 || mode!=RADIO_MODE_DMR) { firstboot_deny(FB_BOARD_TX); board_rf_safe(); return false; }\n#endif"),
        ("static void init(void)", "#if CONFIG_P25_NATIVE\nstatic void native_init(void);\n#endif\nstatic void init(void)"),
        ("hrc_fm_rx_attach(&modem);", "hrc_fm_rx_attach(&modem);\n#if CONFIG_P25_NATIVE\n\tnative_init();\n#endif"),
    ])
    # Append board glue while preserving ownership checks on generated files.
    board_path=DEST / "boards/dm32/radio_dm32.c"
    board_body=staged(board_path)
    write_owned(board_path,board_body+b'\n#include "dm32_native.inc"\n#include "dm32_nxdn.inc"\n',board_body)
    transform("platform/hrc7000/gpio.c", [
        ('#include "platform.h"', '#include "platform.h"\n#include "config.h"'),
        ('\tbank_rmw(hrc_gpio_bank(pin) + GPIO_DR, hrc_gpio_bit(pin), level);', '#if CONFIG_FIRSTBOOT\n\tlevel=board_firstboot_pin_level(pin,level);\n#endif\n\tbank_rmw(hrc_gpio_bank(pin) + GPIO_DR, hrc_gpio_bit(pin), level);')])
    transform("platform/hrc7000/dac.c", [
        ('#include "platform.h"', '#include "platform.h"\n#include "config.h"'),
        ('DAC_PD_CTRL = DAC_A_OFF;', 'DAC_PD_CTRL = DAC_A_OFF | (CONFIG_FIRSTBOOT ? DAC_B_OFF : 0);'),
        ('void hrc_dac_b_power(bool on)\n{', 'void hrc_dac_b_power(bool on)\n{\n#if CONFIG_FIRSTBOOT\n\tif (on && !firstboot_tx_allowed(FB_BOARD_TX))on=false;\n#endif')])
    transform("platform/hrc7000/flash_spi0.c", [
        ('#include "platform.h"', '#include "platform.h"\n#include "config.h"'),
        ('static bool wr(uint32_t addr, const void *buf, uint32_t len)\n{', 'static bool wr(uint32_t addr, const void *buf, uint32_t len)\n{\n#if CONFIG_FIRSTBOOT\n\treturn firstboot_deny(FB_FLASH_WRITE);\n#endif'),
        ('static bool er(uint32_t addr)\n{', 'static bool er(uint32_t addr)\n{\n#if CONFIG_FIRSTBOOT\n\treturn firstboot_deny(FB_FLASH_ERASE);\n#endif')])
    transform("drivers/flash/spi_nor/spi_nor.c", [
        ('#include "spi_nor.h"', '#include "spi_nor.h"\n#include "config.h"'),
        ('static bool write_enable(const spi_nor_t *nor)\n{', 'static bool write_enable(const spi_nor_t *nor)\n{\n#if CONFIG_FIRSTBOOT\n\treturn firstboot_deny(FB_NOR_WRITE_ENABLE);\n#endif')])
    transform("drivers/rf/fd6818/fd6818.c", [
        ('#include "fd6818.h"', '#include "fd6818.h"\n#include "config.h"'),
        ('void fd6818_write(fd6818_t *dev, uint8_t reg, uint16_t val)\n{', 'void fd6818_write(fd6818_t *dev, uint8_t reg, uint16_t val)\n{\n#if CONFIG_FIRSTBOOT\n\tif (reg==FD6818_REG_STATE && val==FD6818_STATE_TX && !firstboot_tx_allowed(FB_FD_TX)) { firstboot_deny(FB_FD_TX); return; }\n#endif')])
    transform("core/storage/storage.c", [
        ('#include <string.h>', '#include <string.h>\n#include "config.h"'),
        ('bool storage_claim_request(void)\n{', 'bool storage_claim_request(void)\n{\n#if CONFIG_FIRSTBOOT\n\treturn firstboot_deny(FB_STORAGE_CLAIM);\n#endif'),
        ('bool storage_claim(void)\n{', 'bool storage_claim(void)\n{\n#if CONFIG_FIRSTBOOT\n\treturn firstboot_deny(FB_STORAGE_CLAIM);\n#endif')])
    transform("platform/hrc7000/common.ld", [
        ("*(EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o) .text EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o) .text.*)",
         "*(EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o *native_test.o *hrc_native_chip.o *hrc_native_port.o) .text EXCLUDE_FILE(*hrc7000_dmr.o *hrc7000_bs.o *dmr_glue.o *native_test.o *hrc_native_chip.o *hrc_native_port.o) .text.*)"),
        ("*dmr_glue.o(.text .text.*)", "*dmr_glue.o(.text .text.*)\n\t\t*native_test.o(.text .text.*)\n\t\t*hrc_native_chip.o(.text .text.*)\n\t\t*hrc_native_port.o(.text .text.*)"),
    ])
    transform("core/input/input_task.c", [
        ('static void input_task(void *arg)', '#if CONFIG_DEV_TX\nstatic const keymap_entry_t *key_for(uint8_t action, bool long_action);\nstatic volatile bool physical_ptt;\nbool input_physical_ptt(void) { return physical_ptt; }\n#endif\n\nstatic void input_task(void *arg)'),
        ('\t\traw_keys = keys;', '\t\traw_keys = keys;\n#if CONFIG_DEV_TX\n\t\tconst keymap_entry_t *ptt = keys && !ptt_blocked ? key_for(ACT_PTT, false) : NULL;\n\t\tbool physical_down = ptt && (keys & (1u << ptt->bit));\n\t\tif (physical_down != physical_ptt) {\n\t\t\tphysical_ptt = physical_down;\n\t\t\tradio_req_physical_ptt(physical_down);\n\t\t}\n#endif'),
    ])
    transform("core/input/input.h", [("uint32_t input_raw_keys(void);", "uint32_t input_raw_keys(void);\n#if CONFIG_DEV_TX\nbool input_physical_ptt(void);\n#endif")])
    transform("core/radio/radio.h", [("void radio_req_ptt(bool down);", "void radio_req_ptt(bool down);\n#if CONFIG_DEV_TX\nvoid radio_req_physical_ptt(bool down);\n#endif")])
    transform("core/radio/radio_task.c", [
        ('#include "config.h"', '#include "config.h"\n#if CONFIG_P25_NATIVE\n#include "native_modem/native_lab.h"\n#endif'),
        ("for (;;) {\n\t\tTickType_t due", "for (;;) {\n#if CONFIG_P25_NATIVE\n\t\tnative_lab_service(&sm,hal_micros());\n#endif\n\t\tTickType_t due"),
        ("radio_req_t req;\n\t\tif (xQueueReceive", "radio_req_t req;\n#if CONFIG_P25_NATIVE\n\t\tint32_t native_wait=(int32_t)native_lab_wait_ms();\n\t\tif (native_wait && left>native_wait) left=native_wait;\n#endif\n\t\tif (xQueueReceive"),
        ("req_handled++;", "req_handled++;\n#if CONFIG_P25_NATIVE\n\t\t\t\tif (native_lab_request(&sm,&req)) continue;\n#endif"),
        ("TickType_t now = xTaskGetTickCount();", "TickType_t now = xTaskGetTickCount();\n#if CONFIG_P25_NATIVE\n\t\tif (native_lab_owns()) { next_tick=now+tick_period; next_poll=now+period; publish(); continue; }\n#endif"),
        ("radio_sm_init(&sm, hal_millis());", "radio_sm_init(&sm, hal_millis());\n#if CONFIG_P25_NATIVE\n\tnative_lab_init();\n#endif"),
        ("void radio_req_ptt(bool down)", "#if CONFIG_DEV_TX\nvoid radio_req_physical_ptt(bool down)\n{ post(RREQ_PTT, down ? NATIVE_PHYSICAL_PTT_DOWN : NATIVE_PHYSICAL_PTT_UP, pdMS_TO_TICKS(50)); }\n#endif\nvoid radio_req_ptt(bool down)"),
    ])
    # Upstream intentionally bounds the CPS version to 15 bytes in a zeroed
    # buffer. memcpy makes that deliberate truncation explicit for long tags.
    transform("core/cps/cps_openfw.c", [
        ('strncpy((char *)&p[8], RFOUNDRY_GIT, 15);',
         'memcpy(&p[8], RFOUNDRY_GIT, sizeof(RFOUNDRY_GIT) - 1 < 15 ? sizeof(RFOUNDRY_GIT) - 1 : 15);'),
    ])
    # Reserve the worker's actual stack: 8 KiB bench / 4 KiB receive-only.
    # Keep the AMBE code/table windows and full-resolution framebuffer intact.
    transform("boards/dm32/freertos_board.h", [("#define configTOTAL_HEAP_SIZE\t(32 * 1024 + 512)",
        "#if CONFIG_P25 && (CONFIG_P25_BENCH || CONFIG_P25_NATIVE)\n#define configTOTAL_HEAP_SIZE (40 * 1024 + 512)\n#elif CONFIG_P25\n#define configTOTAL_HEAP_SIZE (36 * 1024 + 512)\n#else\n#define configTOTAL_HEAP_SIZE (32 * 1024 + 512)\n#endif")])
    for p in sorted((ROOT / "src/p25").glob("*")):
        if p.suffix in (".c", ".h", ".inc"):
            write_owned(DEST / "core/p25" / p.name, p.read_bytes())
    write_owned(DEST / "core/p25/dm32_imbe.h", (ROOT / "src/codec/dm32_imbe.h").read_bytes())
    for p in sorted((ROOT / "integration/radiofoundry").glob("*")):
        if p.suffix in (".c", ".h"):
            # the digital voice channel interface serves P25 and NXDN builds alike
            write_owned(DEST / ("core/dv" if p.stem == "radio_dv" else "core/p25") / p.name, p.read_bytes())
    for p in sorted((ROOT / "src/nxdn").glob("*")):
        if p.suffix in (".c", ".h", ".inc"):
            write_owned(DEST / "core/nxdn" / p.name, p.read_bytes())
    for p in sorted((ROOT / "integration/radiofoundry/nxdn").glob("*")):
        folder = "boards/dm32" if p.suffix == ".inc" else "core/dv"
        write_owned(DEST / folder / p.name, p.read_bytes())
    for sub in ("p25t", "p25p2"):
        for p in sorted((ROOT / "src" / sub).glob("*")):
            if p.suffix in (".c", ".h", ".inc"):
                write_owned(DEST / "core" / sub / p.name, p.read_bytes())
    for p in sorted((ROOT / "integration/radiofoundry/p25t").glob("*")):
        write_owned(DEST / "core/dv" / p.name, p.read_bytes())
    for p in sorted((ROOT / "src/modem").glob("*")):
        if p.suffix in (".c", ".h"): write_owned(DEST / "core/native_modem" / p.name,p.read_bytes())
    for p in sorted((ROOT / "integration/radiofoundry/native").glob("*")):
        folder="boards/dm32" if p.suffix==".inc" else "platform/hrc7000" if p.name.startswith("hrc_") else "core/native_modem"
        write_owned(DEST / folder / p.name,p.read_bytes())
    for p in sorted((ROOT / "integration/radiofoundry/firstboot").glob("*")):
        folder="boards/dm32" if p.suffix==".inc" else "platform/hrc7000" if p.name=="firstboot_irq.c" else "core/firstboot"
        write_owned(DEST / folder / p.name,p.read_bytes())
    write_owned(DEST / "COPYING-P25", (ROOT / "LICENSE").read_bytes())
    write_owned(DEST / "P25-DERIVATIVE.txt", (
        "User-authorized GPL-compatible P25 development derivative.\n"
        "Original RadioFoundry files remain MIT with their notices. Added P25/IMBE\n"
        "files retain GPL-3.0-or-later; combined laboratory distribution is GPLv3+.\n"
        "Not an upstream MIT-only contribution. See COPYING-P25 and source manifest.\n"
        "Hardware-unverified. Bounded native-PHY tests; no operational P25 voice mode.\n"
    ).encode())
    if apply_series:
        apply_patches(limit)
    if not write:
        return
    flush()
    OWNED_FILE.write_text(json.dumps(OWNED, indent=2) + "\n")
    changed=subprocess.check_output(["git","-C",str(DEST),"diff","--name-only","-z","HEAD"]).decode().split("\0")
    extra=subprocess.check_output(["git","-C",str(DEST),"ls-files","--others","--exclude-standard","-z"]).decode().split("\0")
    for key in filter(None,changed+extra):
        path=DEST/key
        if key in changed or path.suffix in (".c",".h",".inc",".ld",".mk"):
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=OWNED.get(key):
                raise SystemExit(f"Preserving unreviewed derivative input: {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--prepare-only", action="store_true")
    ap.add_argument("--first-boot", action="store_true", help="immutable TX/write inhibits; omit RAM loader and CPS")
    ap.add_argument("--disable", action="store_true", help="build same derivative with CONFIG_P25=n")
    ap.add_argument("--disable-native", action="store_true", help="keep offline P25 lab, omit native modem interface")
    ap.add_argument("--stack-check", action="store_true", help="include external codec call graphs in static stack report")
    ap.add_argument("--with-local-stock", action="store_true", help="merge recognized local stock AMBE via upstream tool; no redistribution or flashing")
    ap.add_argument("--verify-rebuild", action="store_true", help="force recompilation and require identical application bytes")
    ap.add_argument("--speed-codec", action="store_true", help="O3 unity fixed-point codec; same DSP with cross-file inlining")
    ap.add_argument("--lean", action="store_true", help="omit replay, synthetic self-test and per-sample diagnostics")
    ap.add_argument("--cpu-profile", action="store_true", help="optional RTOS task runtime counters; excluded by default")
    ap.add_argument("--dev-update", action="store_true", help="approved RAM-helper cable entry; TX/write inhibits remain")
    ap.add_argument("--reuse-codec", action="store_true", help="reuse codec only when inputs/compiler/archive/call graphs match checked hashes")
    ap.add_argument("--tx-bench",action="store_true",help="bounded native TX on 145.500 MHz; retains ordinary PTT/storage guards")
    ap.add_argument("--no-nxdn", action="store_true", help="build with CONFIG_NXDN=n")
    ap.add_argument("--no-trunk", action="store_true", help="build with CONFIG_P25_TRUNK=n (and no Phase 2)")
    ap.add_argument("--trunk-tx", action="store_true",
                    help="CONFIG_P25_TRUNK_TX=y: trunked registration, affiliation and calls (off by default; "
                         "docs/p25-trunking.md: only on a system you are authorized on)")
    ap.add_argument("--no-phase2", action="store_true", help="build with CONFIG_P25_PHASE2=n")
    args = ap.parse_args()
    if args.trunk_tx and args.no_trunk: ap.error("--trunk-tx needs trunking")
    if args.tx_bench and not (args.first_boot and args.dev_update):ap.error("tx-bench requires first-boot and dev-update")
    if args.dev_update and not args.first_boot:ap.error("dev-update needs first-boot restrictions")
    if args.disable and (args.lean or args.cpu_profile):
        ap.error("lean/CPU measurement require P25 enabled")
    if args.first_boot and (args.disable or args.disable_native or args.with_local_stock):
        ap.error("--first-boot requires native P25 and separately packaged exact-unit codec")
    prepare()
    if args.prepare_only:
        return
    if not (ROOT / "toolchain/csky-elf/lib/libc.a").exists():
        run(["sh", ROOT / "tools/bootstrap_newlib.sh"], cwd=ROOT)
    codec = ROOT / ("build/radiofoundry-codec-speed" if args.speed_codec else "build/radiofoundry-codec")
    reused_codec=False
    if not args.disable:
        cache_key=codec_cache.key(ROOT,args.speed_codec)
        reused_codec=args.reuse_codec and codec_cache.reusable(codec,cache_key)
        if not reused_codec:
            run(["python3", ROOT / "experiments/codec/build.py", "--target", "--out", codec,
                 *(["--speed"] if args.speed_codec else [])], cwd=ROOT)
            codec_cache.remember(codec,cache_key)
        source=codec/'libdm32_imbe.a';destination=DEST/'core/p25/libdm32_imbe.a'
        if not destination.exists() or destination.read_bytes()!=source.read_bytes():shutil.copyfile(source,destination)
        print('Codec cache: '+('verified reuse' if reused_codec else 'rebuilt and hashed'),flush=True)
    native=not args.disable and not args.disable_native
    suffix = "firstboot" if args.first_boot else "off" if args.disable else "native" if native else "lab"
    identity=suffix
    suffix += ("-lean" if args.lean else "") + ("-cpu" if args.cpu_profile else "") + ("-dev" if args.dev_update else "") + ("-tx" if args.tx_bench else "") + ("-nonxdn" if args.no_nxdn else "") + \
        ("-notrunk" if native and args.no_trunk else "-trunktx" if native and args.trunk_tx else "") + \
        ("-nop2" if native and args.no_phase2 and not args.no_trunk else "")
    b = f"build/dm32-p25-{suffix}"
    command = ["make", "-j8", "BOARD=dm32", f"CROSS={ROOT}/toolchain/bin/csky-elf-", f"BUILD={b}",
        "CONFIG_P25=" + ("n" if args.disable else "y"), "CONFIG_P25_NATIVE="+("y" if native else "n"), "CONFIG_DMR_IP_GATEWAY=n", "CONFIG_NET_PPP=n",
        "CONFIG_NXDN=" + ("n" if args.no_nxdn else "y"),
        "CONFIG_P25_TRUNK=" + ("y" if native and not args.no_trunk else "n"),
        "CONFIG_P25_TRUNK_TX=" + ("y" if native and args.trunk_tx and not args.no_trunk else "n"),
        "CONFIG_P25_PHASE2=" + ("y" if native and not args.no_trunk and not args.no_phase2 else "n"),
        "CONFIG_APRS=n", "CONFIG_SFR=n", "CONFIG_HOTSPOT=n", "CONFIG_ZONE_SHARE=n",
        f"RFOUNDRY_VERSION=dfbaf20c-p25-{identity}", f"RFOUNDRY_GIT_TIME={git(UP, 'show', '-s', '--format=%ct', 'HEAD')}",
        "RFOUNDRY_GIT_DIRTY=1",
        "CONFIG_FIRSTBOOT="+("y" if args.first_boot else "n"),
        "CONFIG_P25_BENCH="+("n" if args.lean else "y"),
        "CONFIG_CPU_PROFILE="+("y" if args.cpu_profile else "n"),
        "CONFIG_DEV_UPDATE="+("y" if args.dev_update else "n"),
        "CONFIG_DEV_TX="+("y" if args.tx_bench else "n"),
        f"OPT=-Os -ffile-prefix-map={DEST}=/radiofoundry -fdebug-prefix-map={DEST}=/radiofoundry "
        f"-DRFOUNDRY_SOURCE_EPOCH={git(UP, 'show', '-s', '--format=%ct', 'HEAD')}"]
    if args.first_boot:
        command += ["CONFIG_STUB_ENTRY="+("y" if args.dev_update else "n"), "CONFIG_CPS_OPENFW=n", "CONFIG_CPS_FAST=n", "CONFIG_AUDIO_PROBE=n"]
    log = ROOT / f"logs/radiofoundry-p25-{suffix}.log"
    log.parent.mkdir(exist_ok=True)
    with log.open("w") as f:
        run(command, cwd=DEST, stdout=f, stderr=subprocess.STDOUT,
            env={**os.environ, "SOURCE_DATE_EPOCH": git(UP, "show", "-s", "--format=%ct", "HEAD")})
    # Full build command/output is retained. Reject compiler warnings too.
    if "warning:" in log.read_text():
        raise SystemExit(f"Build has warnings; review {log}")
    if args.stack_check:
        if not args.disable:
            ci = DEST / b / "p25-codec-ci"
            ci.mkdir(exist_ok=True)
            for p in ci.glob("*.ci"):p.unlink()
            for p in codec.glob("*.ci"): shutil.copyfile(p, ci / p.name)
        stack_log = ROOT / f"logs/radiofoundry-stack-{suffix}.log"
        stack_args=[]
        extra=""
        if not args.no_nxdn:
            # The board's flat FM paths live in the included dm32_nxdn.inc, which the scanner doesn't follow; its timer
            # interrupt runs on the 2048-byte IRQ stack
            extra+="\n# NXDN: the board's audio tap (boards/dm32/dm32_nxdn.inc) and its timer interrupt\n"
            extra+="ptr fm_rx_flat boards/dm32/radio_dm32.c:flat_rx\nptr fm_tx_inject boards/dm32/radio_dm32.c:flat_tx_inject\n"
            extra+="ptr fm_rx_read hrc_fm_rx_read\ntask boards/dm32/radio_dm32.c:flat_tick 512\n"
            extra+="ptr flat_pull core/dv/radio_nxdn.c:pull\n"
        if native:
            extra+="\n# Native interrupt entries use the separate 2048-byte IRQ stack.\n"
            extra+="".join(f"task platform/hrc7000/hrc_native_port.c:{fn} 512\n" for fn in ("sys","ts_tx","ts_rx","rftx","guard","pa"))
            # The source scanner does not follow the included board .inc.
            # Supply its actual function-pointer targets for conservative depth.
            for caller,targets in {"start":("native_prepare","native_irq_on"),
                "quiet":("native_off","native_irq_quiet"),"key":("native_key",),
                "pa":("native_pa",),"restore":("native_irq_restore","native_restore"),
                "lock":("native_lock",),"unlock":("native_unlock",),
                "native_lab_command":("snapshot",),
                "p25_voice_rx_dibit":("core/p25/p25_lab.c:sink","core/p25/selftest.c:sink"),
                "audio_task":("core/p25/p25_lab.c:audio_block",),
                "start_audio":("playback_pace",),
                "p25_lab_command":("playback_stats",)}.items():
                extra+="call "+caller+" "+" ".join(targets)+"\n"
            if not args.no_trunk:
                # P25 trunking: the control channel decoder rides the P25 worker's receive stream
                extra+="call p25_phy_rx_record core/dv/radio_p25t.c:tap\n"
                extra+="call p25t_rx_dibit core/dv/radio_p25t.c:on_tsbk core/dv/radio_p25t.c:on_status\n"
        if extra:
            edges=DEST / b / "native-stack-edges.txt"
            edges.write_text((UP / "tools/stack_check_edges.txt").read_text()+extra)
            stack_args=[f"STACK_CHECK_ARGS=--edges {edges}"]
        with stack_log.open("w") as f:
            run([*command, *stack_args, "stack-check"], cwd=DEST, stdout=f, stderr=subprocess.STDOUT,
                env={**os.environ, "SOURCE_DATE_EPOCH": git(UP, "show", "-s", "--format=%ct", "HEAD")})
    prefix = DEST / b / "rfoundry_dm32"
    if args.verify_rebuild:
        before = prefix.with_suffix(".bin").read_bytes()
        rebuild_log=ROOT / f"logs/radiofoundry-rebuild-{suffix}.log"
        with rebuild_log.open("w") as f:
            run([*command, "-B"], cwd=DEST, stdout=f, stderr=subprocess.STDOUT,
                env={**os.environ, "SOURCE_DATE_EPOCH": git(UP, "show", "-s", "--format=%ct", "HEAD")})
        if "warning:" in rebuild_log.read_text():
            raise SystemExit(f"Forced rebuild has warnings; review {rebuild_log}")
        if prefix.with_suffix(".bin").read_bytes() != before:
            raise SystemExit("Application bytes differ on forced rebuild")
    unresolved = subprocess.check_output([str(ROOT / "toolchain/bin/csky-elf-nm"), "-u", str(prefix.with_suffix(".elf"))], text=True)
    disassembly = subprocess.check_output([str(ROOT / "toolchain/bin/csky-elf-objdump"), "-d", str(prefix.with_suffix(".elf"))], text=True)
    if unresolved.strip() or re.search(r"\br(?:1[6-9]|2[0-9]|3[01])\b", disassembly):
        raise SystemExit("ELF failed unresolved-symbol/high-register audit")
    firstboot_audits={}
    if args.first_boot:
        symbols=subprocess.check_output([str(ROOT/"toolchain/bin/csky-elf-nm"),str(prefix.with_suffix(".elf"))],text=True)
        names={line.split()[-1] for line in symbols.splitlines() if line.split()}
        forbidden={"spi_nor_program","spi_nor_erase_sector","write_enable","stub_entry_request",
                   "stub_entry_service","enter_stub","stub_region","cps_fast_cmd","cps_openfw_feed"}
        if args.dev_update:forbidden-={"stub_entry_request","stub_entry_service","enter_stub","stub_region"}
        if names & forbidden:raise SystemExit("First-boot image retained a forbidden write/loader symbol")
        objdump=str(ROOT/"toolchain/bin/csky-elf-objdump")
        tx=subprocess.check_output([objdump,"-dr","-j",".text.tx",str(DEST/b/"boards/dm32/radio_dm32.o")],text=True)
        calls=set(re.findall(r"R_CKCORE_\S+\s+(\w+)",tx))
        expected_calls={"firstboot_deny","board_rf_safe"}
        if args.tx_bench:
            if not {"firstboot_tx_allowed","firstboot_deny","board_rf_safe"}.issubset(calls):
                raise SystemExit("Native bench board TX entry lost its conditional grant guard")
        elif calls!=expected_calls or len(re.findall(r"movi\s+r0, 0",tx))!=2:
            raise SystemExit("First-boot board TX entry differs from the deny/off/false sequence")
        gpio=subprocess.check_output([objdump,"-r",str(DEST/b/"platform/hrc7000/gpio.o")],text=True)
        if "board_firstboot_pin_level" not in gpio:raise SystemExit("Missing last-stage PA GPIO guard")
        firstboot_audits={"forbidden_storage_write_symbols_absent":True,"runtime_loader_enabled":args.dev_update,
                          "forbidden_write_and_loader_symbols_absent":not args.dev_update,
                          "board_tx_only_deny_off_false":not args.tx_bench,"native_tx_scoped_grant":args.tx_bench,"gpio_guard_referenced":True}
    if args.with_local_stock:
        run(["python3", DEST / "tools/codec_auto.py", "--board", "dm32", "--image", prefix.with_suffix(".bin"),
             "--dir", ROOT / "research/originals", "-o", prefix.with_suffix(".codec.bin")], cwd=DEST)
        if not prefix.with_suffix(".codec.bin").exists():
            raise SystemExit("No recognized local stock codec; not claiming preserved DMR voice")
    inputs = [ROOT / "tools/build_radiofoundry.py", ROOT / "tools/codec_cache.py"] + [p for folder in (ROOT / "src/p25", ROOT / "src/modem", ROOT / "src/codec", ROOT / "integration/radiofoundry")
              for p in folder.rglob("*") if p.is_file()]
    manifest = {"kind": "hardware-unverified RadioFoundry laboratory image; bounded native PHY tests, no operational P25 voice mode",
        "upstream_commit": PIN, "freertos_commit": git(DEST / "third_party/FreeRTOS-Kernel", "rev-parse", "HEAD"),
        "command": [str(x) for x in command], "p25_enabled": not args.disable, "native_interface_enabled":native,
        "firstboot_restrictions_enabled": args.first_boot,
        "codec_cache_reused":reused_codec,
        "audits": {"unresolved_symbols": [], "high_register_instructions": 0, "compiler_warnings": 0,**firstboot_audits},
        "forced_rebuild_identical": args.verify_rebuild,
        "codec_speed_build":args.speed_codec, "bench_enabled":not args.lean, "cpu_profile_enabled":args.cpu_profile, "dev_update_enabled":args.dev_update,
        "native_tx_bench_enabled":args.tx_bench,
        "firstboot_flags":47 if args.tx_bench else 15 if args.dev_update else 31,
        "codec_slot": "recognized local stock merged separately" if args.with_local_stock else "reserved; no stock AMBE merged",
        "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)},
        "generated_sources": {str((DEST/key).relative_to(ROOT)): digest for key,digest in sorted(OWNED.items())},
        "outputs": {str(prefix.with_suffix(ext).relative_to(ROOT)): hashlib.sha256(prefix.with_suffix(ext).read_bytes()).hexdigest()
                    for ext in ((".elf", ".bin", ".map", ".codec.bin") if args.with_local_stock else (".elf", ".bin", ".map"))}}
    prefix.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(prefix.with_suffix(".elf"))
    for line in log.read_text().splitlines():
        if "FLASH:" in line or "RAM:" in line: print(line)


if __name__ == "__main__":
    main()

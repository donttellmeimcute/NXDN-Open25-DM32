#!/usr/bin/env python3
"""Pinned, out-of-tree host build of the full IMBE encoder and decoder."""
import argparse
import difflib
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PINS = {
    "imbe_vocoder": ("https://github.com/nostar/imbe_vocoder.git",
                     "03423cb00b8e9ba1be164493e8b98b5bd7c1508c"),
    "mbelib": ("https://github.com/szechyjs/mbelib.git",
               "9a04ed5c78176a9965f3d43f7aa1b1f5330e771f"),
}
SOURCES = ("aux_sub ch_encode dsp_sub imbe_vocoder_impl pitch_est rand_gen "
           "sa_enh v_synt basicop2 dc_rmv encode math_sub pitch_ref sa_decode "
           "tbls v_uv_det ch_decode decode pe_lpf qnt_sub sa_encode uv_synt").split()


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def prepare():
    for name, (url, commit) in PINS.items():
        dest = ROOT / "research" / "codec" / name
        if not dest.exists():
            dest.parent.mkdir(parents=True, exist_ok=True)
            run(["git", "clone", "--no-checkout", url, dest])
            run(["git", "-C", dest, "checkout", "--detach", commit])
        actual = subprocess.check_output(["git", "-C", str(dest), "rev-parse", "HEAD"], text=True).strip()
        if actual != commit:
            raise SystemExit(f"Refusing incompatible {name} commit {actual}; expected {commit}")
        if subprocess.check_output(["git", "-C", str(dest), "status", "--porcelain", "--untracked-files=no"]):
            raise SystemExit(f"Refusing modified pristine dependency {dest}")
    return ROOT / "research" / "codec"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sanitize", action="store_true")
    p.add_argument("--full-ubsan", action="store_true", help="audit legacy arithmetic UB (expected diagnostics before fixes)")
    p.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    p.add_argument("--c-compiler", default=os.environ.get("CC", "clang"))
    p.add_argument("--out", type=Path)
    p.add_argument("--target", action="store_true", help="compile CK803S objects/archive only, using repository native crosscompiler; no target libc or firmware")
    p.add_argument("--speed", action="store_true", help="O3 unity build so fixed-point DSP helpers can inline across source files")
    args = p.parse_args()
    deps = prepare()
    out = (args.out or HERE / "out" / ("ck803s" if args.target else "ubsan" if args.full_ubsan else "sanitize" if args.sanitize else "release")).resolve()
    out.mkdir(parents=True, exist_ok=True)
    portable = out / "portable-source"
    portable.mkdir(exist_ok=True)
    for path in (deps / "imbe_vocoder").iterdir():
        if path.suffix in (".cc", ".h"):
            shutil.copyfile(path, portable / path.name)
    # The pristine library left rand_gen global even though it already has a
    # seed member. Make that routine use its caller's existing state. No DSP
    # algorithm or wire representation changes.
    path = portable / "rand_gen.cc"
    source = path.read_text()
    for old, new in (("#include \"basic_op.h\"", "#include \"basic_op.h\"\n#include \"imbe_vocoder_impl.h\""),
                     ("static UWord32 seed = 1;", "// RNG state is the existing per-instance seed member."),
                     ("Word16 rand_gen(void)", "Word16 imbe_vocoder_impl::rand_gen(void)")):
        if source.count(old) != 1:
            raise SystemExit(f"Portability patch mismatch: {path}: {old}")
        source = source.replace(old, new)
    path.write_text(source)

    # Make intentional modulo-2^32 synthesis arithmetic explicit. Signed
    # saturation helpers also need a defined wrapped temporary before their
    # overflow checks. These changes preserve the existing two's-complement
    # results without depending on signed-overflow optimizer assumptions.
    path = portable / "basicop2.cc"
    source = path.read_text()
    replacements = (
        ("L_var_out = L_var1 + L_var2;", "L_var_out = (Word32)((UWord32)L_var1 + (UWord32)L_var2);", 1),
        ("L_var_out = L_var1 - L_var2;", "L_var_out = (Word32)((UWord32)L_var1 - (UWord32)L_var2);", 2),
        ("L_var_out = L_var1 + L_var2 + Carry;", "L_var_out = (Word32)((UWord32)L_var1 + (UWord32)L_var2 + (UWord32)Carry);", 1),
        ("L_test = L_var1 + L_var2;", "L_test = (Word32)((UWord32)L_var1 + (UWord32)L_var2);", 1),
        ("L_test = L_var1 - L_var2;", "L_test = (Word32)((UWord32)L_var1 - (UWord32)L_var2);", 1),
        ("L_var_out = L_var1 - L_var2 - (Word32) 0X00000001L;", "L_var_out = (Word32)((UWord32)L_var1 - (UWord32)L_var2 - 1u);", 1),
        ("L_var_out = (Word32) var1 << 16;", "L_var_out = (Word32)((UWord32)(Word32)var1 << 16);", 1),
        ("result = (Word32) var1 *((Word32) 1 << var2);", "if (var2 > 15) {\n            if (var1 == 0) return 0;\n            Overflow = 1;\n            return (var1 > 0) ? MAX_16 : MIN_16;\n        }\n        result = (Word32)var1 * ((Word32)1 << var2);", 1),
    )
    for old, new, count in replacements:
        if source.count(old) != count:
            raise SystemExit(f"Arithmetic patch mismatch in basicop2.cc: {old}")
        source = source.replace(old, new)
    path.write_text(source)
    path = portable / "v_synt.cc"
    source = path.read_text()
    replacements = (
        ("(((fund_freq_prev + fund_freq) >> 7) * FRAME/2) << 7", "(Word32)((UWord32)(((fund_freq_prev + fund_freq) >> 7) * FRAME/2) << 7)", 1),
        ("(((L_ph_step >> 7) * 104) << 7)", "((UWord32)((L_ph_step >> 7) * 104) << 7)", 2),
        ("((L_ph_step_aux >> 7) * (i + 1)) << 7", "(Word32)((UWord32)((L_ph_step_aux >> 7) * (i + 1)) << 7)", 1),
        ("((L_ph_step_aux >> 9) * j) << 9", "(Word32)((UWord32)((L_ph_step_aux >> 9) * j) << 9)", 1),
        ("((L_ph_acc_aux >> 9) * j) << 9", "(Word32)((UWord32)((L_ph_acc_aux >> 9) * j) << 9)", 1),
        ("L_ph_acc  += L_tmp;", "L_ph_acc = (Word32)((UWord32)L_ph_acc + (UWord32)L_tmp);", 1),
        ("L_ph_step      += fund_freq;", "L_ph_step = (Word32)((UWord32)L_ph_step + (UWord32)fund_freq);", 1),
        ("L_ph_step_prev += fund_freq_prev;", "L_ph_step_prev = (Word32)((UWord32)L_ph_step_prev + (UWord32)fund_freq_prev);", 1),
        ("L_ph_acc += L_ph_step;", "L_ph_acc = (Word32)((UWord32)L_ph_acc + (UWord32)L_ph_step);", 4),
        ("L_ph_acc += L_ph_step_prev;", "L_ph_acc = (Word32)((UWord32)L_ph_acc + (UWord32)L_ph_step_prev);", 3),
        ("L_ph_acc_aux += L_ph_step_prev;", "L_ph_acc_aux = (Word32)((UWord32)L_ph_acc_aux + (UWord32)L_ph_step_prev);", 2),
        ("L_ph_acc + L_ph_acc_aux", "(Word32)((UWord32)L_ph_acc + (UWord32)L_ph_acc_aux)", 1),
        ("L_ph_acc += L_tmp1;", "L_ph_acc = (Word32)((UWord32)L_ph_acc + (UWord32)L_tmp1);", 1),
    )
    for old, new, count in replacements:
        if source.count(old) != count:
            raise SystemExit(f"Arithmetic patch mismatch in v_synt.cc: {old}")
        source = source.replace(old, new)
    path.write_text(source)
    path = portable / "imbe_vocoder_impl.h"
    source = path.read_text()
    marker = "\t/* member functions */"
    if source.count(marker) != 1:
        raise SystemExit("Portability patch mismatch in imbe_vocoder_impl.h")
    path.write_text(source.replace(marker, marker + "\n\tWord16 rand_gen(void);"))

    # Upstream aborts the entire process on illegal div_s arguments. A radio
    # task must report failure to its owner, which can unkey TX and reset.
    path = portable / "basicop2.cc"
    source = path.read_text()
    source = 'extern "C" void dm32_imbe_note_fault(void);\n' + source
    replacements = (
        ('        printf ("Division Error var1=%d  var2=%d\\n", var1, var2);\n        abort(); /* exit (0); */',
         '        dm32_imbe_note_fault();\n        return 0;'),
        ('        printf ("Division by 0, Fatal error \\n");\n        abort(); /* exit (0); */',
         '        dm32_imbe_note_fault();\n        return 0;'),
    )
    for old, new in replacements:
        if source.count(old) != 1:
            raise SystemExit("Portability patch mismatch in div_s fault handling")
        source = source.replace(old, new)
    path.write_text(source)
    path = portable / "v_uv_det.cc"
    source = path.read_text()
    marker = "extern FILE *fp_in, *fp_out;"
    if source.count(marker) != 1:
        raise SystemExit("Portability patch mismatch in unused FILE declarations")
    path.write_text(source.replace(marker, "// Unused legacy host FILE declarations removed."))

    # FFT's original one-based indexing formed a pointer before its array.
    # Keep every accessed element identical using a valid zero-based pointer.
    path = portable / "dsp_sub.cc"
    source = path.read_text()
    old = "data = &datam1[-1];"
    if source.count(old) != 1:
        raise SystemExit("FFT indexing patch mismatch")
    source = source.replace(old, "data = datam1;")
    source = re.sub(r'data\[([ij])(\+1)?\]',
                    lambda m: 'data[' + m[1] + ('' if m[2] else '-1') + ']', source)
    path.write_text(source.replace('Use pointer indexed from 1 instead of 0',
                                   'One-based FFT loop indices translated at each array access'))

    if args.speed:
        # The shifted dot product is bounded by the full window energy plus
        # one rounding unit per sample. Retain the original saturating path
        # when that bound cannot guarantee every partial sum fits.
        path = portable / "pitch_est.cc"
        source = path.read_text()
        start = "\t// Windowing input signal s * wi^2\n\tfor(i = 0 ; i < PITCH_EST_FRAME; i++)\n\t\tsig_wndwed[i] = mult_r(sigin[i], wi[i]);"
        end = "\t\tcorr[i] = autocorr(sig_wndwed, tmp, scale_shift);"
        if source.count(start) != 1 or source.count(end) != 1:
            raise SystemExit("Pitch correlation optimization patch mismatch")
        source = source.replace(start, """\tbool full_negative = false;
\tfor(i = 0; i < PITCH_EST_FRAME; i++) {
\t\tsig_wndwed[i] = mult_r(sigin[i], wi[i]);
\t\tfull_negative |= sig_wndwed[i] == -32768;
\t}""")
        source = source.replace(end, """\t{
\t\tif(full_negative || L_e0 > MAX_32 - PITCH_EST_FRAME) {
\t\t\tcorr[i] = autocorr(sig_wndwed, tmp, scale_shift);
\t\t\tcontinue;
\t\t}
\t\tWord32 sum = 0;
\t\tconst int count = PITCH_EST_FRAME - tmp;
\t\tif(scale_shift == 0) {
\t\t\tfor(int k = 0; k < count; k++)
\t\t\t\tsum += (Word32)sig_wndwed[k] * sig_wndwed[k + tmp] * 2;
\t\t} else {
\t\t\tfor(int k = 0; k < count; k++)
\t\t\t\tsum += ((Word32)sig_wndwed[k] * sig_wndwed[k + tmp]) >> 4;
\t\t}
\t\tcorr[i] = sum;
\t}""")
        path.write_text(source)

        # Two of the three pitch windows are unchanged when the input advances
        # by FRAME. Keep their scores and only the newest window's PCM. The
        # saved PCM storage offsets the cache within the existing context.
        for name, replacements in {
            'imbe_vocoder_impl.h': [
                ('Word16 pitch_est_buf[PITCH_EST_BUF_SIZE];',
                 'Word16 pitch_est_buf[PITCH_EST_FRAME];\n\tWord16 pitch_scores[2][203];'),
            ],
            'encode.cc': [
                ('v_zap(pitch_est_buf, PITCH_EST_BUF_SIZE);', 'v_zap(pitch_est_buf, PITCH_EST_FRAME);'),
                ('\t\tpitch_est_buf[i] = pitch_est_buf[i + FRAME];\n', ''),
                ('\tpe_lpf(', '\tfor(i = 0; i < PITCH_EST_FRAME - FRAME; i++)\n'
                 '\t\tpitch_est_buf[i] = pitch_est_buf[i + FRAME];\n\tpe_lpf('),
                ('&pitch_est_buf[PITCH_EST_BUF_SIZE - FRAME]', '&pitch_est_buf[PITCH_EST_FRAME - FRAME]'),
            ],
            'pitch_est.cc': [
                ('prev_e_p = prev_prev_e_p = 0;',
                 'prev_e_p = prev_prev_e_p = 0;\n\t// The two leading windows contain only zero samples.\n'
                 '\tfor(int i = 0; i < 203; i++)\n\t\tpitch_scores[0][i] = pitch_scores[1][i] = CNST_1_00_Q4_12;'),
                ('Word16 e_p_arr0[203], e_p_arr1[203], e_p_arr2[203], e1p1_e2p2_est_save[203];',
                 'Word16 e_p_arr0[203], e1p1_e2p2_est_save[203];\n'
                 '\tWord16 *e_p_arr1 = pitch_scores[0], *e_p_arr2 = pitch_scores[1];'),
                ('\t// Calculate E(p) function for current and two future frames\n\te_p(&frames_buf[0], e_p_arr0);',
                 '\tmemcpy(e_p_arr0, e_p_arr1, sizeof e_p_arr0);\n'
                 '\tmemcpy(e_p_arr1, e_p_arr2, sizeof e_p_arr0);\n\te_p(frames_buf, e_p_arr2);'),
                ('\te_p(&frames_buf[FRAME],     e_p_arr1);\n\te_p(&frames_buf[2 * FRAME], e_p_arr2);\n', ''),
            ],
        }.items():
            path = portable / name
            source = path.read_text()
            for old, new in replacements:
                if source.count(old) != 1:
                    raise SystemExit(f"Rolling pitch window patch mismatch in {name}")
                source = source.replace(old, new)
            path.write_text(source)

    # Remove upstream dead locals (only constant/plain assignments, no DSP
    # calls or global arithmetic flags) and repair its misspelled header guard.
    for name, replacements in {
        "pitch_est.cc": [("e1p1_e2p2_est, e1p1_e2p2;", "e1p1_e2p2_est;"),
                         ("e1p1_e2p2 = 1;", "/* unused upstream constant local removed */")],
        "v_uv_det.cc": [("Word32 Sw_sum,", "Word32"),
                        ("Word16 thr[NUM_BANDS_MAX],", "Word16"),
                        ("Sw_sum          = 0;", "/* unused upstream constant local removed */"),
                        ("thr[band_cnt] = dsn_thr;", "/* unused upstream diagnostic array removed */")],
        "pitch_ref.h": [("#define _PIRCH_REF", "#define _PITCH_REF")],
    }.items():
        path = portable / name
        source = path.read_text()
        for old, new in replacements:
            if source.count(old) != 1:
                raise SystemExit(f"Dead-local patch mismatch: {name}: {old}")
            source = source.replace(old, new)
        path.write_text(source)

    # mbelib's interpolation reads log2Ml[57] at the last harmonic when L=56,
    # even though its weight is zero. Clamp the upper neighbor at the endpoint
    # in an out-of-tree reference copy; keep the source clone pristine.
    reference_source = out / "reference-source"
    reference_source.mkdir(exist_ok=True)
    for path in (deps / "mbelib").iterdir():
        if path.suffix in (".c", ".h"):
            shutil.copyfile(path, reference_source / path.name)
    path = reference_source / "imbe7200x4400.c"
    source = path.read_text()
    old = "prev_mp->log2Ml[intkl[l] + 1]"
    if source.count(old) != 2:
        raise SystemExit("Reference patch mismatch in mbelib interpolation")
    path.write_text(source.replace(old, "prev_mp->log2Ml[intkl[l] < prev_mp->L ? intkl[l] + 1 : prev_mp->L]"))

    patches = []
    for original, copied, generated in ((deps / "imbe_vocoder", portable, "generated/portable-source"), (deps / "mbelib", reference_source, "generated/reference-source")):
        for path in sorted(copied.iterdir()):
            if (original / path.name).is_file():
                before = (original / path.name).read_text().splitlines(keepends=True)
                after = path.read_text().splitlines(keepends=True)
                patches.extend(difflib.unified_diff(before, after,
                    fromfile=str(original.relative_to(ROOT) / path.name),
                    tofile=generated + "/" + path.name))
    (out / "portability.patch").write_text("".join(patches))

    common = ["-O3" if args.speed else "-O2", "-g", "-fwrapv", "-fno-strict-aliasing", "-fstack-usage", "-Wall", "-Wextra",
              "-ffile-prefix-map=" + str(ROOT) + "=.",
              "-fdebug-prefix-map=" + str(ROOT) + "=.",
              "-ffile-prefix-map=" + str(out) + "=/generated-codec",
              "-fdebug-prefix-map=" + str(out) + "=/generated-codec"]
    if args.target:
        if args.sanitize or args.full_ubsan:
            raise SystemExit("Sanitizers require the host build")
        args.compiler = str(ROOT / "toolchain" / "bin" / "csky-elf-g++")
        args.c_compiler = str(ROOT / "toolchain" / "bin" / "csky-elf-gcc")
        common += ["-mcpu=ck803s", "-mlittle-endian", "-mno-high-registers", "-msoft-float", "-ffreestanding",
                   "-ffunction-sections", "-fdata-sections", "-fcallgraph-info=su", "-nostdinc++", "-I", HERE / "freestanding" / "include"]
    if args.sanitize:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if args.full_ubsan:
        common += ["-fno-wrapv", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    cxx = common + ["-std=c++11", "-fno-exceptions", "-fno-rtti", "-I", portable, "-I", ROOT / "src" / "codec"]
    objects = []
    stems = SOURCES
    if args.speed:
        # Same audited source and arithmetic; no fast-math or codec algorithm changes.
        unity = portable / "codec_unity.cc"
        unity.write_text('/* SPDX-License-Identifier: GPL-3.0-or-later */\n' +
                         ''.join(('#define BIT_MASK_TABLE8 decode_bit_masks\n' if stem == 'decode' else '') +
                                 '#include "' + stem + '.cc"\n' +
                                 ('#undef BIT_MASK_TABLE8\n' if stem == 'decode' else '') for stem in SOURCES))
        stems = ["codec_unity"]
    for stem in stems:
        obj = out / (stem + ".o")
        run([args.compiler, *cxx, "-c", portable / (stem + ".cc"), "-o", obj], stdout=subprocess.DEVNULL)
        objects.append(obj)
    bridge = out / "dm32_imbe.o"
    run([args.compiler, *cxx, "-c", ROOT / "src" / "codec" / "imbe.cc", "-o", bridge])
    objects.append(bridge)
    ar = ROOT / "toolchain" / "bin" / "csky-elf-ar" if args.target else "ar"
    # ar otherwise retains obsolete members when changing between separate/unity builds.
    (out / "libdm32_imbe.a").unlink(missing_ok=True)
    run([ar, "rcsD" if args.target else "rcs", out / "libdm32_imbe.a", *objects])
    if args.target:
        tool = ROOT / "toolchain" / "bin"
        run([tool / "csky-elf-ld", "-r", *objects, "-o", out / "codec-all.o"])
        outputs = {}
        for name, cmd in (
            ("size", [tool / "csky-elf-size", out / "codec-all.o"]),
            ("unresolved", [tool / "csky-elf-nm", "-u", out / "codec-all.o"]),
            ("elf", [tool / "csky-elf-readelf", "-h", "-A", out / "codec-all.o"]),
        ):
            outputs[name] = subprocess.check_output([str(x) for x in cmd], text=True)
        manifest = {
            "kind": "HARDWARE-UNVERIFIED CK803S relocatable codec objects/archive; not executable radio firmware",
            "pins": {name: {"url": url, "commit": sha} for name, (url, sha) in PINS.items()},
            "compiler": subprocess.check_output([args.compiler, "--version"], text=True),
            "flags": [str(x) for x in cxx],
            "compile_only_headers": "experiments/codec/freestanding/include: GCC builtin scalar headers, string prototypes and placement new; no target libc/sysroot fabricated",
            "archive_sha256": hashlib.sha256((out / "libdm32_imbe.a").read_bytes()).hexdigest(),
            "measurements": outputs,
            "limitations": "No startup, target memory layout, interrupts, audio or modem integration; unresolved runtime symbols must be supplied and audited before linking an application. Static .su files are compiler frame estimates, not measured stack high-water marks.",
        }
        (out / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps(outputs, indent=2))
        return
    references = []
    for stem in ("mbelib", "ecc", "imbe7200x4400"):
        obj = out / ("reference_" + stem + ".o")
        run([args.c_compiler, *common, "-Wno-missing-braces", "-std=c99", "-D_DEFAULT_SOURCE", "-c", reference_source / (stem + ".c"), "-o", obj])
        references.append(obj)
    executable = out / "codec_harness"
    run([args.compiler, *cxx, "-I", deps / "mbelib", HERE / "harness.cc", *objects, *references, "-o", executable])
    p25 = out / "p25.o"
    run([args.c_compiler, *common, "-std=c99", "-I", ROOT / "src" / "p25", "-c", ROOT / "src" / "p25" / "p25.c", "-o", p25])
    run([args.compiler, *cxx, "-I", deps / "mbelib", "-I", ROOT / "src" / "p25", HERE / "pipeline.cc", *objects, *references, p25, "-o", out / "codec_pipeline"])
    manifest = {
        "kind": "HOST-ONLY full IMBE encoder/decoder experiment; not radio firmware",
        "pins": {name: {"url": url, "commit": sha} for name, (url, sha) in PINS.items()},
        "compiler": subprocess.check_output([args.compiler, "--version"], text=True),
        "flags": [str(x) for x in cxx],
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "arithmetic": "twos-complement and arithmetic signed right shift required; explicit unsigned modulo phase arithmetic and saturation intermediates; -fwrapv is defensive; full UBSan audited with -fno-wrapv",
        "rng_patch": "global synthesis PRNG changed to pre-existing per-context seed; pristine sources unchanged",
        "fault_patch": "div_s reports a bounded operation failure instead of printing/aborting",
        "reference_patch": "mbelib last-harmonic interpolation endpoint clamped to avoid index57 UB",
        "arithmetic_patch": "intentional modulo phase arithmetic and saturating-operation temporaries use explicit unsigned arithmetic",
    }
    (out / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(executable)


if __name__ == "__main__":
    main()

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portable clear conventional P25 Phase 1 CAI research implementation.
 * Coding matrices and packet layout adapted from Michael Ossmann's p25craft.py
 * (Copyright 2011 Michael Ossmann, GPL-2.0-or-later) at OP25 commit
 * 3f44d43eb449bb4753247ce848bb42a403823dea. See docs/protocol.md.
 * No hardware driver is implied by this file.
 */
#include "p25.h"
#include "p25_runtime.h"

#include "golay_table.inc"

#define SYNC UINT64_C(0x5575f5ff77ff)
#define SYNC_MASK UINT64_C(0xffffffffffff)

static unsigned popcount64(uint64_t a)
{
    unsigned n = 0;
    while (a) { a &= a - 1; ++n; }
    return n;
}

static uint8_t mul(uint8_t a, uint8_t b)
{
    return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}

static uint8_t divide(uint8_t a, uint8_t b)
{
    return a ? gf_exp[gf_log[a] + 63 - gf_log[b]] : 0;
}

static uint8_t alpha(unsigned power) { return gf_exp[power % 63]; }

/* Berlekamp-Massey, locator coefficients in ascending order. */
static int locator(const uint8_t *syndromes, unsigned n, uint8_t *lambda)
{
    uint8_t bpoly[23] = {1}, saved[23];
    uint8_t b = 1;
    unsigned length = 0, step = 1, i, k;
    memset(lambda, 0, 23);
    lambda[0] = 1;
    for (k = 0; k < n; ++k) {
        uint8_t d = syndromes[k];
        for (i = 1; i <= length; ++i)
            d ^= mul(lambda[i], syndromes[k - i]);
        if (!d) { ++step; continue; }
        memcpy(saved, lambda, 23);
        for (i = 0; i + step <= n; ++i)
            lambda[i + step] ^= mul(divide(d, b), bpoly[i]);
        if (2 * length <= k) {
            length = k + 1 - length;
            memcpy(bpoly, saved, 23);
            b = d;
            step = 1;
        } else ++step;
    }
    return length <= n / 2 ? (int)length : -1;
}

static uint8_t evaluate_ascending(const uint8_t *poly, unsigned degree, uint8_t x)
{
    uint8_t y = poly[degree];
    while (degree) y = mul(y, x) ^ poly[--degree];
    return y;
}

static int valid_rs(size_t count, unsigned parity)
{
    return (parity == 8 || parity == 12 || parity == 16) && count > parity && count <= 63;
}

int p25_rs_encode(const uint8_t *data, size_t count, unsigned parity, uint8_t *out)
{
    uint8_t generator[17] = {1}, next[17], work[63];
    unsigned i, j;
    size_t k;
    if (!data || !out || count > 63 || parity > 63 || count > 63 - parity || !valid_rs(count + parity, parity)) return -1;
    for (k = 0; k < count; ++k) if (data[k] > 63) return -1;
    for (i = 0; i < parity; ++i) {
        memset(next, 0, sizeof next);
        for (j = 0; j <= i; ++j) {
            next[j] ^= generator[j];
            next[j + 1] ^= mul(generator[j], alpha(i + 1));
        }
        memcpy(generator, next, sizeof generator);
    }
    memcpy(work, data, count);
    memset(work + count, 0, parity);
    for (k = 0; k < count; ++k) {
        uint8_t feedback = work[k];
        for (j = 1; j <= parity; ++j)
            work[k + j] ^= mul(feedback, generator[j]);
    }
    memmove(out, data, count);
    memcpy(out + count, work + count, parity);
    return 0;
}

static int rs_syndromes(const uint8_t *cw, size_t count, unsigned parity, uint8_t *s)
{
    unsigned i;
    size_t j;
    int any = 0;
    for (i = 0; i < parity; ++i) {
        s[i] = 0;
        for (j = 0; j < count; ++j)
            s[i] = mul(s[i], alpha(i + 1)) ^ cw[j];
        any |= s[i];
    }
    return any;
}

int p25_rs_decode(uint8_t *cw, size_t count, unsigned parity)
{
    uint8_t s[16], lambda[23], candidate[63], matrix[8][9] = {{0}};
    uint8_t x[8], positions[8];
    unsigned found = 0, power, i, j, k;
    int errors;
    if (!cw || !valid_rs(count, parity)) return -1;
    for (i = 0; i < count; ++i) if (cw[i] > 63) return -1;
    if (!rs_syndromes(cw, count, parity, s)) return 0;
    errors = locator(s, parity, lambda);
    if (errors <= 0) return -1;
    /* Polynomial coefficient exponent is count-1-index for shortened words. */
    for (power = 0; power < count; ++power) {
        if (!evaluate_ascending(lambda, (unsigned)errors, alpha(63 - power))) {
            if (found >= (unsigned)errors) return -1;
            positions[found] = (uint8_t)(count - 1 - power);
            x[found++] = alpha(power);
        }
    }
    if (found != (unsigned)errors) return -1;
    /* Solve syndrome Vandermonde system for error magnitudes. Max 8x9. */
    for (j = 0; j < found; ++j) {
        uint8_t value = x[j];
        for (i = 0; i < found; ++i) {
            matrix[i][j] = value;
            value = mul(value, x[j]);
        }
    }
    for (i = 0; i < found; ++i) matrix[i][found] = s[i];
    for (k = 0; k < found; ++k) {
        uint8_t pivot;
        for (i = k; i < found && !matrix[i][k]; ++i) {}
        if (i == found) return -1;
        if (i != k) for (j = k; j <= found; ++j) {
            uint8_t temp = matrix[k][j];
            matrix[k][j] = matrix[i][j]; matrix[i][j] = temp;
        }
        pivot = matrix[k][k];
        for (j = k; j <= found; ++j) matrix[k][j] = divide(matrix[k][j], pivot);
        for (i = 0; i < found; ++i) if (i != k) {
            uint8_t factor = matrix[i][k];
            for (j = k; j <= found; ++j) matrix[i][j] ^= mul(factor, matrix[k][j]);
        }
    }
    memcpy(candidate, cw, count);
    for (i = 0; i < found; ++i) candidate[positions[i]] ^= matrix[i][found];
    if (rs_syndromes(candidate, count, parity, s)) return -1;
    memcpy(cw, candidate, count);
    return errors;
}

uint64_t p25_nid_encode(uint16_t nac, uint8_t duid)
{
    static const uint64_t matrix[16] = {
        UINT64_C(0x8000cd930bdd3b2a), UINT64_C(0x4000ab5a8e33a6be),
        UINT64_C(0x2000983e4cc4e874), UINT64_C(0x10004c1f2662743a),
        UINT64_C(0x0800eb9c98ec0136), UINT64_C(0x0400b85d47ab3bb0),
        UINT64_C(0x02005c2ea3d59dd8), UINT64_C(0x01002e1751eaceec),
        UINT64_C(0x0080170ba8f56776), UINT64_C(0x0040c616dfa78890),
        UINT64_C(0x0020630b6fd3c448), UINT64_C(0x00103185b7e9e224),
        UINT64_C(0x000818c2dbf4f112), UINT64_C(0x0004c1f2662743a2),
        UINT64_C(0x0002ad6a38ce9afb), UINT64_C(0x00019b2617ba7657)
    };
    uint16_t data = (uint16_t)((nac << 4) | (duid & 15));
    uint64_t result = 0;
    unsigned i;
    for (i = 0; i < 16; ++i) if (data & (0x8000u >> i)) result ^= matrix[i];
    return result;
}

size_t p25_frame_bytes(uint8_t duid)
{
    switch (duid) {
    case P25_DUID_HDU: return P25_HDU_BYTES;
    case P25_DUID_TDU: return P25_TDU_BYTES;
    case P25_DUID_LDU1: case P25_DUID_LDU2: return P25_LDU_BYTES;
    case P25_DUID_TDULC: return P25_TDULC_BYTES;
    default: return 0;
    }
}

int p25_nid_decode(uint64_t air_nid, p25_nid *result)
{
    uint64_t cw = air_nid >> 1, corrected;
    uint8_t s[22] = {0}, lambda[23];
    unsigned i, bit, found = 0, differences;
    int errors;
    if (!result) return -1;
    for (i = 0; i < 22; ++i)
        for (bit = 0; bit < 63; ++bit)
            if ((cw >> bit) & 1) s[i] ^= alpha((i + 1) * bit);
    errors = locator(s, 22, lambda);
    if (errors < 0) return -1;
    if (errors) {
        for (bit = 0; bit < 63; ++bit)
            if (!evaluate_ascending(lambda, (unsigned)errors, alpha(63 - bit))) {
                cw ^= UINT64_C(1) << bit; ++found;
            }
        if (found != (unsigned)errors) return -1;
    }
    corrected = p25_nid_encode((uint16_t)(cw >> 51), (uint8_t)(cw >> 47));
    differences = popcount64(corrected ^ air_nid);
    if (differences > 11 || !p25_frame_bytes((uint8_t)((cw >> 47) & 15))) return -1;
    result->nac = (uint16_t)(cw >> 51);
    result->duid = (uint8_t)((cw >> 47) & 15);
    result->corrected_bits = (uint8_t)differences;
    return 0;
}

static unsigned bit_get(const uint8_t *p, unsigned i)
{ return (p[i >> 3] >> (7 - (i & 7))) & 1; }

static void bit_set(uint8_t *p, unsigned i, unsigned value)
{
    unsigned mask = 1u << (7 - (i & 7));
    p[i >> 3] = (uint8_t)((p[i >> 3] & ~mask) | (value ? mask : 0));
}

/* Translate logical data bit positions around every status-symbol gap. */
static unsigned physical(unsigned logical) { return logical + 2 * (logical / 70); }

static void put_value(uint8_t *frame, unsigned logical, uint64_t value, unsigned bits)
{
    unsigned i;
    for (i = 0; i < bits; ++i)
        bit_set(frame, physical(logical + i), (unsigned)((value >> (bits - 1 - i)) & 1));
}

static uint32_t get_value(const uint8_t *frame, unsigned logical, unsigned bits)
{
    uint32_t result = 0;
    unsigned i;
    for (i = 0; i < bits; ++i) result = (result << 1) | bit_get(frame, physical(logical + i));
    return result;
}

static uint32_t matrix_encode(uint32_t data, const uint32_t *matrix, unsigned count)
{
    uint32_t cw = 0;
    unsigned i;
    for (i = 0; i < count; ++i) if ((data >> (count - 1 - i)) & 1) cw ^= matrix[i];
    return cw;
}

static uint32_t golay24(uint16_t data)
{
    static const uint32_t matrix[12] = {
        040006165, 020003073, 010007550, 04003664, 02001732,
        01006631, 0403315, 0201547, 0106706, 045227, 024476, 014353
    };
    return matrix_encode(data, matrix, 12);
}

static unsigned golay_syndrome(uint32_t pattern)
{
    int bit;
    for (bit = 22; bit >= 11; --bit) if (pattern & (1u << bit)) pattern ^= 0xc75u << (bit - 11);
    return pattern;
}

static unsigned golay23_decode(uint32_t cw, uint16_t *data)
{
    uint16_t correction = golay_errors[golay_syndrome(cw)];
    *data = (uint16_t)((cw >> 11) ^ (correction >> 2));
    return correction & 3;
}

static int golay24_decode(uint32_t cw, uint16_t *data, int shortened)
{
    unsigned errors;
    golay23_decode(cw >> 1, data);
    if (shortened && *data > 63) return -1;
    errors = popcount64(golay24(*data) ^ cw);
    return errors <= 3 ? (int)errors : -1;
}

static uint16_t hamming15(uint16_t data)
{
    static const uint32_t matrix[11] = {
        0x400f, 0x200e, 0x100d, 0x080c, 0x040b, 0x020a,
        0x0109, 0x0087, 0x0046, 0x0025, 0x0013
    };
    return (uint16_t)matrix_encode(data, matrix, 11);
}

static uint16_t hamming10(uint16_t data)
{
    static const uint32_t matrix[6] = {0x20e, 0x10d, 0x08b, 0x047, 0x023, 0x01c};
    return (uint16_t)matrix_encode(data, matrix, 6);
}

static int hamming_decode(uint16_t cw, uint16_t *data, int shortened)
{
    uint16_t (*encode)(uint16_t) = shortened ? hamming10 : hamming15;
    unsigned syndrome = cw ^ encode(cw >> 4), bit, count = shortened ? 10 : 15;
    if (!syndrome) { *data = cw >> 4; return 0; }
    for (bit = 0; bit < count; ++bit) {
        uint16_t single = (uint16_t)(1u << bit);
        if ((unsigned)(single ^ encode(single >> 4)) == syndrome) {
            *data = (cw ^ single) >> 4; return 1;
        }
    }
    return -1;
}

static uint16_t cyclic16(uint8_t data)
{
    static const uint32_t matrix[8] = {0x804e, 0x4027, 0x208f, 0x10db, 0x08f1, 0x04e4, 0x0272, 0x0139};
    return (uint16_t)matrix_encode(data, matrix, 8);
}

static int cyclic_decode(uint16_t cw, uint8_t *data)
{
    unsigned candidate, best = 17, nearest = 0;
    for (candidate = 0; candidate < 256; ++candidate) {
        unsigned distance = popcount64(cyclic16((uint8_t)candidate) ^ cw);
        if (distance < best) { best = distance; nearest = candidate; }
    }
    if (best > 2) return -1;
    *data = (uint8_t)nearest;
    return (int)best;
}

static uint32_t pn_bits(uint32_t *state, unsigned bits)
{
    uint32_t out = 0;
    unsigned i;
    for (i = 0; i < bits; ++i) {
        *state = (173 * *state + 13849) & 0xffff;
        out = (out << 1) | ((*state >> 15) & 1);
    }
    return out;
}

static void cw_put(uint8_t *cw, unsigned start, unsigned bits, uint32_t word)
{
    unsigned i;
    for (i = 0; i < bits; ++i) bit_set(cw, start + i, (word >> (bits - 1 - i)) & 1);
}

static uint32_t cw_get(const uint8_t *cw, unsigned start, unsigned bits)
{
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < bits; ++i) value = (value << 1) | bit_get(cw, start + i);
    return value;
}

int p25_imbe_encode(const uint16_t u[8], uint8_t cw[18])
{
    uint32_t pn;
    unsigned i;
    if (!u || !cw) return -1;
    for (i = 0; i < 8; ++i) if (u[i] >= (1u << (i < 4 ? 12 : i < 7 ? 11 : 7))) return -1;
    memset(cw, 0, 18);
    pn = (uint32_t)u[0] << 4;
    cw_put(cw, 0, 23, golay24(u[0]) >> 1);
    for (i = 1; i < 4; ++i) cw_put(cw, 23 * i, 23, (golay24(u[i]) >> 1) ^ pn_bits(&pn, 23));
    for (i = 4; i < 7; ++i) cw_put(cw, 92 + 15 * (i - 4), 15, hamming15(u[i]) ^ pn_bits(&pn, 15));
    cw_put(cw, 137, 7, u[7]);
    return 0;
}

int p25_imbe_decode(const uint8_t cw[18], uint16_t u[8], uint8_t *u0_errors)
{
    uint32_t pn;
    unsigned i, total;
    if (!cw || !u || !u0_errors) return -1;
    total = golay23_decode(cw_get(cw, 0, 23), &u[0]);
    *u0_errors = (uint8_t)total;
    pn = (uint32_t)u[0] << 4;
    for (i = 1; i < 4; ++i)
        total += golay23_decode(cw_get(cw, 23 * i, 23) ^ pn_bits(&pn, 23), &u[i]);
    for (i = 4; i < 7; ++i) {
        int n = hamming_decode((uint16_t)(cw_get(cw, 92 + 15 * (i - 4), 15) ^ pn_bits(&pn, 15)), &u[i], 0);
        if (n < 0) return -1;
        total += (unsigned)n;
    }
    u[7] = (uint16_t)cw_get(cw, 137, 7);
    return (int)total;
}

static int valid_call(const p25_call *call)
{
    return call && call->nac <= 0xfff && call->source_id <= 0xffffff && call->manufacturer_id <= 1 && call->status_symbol <= 3 && !(call->service_options & 0x78);
}

static void frame_start(uint8_t *out, size_t size, const p25_call *call, uint8_t duid)
{
    unsigned symbol;
    memset(out, 0, size);
    put_value(out, 0, SYNC, 48);
    put_value(out, 48, p25_nid_encode(call->nac, duid), 64);
    for (symbol = 35; symbol < size * 4; symbol += 36) {
        bit_set(out, 2 * symbol, call->status_symbol >> 1);
        bit_set(out, 2 * symbol + 1, call->status_symbol & 1);
    }
}

static void bytes_to_symbols(const uint8_t *bytes, unsigned n, uint8_t *symbols)
{
    unsigned i, bit;
    for (i = 0; i < n; ++i) {
        symbols[i] = 0;
        for (bit = 0; bit < 6; ++bit) symbols[i] = (uint8_t)((symbols[i] << 1) | bit_get(bytes, 6 * i + bit));
    }
}

static void symbols_to_bytes(const uint8_t *symbols, unsigned count, uint8_t *bytes)
{
    unsigned i, bit;
    memset(bytes, 0, count * 6 / 8);
    for (i = 0; i < count; ++i) for (bit = 0; bit < 6; ++bit) bit_set(bytes, 6 * i + bit, (symbols[i] >> (5 - bit)) & 1);
}

static void group_lc(const p25_call *call, uint8_t lc[9])
{
    lc[0] = 0; lc[1] = call->manufacturer_id; lc[2] = call->service_options;
    lc[3] = 0;
    lc[4] = (uint8_t)(call->talkgroup >> 8); lc[5] = (uint8_t)call->talkgroup;
    lc[6] = (uint8_t)(call->source_id >> 16); lc[7] = (uint8_t)(call->source_id >> 8); lc[8] = (uint8_t)call->source_id;
}

int p25_encode_hdu(const p25_call *call, uint8_t out[P25_HDU_BYTES])
{
    uint8_t payload[15] = {0}, symbols[36];
    unsigned i;
    if (!out || !valid_call(call)) return -1;
    payload[9] = call->manufacturer_id; payload[10] = 0x80; /* clear */
    payload[13] = (uint8_t)(call->talkgroup >> 8); payload[14] = (uint8_t)call->talkgroup;
    bytes_to_symbols(payload, 20, symbols);
    p25_rs_encode(symbols, 20, 16, symbols);
    frame_start(out, P25_HDU_BYTES, call, P25_DUID_HDU);
    for (i = 0; i < 36; ++i) put_value(out, 112 + 18 * i, golay24(symbols[i]), 18);
    return 0;
}

static unsigned voice_start(unsigned frame)
{
    if (frame < 2) return 112 + 144 * frame;
    if (frame < 8) return 440 + 184 * (frame - 2);
    return 1536;
}

/* Six rows of 24 bits, swapping bit-pairs in odd columns. Formula verified
 * against every one of the 1296 OP25 voice_codeword_bits entries in tests. */
static unsigned voice_permutation(unsigned bit)
{ return 6 * (bit % 24) + ((bit / 24) ^ (bit & 1)); }

int p25_encode_ldu(const p25_call *call, uint8_t duid, const uint16_t imbe[9][8], const uint8_t lsd[2], uint8_t out[P25_LDU_BYTES])
{
    uint8_t payload[12] = {0}, symbols[24], cw[18];
    uint16_t u[8];
    unsigned count, parity, i, j;
    if (!out || !imbe || !lsd || !valid_call(call) || (duid != P25_DUID_LDU1 && duid != P25_DUID_LDU2)) return -1;
    /* Validate every frame before touching caller output. */
    for (i = 0; i < 9; ++i) for (j = 0; j < 8; ++j)
        if (imbe[i][j] >= (1u << (j < 4 ? 12 : j < 7 ? 11 : 7))) return -1;
    if (duid == P25_DUID_LDU1) { group_lc(call, payload); count = 12; parity = 12; }
    else { payload[9] = 0x80; count = 16; parity = 8; }
    bytes_to_symbols(payload, count, symbols);
    p25_rs_encode(symbols, count, parity, symbols);
    frame_start(out, P25_LDU_BYTES, call, duid);
    for (i = 0; i < 9; ++i) {
        memcpy(u, imbe[i], sizeof u);
        u[7] = (uint16_t)((u[7] & ~1u) | ((i + (duid == P25_DUID_LDU2)) & 1));
        p25_imbe_encode(u, cw);
        for (j = 0; j < 144; ++j) bit_set(out, physical(voice_start(i) + voice_permutation(j)), bit_get(cw, j));
    }
    for (i = 0; i < 24; ++i)
        put_value(out, 400 + 184 * (i / 4) + 10 * (i % 4), hamming10(symbols[i]), 10);
    put_value(out, 1504, cyclic16(lsd[0]), 16);
    put_value(out, 1520, cyclic16(lsd[1]), 16);
    return 0;
}

int p25_encode_tdu(const p25_call *call, uint8_t out[P25_TDU_BYTES])
{
    if (!out || !valid_call(call)) return -1;
    frame_start(out, P25_TDU_BYTES, call, P25_DUID_TDU);
    return 0;
}

int p25_encode_tdulc(const p25_call *call, uint8_t out[P25_TDULC_BYTES])
{
    uint8_t payload[9], symbols[24];
    unsigned i;
    if (!out || !valid_call(call)) return -1;
    group_lc(call, payload);
    bytes_to_symbols(payload, 12, symbols);
    p25_rs_encode(symbols, 12, 12, symbols);
    frame_start(out, P25_TDULC_BYTES, call, P25_DUID_TDULC);
    for (i = 0; i < 12; ++i) put_value(out, 112 + 24 * i, golay24((uint16_t)((symbols[2 * i] << 6) | symbols[2 * i + 1])), 24);
    return 0;
}

static void decode_lc(const uint8_t *payload, p25_decoded *r)
{
    r->link_control_format = payload[0]; r->manufacturer_id = payload[1];
    r->service_options = payload[2];
    /* Decode group voice LCO 0 only; reject protected/nonstandard formats. */
    if (payload[0] || payload[1] > 1) { r->metadata_valid = 0; return; }
    r->talkgroup = (uint16_t)((payload[4] << 8) | payload[5]);
    r->source_id = ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 8) | payload[8];
    r->clear_voice = !(payload[2] & 0x40);
}

int p25_decode_frame(const uint8_t *frame, size_t bytes, p25_decoded *r)
{
    uint8_t symbols[36] = {0}, payload[15], cw[18];
    uint64_t nid = 0, sync = 0;
    uint16_t word;
    unsigned i, j, count, parity;
    int errors, meta_errors = 0;
    if (!frame || !r || bytes < P25_TDU_BYTES) return -1;
    memset(r, 0, sizeof *r);
    /* Exact frame sync can contain errors after tolerant stream acquisition. */
    for (i = 0; i < 48; ++i) sync = (sync << 1) | bit_get(frame, i);
    if (popcount64(sync ^ SYNC) > 4) return -1;
    for (i = 48; i < 112; ++i) nid = (nid << 1) | bit_get(frame, physical(i));
    if (p25_nid_decode(nid, &r->nid) || p25_frame_bytes(r->nid.duid) != bytes) return -1;
    if (r->nid.duid == P25_DUID_TDU) { r->metadata_valid = 1; return 0; }
    if (r->nid.duid == P25_DUID_HDU) {
        count = 36; parity = 16;
        for (i = 0; i < count; ++i) {
            uint32_t coded = get_value(frame, 112 + 18 * i, 18);
            errors = golay24_decode(coded, &word, 1);
            if (errors < 0) { meta_errors = -1000; word = (uint16_t)(coded >> 12); }
            else if (meta_errors >= 0) meta_errors += errors;
            symbols[i] = (uint8_t)word;
        }
    } else if (r->nid.duid == P25_DUID_TDULC) {
        count = 24; parity = 12;
        for (i = 0; i < 12; ++i) {
            uint32_t coded = get_value(frame, 112 + 24 * i, 24);
            errors = golay24_decode(coded, &word, 0);
            if (errors < 0) { meta_errors = -1000; word = (uint16_t)(coded >> 12); }
            else if (meta_errors >= 0) meta_errors += errors;
            symbols[2 * i] = (uint8_t)(word >> 6); symbols[2 * i + 1] = (uint8_t)(word & 63);
        }
    } else {
        count = 24; parity = r->nid.duid == P25_DUID_LDU1 ? 12 : 8;
        for (i = 0; i < 24; ++i) {
            errors = hamming_decode((uint16_t)get_value(frame, 400 + 184 * (i / 4) + 10 * (i % 4), 10), &word, 1);
            if (errors < 0) meta_errors = -1000; else if (meta_errors >= 0) meta_errors += errors;
            symbols[i] = errors < 0 ? (uint8_t)(get_value(frame, 400 + 184 * (i / 4) + 10 * (i % 4), 6)) : (uint8_t)word;
        }
        for (i = 0; i < 9; ++i) {
            memset(cw, 0, sizeof cw);
            for (j = 0; j < 144; ++j) bit_set(cw, j, bit_get(frame, physical(voice_start(i) + voice_permutation(j))));
            errors = p25_imbe_decode(cw, r->imbe[i], &r->voice_u0_corrected_bits[i]);
            if (errors < 0) return -1;
            r->voice_corrected_bits[i] = (uint8_t)errors;
        }
        for (i = 0; i < 2; ++i) {
            errors = cyclic_decode((uint16_t)get_value(frame, 1504 + 16 * i, 16), &r->low_speed_data[i]);
            r->low_speed_valid[i] = errors >= 0;
        }
    }
    errors = p25_rs_decode(symbols, count, parity);
    if (errors < 0) return 0;
    r->metadata_valid = 1;
    r->metadata_corrected_symbols = (uint8_t)errors;
    r->metadata_corrected_bits = meta_errors < 0 ? UINT16_MAX : (uint16_t)meta_errors;
    symbols_to_bytes(symbols, count - parity, payload);
    if (r->nid.duid == P25_DUID_HDU) {
        memcpy(r->message_indicator, payload, 9);
        r->manufacturer_id = payload[9]; r->algorithm_id = payload[10];
        r->key_id = (uint16_t)((payload[11] << 8) | payload[12]);
        r->talkgroup = (uint16_t)((payload[13] << 8) | payload[14]);
        r->clear_voice = payload[10] == 0x80;
    } else if (r->nid.duid == P25_DUID_LDU2) {
        memcpy(r->message_indicator, payload, 9);
        r->algorithm_id = payload[9]; r->key_id = (uint16_t)((payload[10] << 8) | payload[11]);
        r->clear_voice = payload[9] == 0x80;
    } else decode_lc(payload, r);
    return 0;
}

void p25_rx_init(p25_rx *rx, uint16_t nac, uint8_t max_sync_errors)
{
    memset(rx, 0, sizeof *rx);
    rx->expected_nac = nac;
    rx->max_sync_errors = max_sync_errors <= 4 ? max_sync_errors : 4;
}

static void rx_acquire(p25_rx *rx, uint64_t sync, uint8_t inverted)
{
    unsigned i;
    rx->inverted = inverted;
    if (inverted) sync ^= UINT64_C(0xaaaaaaaaaaaa);
    memset(rx->frame, 0, sizeof rx->frame);
    for (i = 0; i < 48; ++i) bit_set(rx->frame, i, (unsigned)((sync >> (47 - i)) & 1));
    rx->symbols = 24; rx->target_symbols = 0;
    ++rx->sync_acquisitions;
}

int p25_rx_push_dibit(p25_rx *rx, uint8_t dibit)
{
    unsigned normal, inverted;
    if (!rx || dibit > 3) { if (rx) rx->symbols = 0; return -1; }
    if (rx->symbols_seen != UINT32_MAX) ++rx->symbols_seen;
    rx->sync_shift = ((rx->sync_shift << 2) | dibit) & SYNC_MASK;
    normal = popcount64(rx->sync_shift ^ SYNC);
    inverted = popcount64(rx->sync_shift ^ SYNC ^ UINT64_C(0xaaaaaaaaaaaa));
    if (rx->symbols_seen >= 24 && (normal <= rx->max_sync_errors || inverted <= rx->max_sync_errors)) {
        if (rx->symbols) ++rx->truncated_frames;
        rx_acquire(rx, rx->sync_shift, inverted < normal);
        return 0;
    }
    if (!rx->symbols) return 0;
    if (rx->inverted) dibit ^= 2;
    bit_set(rx->frame, 2 * rx->symbols, dibit >> 1);
    bit_set(rx->frame, 2 * rx->symbols + 1, dibit & 1);
    ++rx->symbols;
    if (rx->symbols == 57) {
        uint64_t nid = 0;
        unsigned i;
        for (i = 48; i < 112; ++i) nid = (nid << 1) | bit_get(rx->frame, physical(i));
        if (p25_nid_decode(nid, &rx->nid) || (rx->expected_nac != P25_NAC_ANY && rx->nid.nac != rx->expected_nac)) {
            rx->symbols = 0; ++rx->bad_nids; return 0;
        }
        rx->target_symbols = (uint16_t)(4 * p25_frame_bytes(rx->nid.duid));
    }
    if (rx->target_symbols && rx->symbols == rx->target_symbols) {
        rx->symbols = 0; ++rx->complete_frames; return 1;
    }
    return 0;
}

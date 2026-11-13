/* SPDX-License-Identifier: GPL-3.0-or-later */
/* p2.h: P25 Phase 2 channel coding. Field positions and code parameters follow OP25's p25p2_tdma.cc and
 * apps/tdma (isch.py, duid.py, lfsr.py) and dsd-fme's p25p2_frame.c / p25p2_xcch.c; the code here is new. */
#include "p2.h"
#include <string.h>

#include "p2_tables.inc"

/* ---- ISCH and DUID ---- */

uint64_t p2_isch_encode(uint8_t value)
{
    uint64_t cw = P2_ISCH_C0;
    for (unsigned i = 0; i < 9; i++)
        if ((value >> (8 - i)) & 1u) cw ^= p2_isch_rows[i];
    return cw;
}

static unsigned popcount64(uint64_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

int p2_isch_decode(uint64_t cw, uint8_t *errors)
{
    unsigned best = 41;
    int value = -1;
    cw &= UINT64_C(0xFFFFFFFFFF);
    if (cw == P2_S_ISCH) {
        if (errors) *errors = 0;
        return -2;
    }
    for (unsigned v = 0; v < 128; v++) {
        unsigned d = popcount64(cw ^ p2_isch_encode((uint8_t)v));
        if (d < best) {
            best = d;
            value = (int)v;
        }
    }
    if (best > 7) return -1;   /* (40,9,16): 7 correctable */
    if (errors) *errors = (uint8_t)best;
    return value;
}

/* (8,4) code: generator rows 10001101 01001011 00101110 00010111 (OP25 apps/tdma/duid.py) */
static const uint8_t duid_rows[4] = { 0x8D, 0x4B, 0x2E, 0x17 };

uint8_t p2_duid_encode(uint8_t duid)
{
    uint8_t cw = 0;
    for (unsigned i = 0; i < 4; i++)
        if ((duid >> (3 - i)) & 1u) cw ^= duid_rows[i];
    return cw;
}

int p2_duid_decode(uint8_t cw)
{
    for (unsigned d = 0; d < 16; d++)
        if (popcount64((uint64_t)(cw ^ p2_duid_encode((uint8_t)d))) <= 1) return (int)d;
    return -1;
}

/* ---- scrambling ---- */

void p2_mask(uint32_t wacn, uint16_t sysid, uint16_t nac, uint8_t mask[P2_MASK_BYTES])
{
    /* TIA-102.BBAC figure 7.1 as dsd-fme implements it: x^44 + x^40 + x^35 + x^29 + x^24 + x^10 + 1 taps */
    uint64_t r = ((uint64_t)(wacn & 0xFFFFFu) << 24 | (uint64_t)(sysid & 0xFFFu) << 12 | (nac & 0xFFFu));
    memset(mask, 0, P2_MASK_BYTES);
    for (unsigned i = 0; i < 4320; i++) {
        if ((r >> 43) & 1u) mask[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
        uint64_t fb = ((r >> 33) ^ (r >> 19) ^ (r >> 14) ^ (r >> 8) ^ (r >> 3) ^ (r >> 43)) & 1u;
        r = ((r << 1) | fb) & ((UINT64_C(1) << 44) - 1);
    }
}

/* ---- GF(64) Reed-Solomon (63,35) ---- */

enum { NN = 63, NROOTS = 28, KK = 35, A0 = 63 };
static uint8_t alpha_to[64], index_of[64], genpoly[NROOTS + 1];
static bool rs_ready;

static unsigned modnn(unsigned x)
{
    while (x >= NN) x -= NN;
    return x;
}

static void rs_init(void)
{
    if (rs_ready) return;
    unsigned sr = 1;
    for (unsigned i = 0; i < NN; i++) {
        index_of[sr] = (uint8_t)i;
        alpha_to[i] = (uint8_t)sr;
        sr <<= 1;
        if (sr & 0x40u) sr ^= 0x43u;
    }
    index_of[0] = A0;
    alpha_to[A0] = 0;
    /* g(x) = prod (x - alpha^(1+i)), i = 0..27, kept in index form */
    uint8_t g[NROOTS + 1] = { 1 };
    for (unsigned i = 0, root = 1; i < NROOTS; i++, root++) {
        g[i + 1] = 1;
        for (unsigned j = i; j > 0; j--)
            g[j] = g[j] ? (uint8_t)(g[j - 1] ^ alpha_to[modnn(index_of[g[j]] + root)]) : g[j - 1];
        g[0] = alpha_to[modnn(index_of[g[0]] + root)];
    }
    for (unsigned i = 0; i <= NROOTS; i++) genpoly[i] = index_of[g[i]];
    rs_ready = true;
}

void p2_rs_encode(uint8_t cw[63])
{
    rs_init();
    uint8_t *parity = cw + KK;
    memset(parity, 0, NROOTS);
    for (unsigned i = 0; i < KK; i++) {
        uint8_t fb = index_of[(cw[i] & 63u) ^ parity[0]];
        if (fb != A0) {
            for (unsigned j = 1; j < NROOTS; j++) parity[j] ^= alpha_to[modnn(fb + genpoly[NROOTS - j])];
        }
        memmove(parity, parity + 1, NROOTS - 1);
        parity[NROOTS - 1] = fb != A0 ? alpha_to[modnn(fb + genpoly[0])] : 0;
    }
}

int p2_rs_decode(uint8_t cw[63], const uint8_t *erasures, unsigned n_eras)
{
    rs_init();
    if (n_eras > NROOTS) return -1;
    uint8_t s[NROOTS], lambda[NROOTS + 1], b[NROOTS + 1], t[NROOTS + 1], omega[NROOTS + 1];
    uint8_t root[NROOTS], loc[NROOTS];
    /* syndromes: r(alpha^(1+i)), symbol 0 the highest degree */
    bool zero = true;
    for (unsigned i = 0; i < NROOTS; i++) {
        uint8_t v = 0;
        for (unsigned j = 0; j < NN; j++) {
            v = v ? alpha_to[modnn(index_of[v] + i + 1)] : 0;
            v ^= cw[j] & 63u;
        }
        s[i] = index_of[v];
        zero &= v == 0;
    }
    if (zero) return 0;
    /* erasure locator */
    memset(lambda, 0, sizeof lambda);
    lambda[0] = 1;
    for (unsigned i = 0; i < n_eras; i++) {
        uint8_t u = (uint8_t)modnn(NN - 1 - erasures[i]);
        for (unsigned j = i + 1; j > 0; j--) {
            uint8_t tmp = index_of[lambda[j - 1]];
            if (tmp != A0) lambda[j] ^= alpha_to[modnn(u + tmp)];
        }
    }
    for (unsigned i = 0; i <= NROOTS; i++) b[i] = index_of[lambda[i]];
    /* Berlekamp-Massey */
    unsigned r = n_eras, el = n_eras;
    while (++r <= NROOTS) {
        uint8_t discr = 0;
        for (unsigned i = 0; i < r; i++)
            if (lambda[i] && s[r - i - 1] != A0) discr ^= alpha_to[modnn(index_of[lambda[i]] + s[r - i - 1])];
        uint8_t dr = index_of[discr];
        if (dr == A0) {
            memmove(b + 1, b, NROOTS);
            b[0] = A0;
        } else {
            t[0] = lambda[0];
            for (unsigned i = 0; i < NROOTS; i++)
                t[i + 1] = b[i] != A0 ? (uint8_t)(lambda[i + 1] ^ alpha_to[modnn(dr + b[i])]) : lambda[i + 1];
            if (2 * el <= r + n_eras - 1) {
                el = r + n_eras - el;
                for (unsigned i = 0; i <= NROOTS; i++)
                    b[i] = lambda[i] == 0 ? A0 : (uint8_t)modnn(index_of[lambda[i]] - dr + NN);
            } else {
                memmove(b + 1, b, NROOTS);
                b[0] = A0;
            }
            memcpy(lambda, t, sizeof lambda);
        }
    }
    unsigned deg_lambda = 0;
    for (unsigned i = 0; i <= NROOTS; i++) {
        lambda[i] = index_of[lambda[i]];
        if (lambda[i] != A0) deg_lambda = i;
    }
    /* Chien search (Karn): root[] the exponents i with lambda(alpha^i) = 0, loc[] the code word index */
    uint8_t reg[NROOTS + 1];
    memcpy(reg + 1, lambda + 1, NROOTS);
    unsigned count = 0;
    for (unsigned i = 1; i <= NN; i++) {
        uint8_t q = 1;
        for (unsigned j = deg_lambda; j > 0; j--)
            if (reg[j] != A0) {
                reg[j] = (uint8_t)modnn(reg[j] + j);
                q ^= alpha_to[reg[j]];
            }
        if (q) continue;
        root[count] = (uint8_t)i;
        loc[count] = (uint8_t)(i - 1);
        if (++count == deg_lambda) break;
    }
    if (deg_lambda == 0 || count != deg_lambda) return -1;
    /* omega = s(x) lambda(x) mod x^NROOTS, index form */
    unsigned deg_omega = deg_lambda - 1;
    for (unsigned i = 0; i <= deg_omega; i++) {
        uint8_t tmp = 0;
        for (int j = (int)i; j >= 0; j--)
            if (s[i - (unsigned)j] != A0 && lambda[j] != A0) tmp ^= alpha_to[modnn(s[i - (unsigned)j] + lambda[j])];
        omega[i] = index_of[tmp];
    }
    /* Forney, first root 1: e = omega(X^-1) / lambda'(X^-1) */
    for (int j = (int)count - 1; j >= 0; j--) {
        uint8_t num1 = 0, den = 0;
        for (int i = (int)deg_omega; i >= 0; i--)
            if (omega[i] != A0) num1 ^= alpha_to[modnn(omega[i] + (unsigned)i * root[j] % NN)];
        unsigned top = deg_lambda < NROOTS - 1 ? deg_lambda : NROOTS - 1;
        for (int i = (int)(top & ~1u); i >= 0; i -= 2)
            if (lambda[i + 1] != A0) den ^= alpha_to[modnn(lambda[i + 1] + (unsigned)i * root[j] % NN)];
        if (den == 0) return -1;
        if (num1) cw[loc[j]] ^= alpha_to[modnn(index_of[num1] + NN - index_of[den])];
    }
    return (int)count;
}

/* ---- CRC-12 ---- */

uint16_t p2_crc12(const uint8_t *bytes, unsigned bits)
{
    uint16_t crc = 0;
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = (bytes[i >> 3] >> (7 - (i & 7u))) & 1u;
        unsigned top = (crc >> 11) & 1u;
        crc = (uint16_t)((crc << 1) & 0xFFFu);
        if (top ^ bit) crc ^= 0x897u;
    }
    return crc ^ 0xFFFu;
}

/* ---- payload fields ---- */

static const struct { uint8_t at, n; } sacch_runs[3] = { { 1, 36 }, { 38, 84 }, { 123, 36 } };
static const struct { uint8_t at, n; } facch_runs[4] = { { 1, 36 }, { 38, 31 }, { 90, 32 }, { 123, 36 } };

static unsigned field_dibits(bool facch, unsigned k)
{
    /* the k-th dibit of the field -> payload index */
    unsigned n = facch ? 4 : 3;
    for (unsigned i = 0; i < n; i++) {
        unsigned at = facch ? facch_runs[i].at : sacch_runs[i].at, len = facch ? facch_runs[i].n : sacch_runs[i].n;
        if (k < len) return at + k;
        k -= len;
    }
    return 0;
}

static unsigned get_bits(const uint8_t *b, unsigned at, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++) v = v << 1 | ((b[(at + i) >> 3] >> (7 - ((at + i) & 7u))) & 1u);
    return v;
}

static void put_bits(uint8_t *b, unsigned at, unsigned n, unsigned v)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = (v >> (n - 1 - i)) & 1u, p = at + i;
        if (bit) b[p >> 3] |= (uint8_t)(0x80u >> (p & 7u));
        else b[p >> 3] &= (uint8_t)~(0x80u >> (p & 7u));
    }
}

/* SACCH: 30 data hex bits at 5..34, parity 35..56 sent; FACCH: 26 at 9..34, parity 35..53 sent */
#define XCCH_FIRST(f) ((f) ? 9u : 5u)
#define XCCH_SENT(f) ((f) ? 45u : 52u)

void p2_xcch_encode(bool facch, const uint8_t *pdu, uint8_t payload[P2_PAYLOAD_DIBITS])
{
    unsigned pdu_bits = facch ? 144 : 168;
    uint8_t data[24] = { 0 };
    memcpy(data, pdu, pdu_bits / 8);
    put_bits(data, pdu_bits, 12, p2_crc12(pdu, pdu_bits));
    uint8_t cw[63] = { 0 };
    unsigned first = XCCH_FIRST(facch);
    for (unsigned i = 0; i < (pdu_bits + 12) / 6; i++) cw[first + i] = (uint8_t)get_bits(data, 6 * i, 6);
    p2_rs_encode(cw);
    for (unsigned h = 0; h < XCCH_SENT(facch); h++) {
        unsigned v = cw[first + h];
        for (unsigned d = 0; d < 3; d++) payload[field_dibits(facch, 3 * h + d)] = (uint8_t)((v >> (4 - 2 * d)) & 3u);
    }
}

int p2_xcch_decode(bool facch, const uint8_t payload[P2_PAYLOAD_DIBITS], uint8_t *pdu)
{
    uint8_t cw[63] = { 0 }, eras[18];
    unsigned first = XCCH_FIRST(facch), sent = XCCH_SENT(facch), n = 0;
    for (unsigned h = 0; h < sent; h++) {
        unsigned v = 0;
        for (unsigned d = 0; d < 3; d++) v = v << 2 | (payload[field_dibits(facch, 3 * h + d)] & 3u);
        cw[first + h] = (uint8_t)v;
    }
    /* as OP25: the shortened and the punctured symbols are erasures */
    for (unsigned i = 0; i < first; i++) eras[n++] = (uint8_t)i;
    for (unsigned i = first + sent; i < 63; i++) eras[n++] = (uint8_t)i;
    int fixed = p2_rs_decode(cw, eras, n);
    if (fixed < 0) return -1;
    for (unsigned i = 0; i < first; i++)
        if (cw[i]) return -1;   /* a shortened symbol decoded nonzero: not a code word of ours */
    unsigned pdu_bits = facch ? 144 : 168;
    uint8_t data[24] = { 0 };
    for (unsigned i = 0; i < (pdu_bits + 12) / 6; i++) put_bits(data, 6 * i, 6, cw[first + i]);
    if (get_bits(data, pdu_bits, 12) != p2_crc12(data, pdu_bits)) return -1;
    memcpy(pdu, data, pdu_bits / 8);
    fixed -= (int)n;
    return fixed < 0 ? 0 : fixed;
}

/* ---- ESS ---- */

void p2_ess_encode(uint8_t algid, uint16_t keyid, const uint8_t mi[9], uint8_t hex[P2_ESS_HEX])
{
    uint8_t bits[12];
    bits[0] = algid;
    bits[1] = (uint8_t)(keyid >> 8);
    bits[2] = (uint8_t)keyid;
    memcpy(bits + 3, mi, 9);
    uint8_t cw[63] = { 0 };
    for (unsigned i = 0; i < 16; i++) cw[19 + i] = (uint8_t)get_bits(bits, 6 * i, 6);
    p2_rs_encode(cw);
    memcpy(hex, cw + 19, P2_ESS_HEX);
}

int p2_ess_decode(uint8_t hex[P2_ESS_HEX], uint8_t *algid, uint16_t *keyid, uint8_t mi[9])
{
    /* shortened by 19: those symbols are known zeros (as OP25's ezpwd RS<63,35> decode of ESS-B, ESS-A) */
    uint8_t cw[63] = { 0 };
    memcpy(cw + 19, hex, P2_ESS_HEX);
    int fixed = p2_rs_decode(cw, NULL, 0);
    if (fixed < 0) return -1;
    for (unsigned i = 0; i < 19; i++)
        if (cw[i]) return -1;
    memcpy(hex, cw + 19, P2_ESS_HEX);
    uint8_t bits[12] = { 0 };
    for (unsigned i = 0; i < 16; i++) put_bits(bits, 6 * i, 6, cw[19 + i]);
    *algid = bits[0];
    *keyid = (uint16_t)(bits[1] << 8 | bits[2]);
    memcpy(mi, bits + 3, 9);
    return fixed;
}

/* ---- voice bursts ---- */

static const uint8_t vcw_at[4] = { 1, 38, 86, 123 };

static void put_vcw(const uint8_t v[P2_VCW_BYTES], uint8_t *payload)
{
    for (unsigned i = 0; i < 36; i++) payload[i] = (uint8_t)((v[i >> 2] >> (6 - 2 * (i & 3u))) & 3u);
}

static void get_vcw(const uint8_t *payload, uint8_t v[P2_VCW_BYTES])
{
    memset(v, 0, P2_VCW_BYTES);
    for (unsigned i = 0; i < 36; i++) v[i >> 2] |= (uint8_t)((payload[i] & 3u) << (6 - 2 * (i & 3u)));
}

static void put_hex(uint8_t *payload, unsigned at, uint8_t h)
{
    payload[at] = (uint8_t)((h >> 4) & 3u);
    payload[at + 1] = (uint8_t)((h >> 2) & 3u);
    payload[at + 2] = (uint8_t)(h & 3u);
}

static uint8_t get_hex(const uint8_t *payload, unsigned at)
{
    return (uint8_t)((payload[at] & 3u) << 4 | (payload[at + 1] & 3u) << 2 | (payload[at + 2] & 3u));
}

void p2_4v_encode(const uint8_t vcw[4][P2_VCW_BYTES], const uint8_t essb[4], uint8_t payload[P2_PAYLOAD_DIBITS])
{
    for (unsigned k = 0; k < 4; k++) put_vcw(vcw[k], payload + vcw_at[k]);
    for (unsigned k = 0; k < 4; k++) put_hex(payload, 74 + 3 * k, essb[k]);
}

void p2_4v_decode(const uint8_t payload[P2_PAYLOAD_DIBITS], uint8_t vcw[4][P2_VCW_BYTES], uint8_t essb[4])
{
    for (unsigned k = 0; k < 4; k++) get_vcw(payload + vcw_at[k], vcw[k]);
    for (unsigned k = 0; k < 4; k++) essb[k] = get_hex(payload, 74 + 3 * k);
}

static unsigned essa_at(unsigned i)
{
    return i < 16 ? 74 + 3 * i : 123 + 3 * (i - 16);
}

void p2_2v_encode(const uint8_t vcw[2][P2_VCW_BYTES], const uint8_t essa[28], uint8_t payload[P2_PAYLOAD_DIBITS])
{
    put_vcw(vcw[0], payload + 1);
    put_vcw(vcw[1], payload + 38);
    for (unsigned i = 0; i < 28; i++) put_hex(payload, essa_at(i), essa[i]);
}

void p2_2v_decode(const uint8_t payload[P2_PAYLOAD_DIBITS], uint8_t vcw[2][P2_VCW_BYTES], uint8_t essa[28])
{
    get_vcw(payload + 1, vcw[0]);
    get_vcw(payload + 38, vcw[1]);
    for (unsigned i = 0; i < 28; i++) essa[i] = get_hex(payload, essa_at(i));
}

/* ---- bursts ---- */

static const uint8_t duid_at[4] = { 0, 37, 122, 159 };

static bool is_duid_pos(unsigned p)
{
    return p == 0 || p == 37 || p == 122 || p == 159;
}

void p2_burst_build(uint64_t isch, uint8_t duid, const uint8_t payload[P2_PAYLOAD_DIBITS], const uint8_t *mask,
                    unsigned burst, uint8_t out[P2_BURST_DIBITS])
{
    for (unsigned i = 0; i < P2_ISCH_DIBITS; i++) out[i] = (uint8_t)((isch >> (38 - 2 * i)) & 3u);
    bool scramble = mask && p2_duid_scrambled(duid);
    for (unsigned p = 0; p < P2_PAYLOAD_DIBITS; p++) {
        uint8_t d = payload[p] & 3u;
        if (scramble && !is_duid_pos(p)) d ^= p2_mask_dibit(mask, 180u * burst + 10u + p);
        out[P2_ISCH_DIBITS + p] = d;
    }
    uint8_t cw = p2_duid_encode(duid);
    for (unsigned k = 0; k < 4; k++) out[P2_ISCH_DIBITS + duid_at[k]] = (uint8_t)((cw >> (6 - 2 * k)) & 3u);
}

int p2_burst_parse(const uint8_t in[P2_BURST_DIBITS], const uint8_t *mask, unsigned burst,
                   uint8_t payload[P2_PAYLOAD_DIBITS])
{
    const uint8_t *p = in + P2_ISCH_DIBITS;
    uint8_t cw = (uint8_t)((p[0] & 3u) << 6 | (p[37] & 3u) << 4 | (p[122] & 3u) << 2 | (p[159] & 3u));
    int duid = p2_duid_decode(cw);
    bool scramble = duid >= 0 && mask && p2_duid_scrambled((uint8_t)duid);
    for (unsigned i = 0; i < P2_PAYLOAD_DIBITS; i++) {
        uint8_t d = p[i] & 3u;
        if (scramble && !is_duid_pos(i)) d ^= p2_mask_dibit(mask, 180u * burst + 10u + i);
        payload[i] = d;
    }
    return duid;
}

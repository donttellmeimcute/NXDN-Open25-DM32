// SPDX-License-Identifier: GPL-3.0-or-later
// src/nxdn against MMDVMHost's NXDN classes (G4KLX, GPL-2.0-or-later, pinned in docs/protocol-sources.json), compiled
// on the host as an oracle: LICH, SACCH, FACCH1, CRC, layer 3, scrambler and the 72-bit voice frames must be
// bit-identical in both directions, including over channel errors.
#include "NXDNLICH.h"
#include "NXDNSACCH.h"
#include "NXDNFACCH1.h"
#include "NXDNAudio.h"
#include "NXDNDefines.h"
extern "C" {
#include "nxdn.h"
#include "nxdn_call.h"
#include "dmr/ambe/ambeFec.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>

static uint32_t rnd = 0xC0FFEE11u;
static uint32_t next() { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

// MMDVMHost's scrambler table (NXDNControl.cpp): our PN9 must equal it
static const unsigned char MMDVM_SCRAMBLER[] = {
	0x00U, 0x00U, 0x00U, 0x82U, 0xA0U, 0x88U, 0x8AU, 0x00U, 0xA2U, 0xA8U, 0x82U, 0x8AU, 0x82U, 0x02U,
	0x20U, 0x08U, 0x8AU, 0x20U, 0xAAU, 0xA2U, 0x82U, 0x08U, 0x22U, 0x8AU, 0xAAU, 0x08U, 0x28U, 0x88U,
	0x28U, 0x28U, 0x00U, 0x0AU, 0x02U, 0x82U, 0x20U, 0x28U, 0x82U, 0x2AU, 0xAAU, 0x20U, 0x22U, 0x80U,
	0xA8U, 0x8AU, 0x08U, 0xA0U, 0xAAU, 0x02U };

static void flip(uint8_t *b, unsigned from, unsigned bits, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		unsigned k = from + next() % bits;
		b[k >> 3] ^= (uint8_t)(0x80u >> (k & 7));
	}
}

int main()
{
	assert(memcmp(nxdn_scrambler_mask(), MMDVM_SCRAMBLER, 48) == 0);

	// LICH: every value, both ways
	for (unsigned v = 0; v < 128; v++) {
		nxdn_lich l = { (uint8_t)(v >> 5), (uint8_t)((v >> 3) & 3), (uint8_t)((v >> 1) & 3), (uint8_t)(v & 1) };
		uint8_t ours[48] = { 0 }, theirs[48] = { 0 };
		nxdn_lich_put(ours, nxdn_lich_pack(&l));
		CNXDNLICH m;
		m.setRFCT(l.rfct); m.setFCT(l.fct); m.setOption(l.option); m.setDirection(l.direction);
		m.encode(theirs);
		// MMDVMHost computes parity only for the RFCT/FCT combinations it sends (0x80, 0xB0 upper nibbles); compare
		// the frames where its rule and the XOR of RFCT and FCT agree, and decode ours with it everywhere
		uint8_t up = nxdn_lich_pack(&l) & 0xF0;
		bool mm_odd = up == 0x80 || up == 0xB0;
		bool xor_odd = (((up >> 7) ^ (up >> 6) ^ (up >> 5) ^ (up >> 4)) & 1) != 0;
		if (mm_odd == xor_odd) assert(memcmp(ours, theirs, 48) == 0);
		CNXDNLICH d;
		bool ok = d.decode(ours);
		assert(!(mm_odd == xor_odd) || ok);
		assert(d.getRFCT() == l.rfct && d.getFCT() == l.fct && d.getOption() == l.option && d.getDirection() == l.direction);
	}

	unsigned sacch_cases = 0, facch_cases = 0, err_cases = 0, sacch_disagree = 0, facch_disagree = 0;
	unsigned sacch_better = 0, facch_better = 0;
	for (unsigned it = 0; it < 2000; it++) {
		// SACCH: ours encodes, MMDVMHost decodes, and the other way, then both with errors
		nxdn_sacch s = { (uint8_t)(next() & 63), (uint8_t)(next() & 3), next() & 0x3FFFF };
		uint8_t ours[48] = { 0 }, theirs[48] = { 0 };
		nxdn_sacch_encode(&s, ours);
		CNXDNSACCH ms;
		uint8_t raw[5] = { 0 };
		raw[0] = (uint8_t)(s.structure << 6 | s.ran);
		raw[1] = (uint8_t)(s.data >> 10); raw[2] = (uint8_t)(s.data >> 2); raw[3] = (uint8_t)((s.data & 3) << 6);
		ms.setRaw(raw);
		ms.encode(theirs);
		assert(memcmp(ours, theirs, 48) == 0);
		flip(ours, NXDN_SACCH_OFFSET, NXDN_SACCH_BITS, it % 4);
		nxdn_sacch got;
		bool ok = nxdn_sacch_decode(ours, &got, NULL);
		CNXDNSACCH md;
		bool mok = md.decode(ours);
		// Past the code's reach the two decoders may land on different words: count who got it right
		bool ours_right = ok && got.ran == s.ran && got.structure == s.structure && got.data == s.data;
		bool theirs_right = mok && md.getRAN() == s.ran;
		sacch_disagree += theirs_right && !ours_right;
		sacch_better += ours_right && !theirs_right;
		if (ok && mok) {
			assert(got.ran == md.getRAN() && got.structure == md.getStructure());
			uint8_t d18[3] = { 0 };
			md.getData(d18);
			assert(got.data == ((uint32_t)d18[0] << 10 | (uint32_t)d18[1] << 2 | (uint32_t)d18[2] >> 6));
		}
		if (it % 4 <= 1) assert(ok && got.ran == s.ran && got.data == s.data);   // one error always corrected
		sacch_cases++;

		// FACCH1
		uint8_t l3[10];
		for (unsigned i = 0; i < 10; i++) l3[i] = (uint8_t)next();
		memset(ours, 0, 48); memset(theirs, 0, 48);
		unsigned which = it & 1;
		nxdn_facch1_encode(l3, ours, which);
		CNXDNFACCH1 mf;
		mf.setData(l3);
		mf.encode(theirs, NXDN_PAYLOAD_OFFSET + which * NXDN_FACCH1_BITS);
		assert(memcmp(ours, theirs, 48) == 0);
		flip(ours, NXDN_PAYLOAD_OFFSET + which * 144, 144, it % 6);
		uint8_t back[10];
		ok = nxdn_facch1_decode(ours, which, back, NULL);
		CNXDNFACCH1 mfd;
		mok = mfd.decode(ours, NXDN_PAYLOAD_OFFSET + which * NXDN_FACCH1_BITS);
		uint8_t mback[10] = { 0 };
		if (mok) mfd.getData(mback);
		bool f_ours = ok && memcmp(back, l3, 10) == 0, f_theirs = mok && memcmp(mback, l3, 10) == 0;
		facch_disagree += f_theirs && !f_ours;
		facch_better += f_ours && !f_theirs;
		if (f_ours && f_theirs) assert(memcmp(back, mback, 10) == 0);
		if (it % 6 <= 1) assert(ok && memcmp(back, l3, 10) == 0);   // two or more: see docs/nxdn-modem.md
		facch_cases++;
		err_cases += it % 6;

		// Voice: RadioFoundry's AMBE+2 FEC (shared with DMR) against MMDVMHost's NXDN audio coding
		uint8_t params[2][49], packed[13] = { 0 }, theirs_v[18], ours_v[18];
		for (unsigned f = 0; f < 2; f++)
			for (unsigned i = 0; i < 49; i++) {
				params[f][i] = next() & 1;
				unsigned k = f * 49 + i;
				packed[k >> 3] |= (uint8_t)(params[f][i] << (7 - (k & 7)));
			}
		CNXDNAudio audio;
		audio.encode(packed, theirs_v);
		ambeFecEncode(params[0], ours_v);
		ambeFecEncode(params[1], ours_v + 9);
		assert(memcmp(ours_v, theirs_v, 18) == 0);
	}

	// A whole call built by our TX, read by MMDVMHost's classes frame by frame
	nxdn_tx tx;
	nxdn_tx_cfg cfg = { 12, 4001, 200, false, false, NXDN_DIR_INBOUND };
	nxdn_tx_begin(&tx, &cfg, 1);
	uint8_t slot[9];
	unsigned frames = 0, voice = 0;
	for (unsigned k = 0; k < 10; k++) {
		for (unsigned i = 0; i < 9; i++) slot[i] = (uint8_t)next();
		if (!nxdn_tx_voice(&tx, slot)) k--;
		uint8_t f[48];
		while (nxdn_tx_frame(&tx, f) == 1) {
			frames++;
			uint8_t sync[3] = { 0xCD, 0xF5, 0x90 };
			assert(f[0] == sync[0] && f[1] == sync[1] && (f[2] & 0xF0) == sync[2]);
			for (unsigned i = 0; i < 48; i++) f[i] ^= MMDVM_SCRAMBLER[i];
			CNXDNLICH l;
			assert(l.decode(f) && l.getRFCT() == NXDN_LICH_RFCT_RDCH);
			CNXDNSACCH s;
			assert(s.decode(f) && s.getRAN() == 12);
			if (l.getFCT() == NXDN_LICH_USC_SACCH_SS) voice++;
			else {
				CNXDNFACCH1 h;
				assert(h.decode(f, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS + NXDN_SACCH_LENGTH_BITS));
				uint8_t d[10];
				h.getData(d);
				assert((d[0] & 0x3F) == NXDN_MESSAGE_TYPE_VCALL && ((d[3] << 8) | d[4]) == 4001 && ((d[5] << 8) | d[6]) == 200);
			}
		}
	}
	nxdn_tx_end(&tx);
	uint8_t f[48];
	unsigned rel = 0;
	while (nxdn_tx_frame(&tx, f) == 1) {
		for (unsigned i = 0; i < 48; i++) f[i] ^= MMDVM_SCRAMBLER[i];
		CNXDNLICH l;
		assert(l.decode(f));
		if (l.getFCT() == NXDN_LICH_USC_SACCH_NS) {
			CNXDNFACCH1 h;
			assert(h.decode(f, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS + NXDN_SACCH_LENGTH_BITS + NXDN_FACCH1_LENGTH_BITS));
			uint8_t d[10];
			h.getData(d);
			rel += (d[0] & 0x3F) == NXDN_MESSAGE_TYPE_TX_REL;
		} else voice++;
	}
	assert(frames == 3 && voice == 3 && rel == 1 && tx.padded_slots == 2);
	printf("PASS NXDN frame layer equals MMDVMHost: scrambler, 128 LICH, %u SACCH, %u FACCH1 (%u injected bit errors; "
	       "see below for frames past the codes' reach), voice coding, a whole call\n",
	       sacch_cases, facch_cases, err_cases);
	// Ours is never worse overall: forcing the flushed end state and true erasures
	printf("  decoded right only by MMDVMHost: %u SACCH, %u FACCH1; only by ours: %u SACCH, %u FACCH1\n",
	       sacch_disagree, facch_disagree, sacch_better, facch_better);
	assert(sacch_disagree <= sacch_better && facch_disagree <= facch_better);
	return 0;
}

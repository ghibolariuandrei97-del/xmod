// lde.cpp - x86-32 instruction length decoder with operand information.
// Used to build relocatable trampolines and to know which bytes of a signature must be wildcards
// (relative branches, absolute addresses). Validated against objdump on real game/engine binaries.
#include "codex.h"
#include <cstdio>

// one-byte opcode flags
enum : uint8_t {
	F_NONE = 0, F_MODRM = 1, F_I8 = 2, F_I32 = 4 /* imm16/32 by operand size */, F_I16 = 8,
	F_REL8 = 16, F_REL32 = 32, F_MOFFS = 64, F_SPECIAL = 128
};
#define M  F_MODRM
#define I8 F_I8
#define IZ F_I32
static const uint8_t OP1[256] = {
	/*00*/ M, M, M, M, I8, IZ, 0, 0, M, M, M, M, I8, IZ, 0, F_SPECIAL /*0F*/,
	/*10*/ M, M, M, M, I8, IZ, 0, 0, M, M, M, M, I8, IZ, 0, 0,
	/*20*/ M, M, M, M, I8, IZ, F_SPECIAL, 0, M, M, M, M, I8, IZ, F_SPECIAL, 0,
	/*30*/ M, M, M, M, I8, IZ, F_SPECIAL, 0, M, M, M, M, I8, IZ, F_SPECIAL, 0,
	/*40*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/*50*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/*60*/ 0, 0, M, M, F_SPECIAL, F_SPECIAL, F_SPECIAL, F_SPECIAL, IZ, M | IZ, I8, M | I8, 0, 0, 0, 0,
	/*70*/ F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8,
	/*80*/ M | I8, M | IZ, M | I8, M | I8, M, M, M, M, M, M, M, M, M, M, M, M,
	/*90*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, F_SPECIAL /*9A far*/, 0, 0, 0, 0, 0,
	/*A0*/ F_MOFFS, F_MOFFS, F_MOFFS, F_MOFFS, 0, 0, 0, 0, I8, IZ, 0, 0, 0, 0, 0, 0,
	/*B0*/ I8, I8, I8, I8, I8, I8, I8, I8, IZ, IZ, IZ, IZ, IZ, IZ, IZ, IZ,
	/*C0*/ M | I8, M | I8, F_I16, 0, M, M, M | I8, M | IZ, F_SPECIAL /*enter*/, 0, F_I16, 0, 0, I8, 0, 0,
	/*D0*/ M, M, M, M, I8, I8, 0, 0, M, M, M, M, M, M, M, M,
	/*E0*/ F_REL8, F_REL8, F_REL8, F_REL8, I8, I8, I8, I8, F_REL32, F_REL32, F_SPECIAL /*EA far*/, F_REL8, 0, 0, 0, 0,
	/*F0*/ F_SPECIAL, 0, F_SPECIAL, F_SPECIAL, 0, 0, F_SPECIAL /*F6*/, F_SPECIAL /*F7*/, 0, 0, 0, 0, 0, 0, M, M,
};
#undef M
#undef I8
#undef IZ

// two-byte (0F xx): 1 = modrm, 2 = modrm + imm8, 3 = rel32, 0 = nothing
static uint8_t op2(uint8_t b)
{
	switch (b) {
	case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x37:
	case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
		return 0;
	case 0x70: case 0x71: case 0x72: case 0x73: case 0xA4: case 0xAC: case 0xBA:
	case 0xC2: case 0xC4: case 0xC5: case 0xC6:
		return 2;
	default:
		if (b >= 0x80 && b <= 0x8F) return 3;
		if (b >= 0xC8 && b <= 0xCF) return 0;   // bswap
		return 1;
	}
}

static inline int32_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static inline int16_t rd16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }

// returns bytes consumed by modrm (+ sib + displacement), fills operand info
static int decode_modrm(const uint8_t *p, int off, Insn &in)
{
	uint8_t m = p[off];
	in.has_modrm = true;
	in.mod = m >> 6; in.reg = (m >> 3) & 7; in.rm = m & 7;
	int n = 1;
	if (in.mod == 3) return n;
	if (in.addr16) {
		if (in.mod == 0 && in.rm == 6) { in.disp_off = off + n; in.disp_size = 2; in.disp = rd16(p + off + n); in.disp_abs = true; n += 2; }
		else if (in.mod == 1) { in.disp_off = off + n; in.disp_size = 1; in.disp = (int8_t)p[off + n]; n += 1; }
		else if (in.mod == 2) { in.disp_off = off + n; in.disp_size = 2; in.disp = rd16(p + off + n); n += 2; }
		return n;
	}
	if (in.rm == 4) {                         // SIB
		uint8_t sib = p[off + n]; n++;
		in.sib_base = sib & 7;
		if (in.mod == 0 && (sib & 7) == 5) {   // [index*s + disp32]
			in.disp_off = off + n; in.disp_size = 4; in.disp = rd32(p + off + n); in.disp_abs = true; n += 4;
			return n;
		}
	}
	if (in.mod == 0 && in.rm == 5) { in.disp_off = off + n; in.disp_size = 4; in.disp = rd32(p + off + n); in.disp_abs = true; n += 4; }
	else if (in.mod == 1) { in.disp_off = off + n; in.disp_size = 1; in.disp = (int8_t)p[off + n]; n += 1; }
	else if (in.mod == 2) { in.disp_off = off + n; in.disp_size = 4; in.disp = rd32(p + off + n); n += 4; }
	return n;
}

static void set_imm(Insn &in, const uint8_t *p, int off, int size)
{
	in.imm_off = off; in.imm_size = size;
	in.imm = size == 1 ? (int8_t)p[off] : size == 2 ? rd16(p + off) : rd32(p + off);
}

bool lde_decode(const uint8_t *p, Insn &in)
{
	in = Insn();
	int i = 0;
	bool rep = false;
	// prefixes
	for (;;) {
		uint8_t b = p[i];
		if (b == 0x66) in.opsize16 = true;
		else if (b == 0x67) in.addr16 = true;
		else if (b == 0xF2 || b == 0xF3) rep = true;
		else if (b == 0xF0 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {}
		else break;
		if (++i > 14) return false;
	}
	(void)rep;
	in.prefixes = i;
	uint8_t op = p[i++];
	int zsize = in.opsize16 ? 2 : 4;

	// VEX (C4/C5) and EVEX (62): in 32-bit mode these bytes are LES/LDS/BOUND unless the next byte has mod == 11
	if ((op == 0xC4 || op == 0xC5 || op == 0x62) && (p[i] & 0xC0) == 0xC0) {
		int map = 1;                         // 1 = 0F, 2 = 0F38, 3 = 0F3A
		if (op == 0xC5) { i += 1; }
		else if (op == 0xC4) { map = p[i] & 0x1F; i += 2; }
		else { map = p[i] & 0x03; i += 3; }   // EVEX: P0.mm
		uint8_t vop = p[i++];
		in.opcode = (map == 2 ? 0x0F3800 : map == 3 ? 0x0F3A00 : 0x0F00) | vop;
		bool modrm = !(map == 1 && vop == 0x77);   // vzeroupper / vzeroall
		if (modrm) i += decode_modrm(p, i, in);
		bool imm8 = map == 3 || (map == 1 && op2(vop) == 2);
		if (imm8) { set_imm(in, p, i, 1); i += 1; }
		in.len = i;
		in.valid = in.len <= 15;
		return in.valid;
	}

	if (op == 0x0F) {
		uint8_t b2 = p[i++];
		if (b2 == 0x38 || b2 == 0x3A) {                  // three-byte opcodes: always modrm, 0F 3A has imm8
			uint8_t b3 = p[i++];
			in.opcode = (b2 == 0x38 ? 0x0F3800 : 0x0F3A00) | b3;
			i += decode_modrm(p, i, in);
			if (b2 == 0x3A) { set_imm(in, p, i, 1); i += 1; }
		} else if (b2 == 0x0F) {                          // 3DNow!: modrm + imm8 suffix
			in.opcode = 0x0F0F;
			i += decode_modrm(p, i, in);
			set_imm(in, p, i, 1); i += 1;
		} else {
			in.opcode = 0x0F00 | b2;
			uint8_t f = op2(b2);
			if (f == 3) {
				in.rel_off = i; in.rel_size = zsize; in.rel = zsize == 4 ? rd32(p + i) : rd16(p + i); i += zsize;
				in.is_jcc = true;
			} else if (f) {
				i += decode_modrm(p, i, in);
				if (f == 2) { set_imm(in, p, i, 1); i += 1; }
			}
		}
		in.len = i;
		in.valid = in.len <= 15;
		return in.valid;
	}

	in.opcode = op;
	uint8_t f = OP1[op];
	if (f == F_SPECIAL) {
		switch (op) {
		case 0x9A: case 0xEA:                             // far call / jmp ptr16:32
			i += zsize + 2; break;
		case 0xC8:                                        // enter imm16, imm8
			set_imm(in, p, i, 2); i += 3; break;
		case 0xF6:                                        // test r/m8, imm8 (only /0 /1)
			i += decode_modrm(p, i, in);
			if (in.reg <= 1) { set_imm(in, p, i, 1); i += 1; }
			break;
		case 0xF7:
			i += decode_modrm(p, i, in);
			if (in.reg <= 1) { set_imm(in, p, i, zsize); i += zsize; }
			break;
		default:
			return false;                                 // prefix byte seen as opcode: cannot happen
		}
	} else {
		if (f & F_MODRM) i += decode_modrm(p, i, in);
		if (f & F_I8) { set_imm(in, p, i, 1); i += 1; }
		if (f & F_I16) { set_imm(in, p, i, 2); i += 2; }
		if (f & F_I32) { set_imm(in, p, i, zsize); i += zsize; }
		if (f & F_MOFFS) {                                // mov al/eax <-> [moffs]: an absolute address
			int as = in.addr16 ? 2 : 4;
			in.disp_off = i; in.disp_size = as; in.disp = as == 4 ? rd32(p + i) : rd16(p + i); in.disp_abs = true;
			i += as;
		}
		if (f & F_REL8) { in.rel_off = i; in.rel_size = 1; in.rel = (int8_t)p[i]; i += 1; }
		if (f & F_REL32) { in.rel_off = i; in.rel_size = zsize; in.rel = zsize == 4 ? rd32(p + i) : rd16(p + i); i += zsize; }
	}
	in.is_ret = op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA;
	in.is_call = op == 0xE8;
	in.is_jmp = op == 0xE9 || op == 0xEB;
	in.is_jcc = (op >= 0x70 && op <= 0x7F);
	in.is_loop = (op >= 0xE0 && op <= 0xE3);
	in.len = i;
	in.valid = in.len <= 15;
	return in.valid;
}

std::string hex_bytes(const uint8_t *p, size_t n)
{
	std::string s;
	char b[4];
	for (size_t i = 0; i < n; ++i) { snprintf(b, sizeof b, i ? " %02X" : "%02X", p[i]); s += b; }
	return s;
}

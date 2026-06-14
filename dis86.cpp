// license:BSD-3-Clause
// copyright-holders:Stephen Hurd
// Inspired by VirtualT (Copyright 2004 Stephen Hurd and Ken Pettit, BSD-2-Clause)
#include "dis86.h"
#include <cstdio>
#include <cstring>

static constexpr const char *r8[]  = {"AL","CL","DL","BL","AH","CH","DH","BH"};
static constexpr const char *r16[] = {"AX","CX","DX","BX","SP","BP","SI","DI"};
static constexpr const char *sr[]  = {"ES","CS","SS","DS"};
static constexpr const char *alu[] = {"ADD","OR","ADC","SBB","AND","SUB","XOR","CMP"};
static constexpr const char *shf[] = {"ROL","ROR","RCL","RCR","SHL","SHR","SAL","SAR"};
static constexpr const char *cc[]  = {
	"JO","JNO","JB","JNB","JZ","JNZ","JBE","JNBE",
	"JS","JNS","JP","JNP","JL","JNL","JLE","JNLE"
};

struct dis_state {
	const uint8_t *src;
	int len;
	int pos;
};

static uint8_t fetch(dis_state *st) { return (st->pos < st->len) ? st->src[st->pos++] : 0; }
static uint16_t fetch16(dis_state *st) { uint8_t lo=fetch(st); return (uint16_t)(lo | (fetch(st)<<8)); }

static int modrm_ea(dis_state *st, char *buf, int w)
{
	uint8_t b = fetch(st);
	int mod = b >> 6, rm = b & 7, reg = (b >> 3) & 7;
	char ea[64];

	if (mod == 3) {
		snprintf(ea, sizeof(ea), "%s", w ? r16[rm] : r8[rm]);
	} else {
		static constexpr const char *base[] = {
			"BX+SI","BX+DI","BP+SI","BP+DI","SI","DI","BP","BX"
		};
		if (mod == 0 && rm == 6) {
			snprintf(ea, sizeof(ea), "[%04Xh]", fetch16(st));
		} else if (mod == 0) {
			snprintf(ea, sizeof(ea), "[%s]", base[rm]);
		} else if (mod == 1) {
			int8_t d = (int8_t)fetch(st);
			if (d >= 0)
				snprintf(ea, sizeof(ea), "[%s+%02Xh]", base[rm], d);
			else
				snprintf(ea, sizeof(ea), "[%s-%02Xh]", base[rm], -d);
		} else {
			snprintf(ea, sizeof(ea), "[%s+%04Xh]", base[rm], fetch16(st));
		}
	}
	snprintf(buf, 64, "%s", ea);
	return reg;
}

int dis86(const uint8_t *code, int len, uint16_t ip, char *buf, int bufsz)
{
	dis_state st = { code, len, 0 };
	char op[128] = "";
	char prefix[16] = "";

	/* prefixes */
pfx:;
	uint8_t b = fetch(&st);
	switch (b) {
	case 0x26: snprintf(prefix,sizeof(prefix),"ES:"); goto pfx;
	case 0x2e: snprintf(prefix,sizeof(prefix),"CS:"); goto pfx;
	case 0x36: snprintf(prefix,sizeof(prefix),"SS:"); goto pfx;
	case 0x3e: snprintf(prefix,sizeof(prefix),"DS:"); goto pfx;
	case 0xf0: snprintf(prefix,sizeof(prefix),"LOCK "); goto pfx;
	case 0xf2: snprintf(prefix,sizeof(prefix),"REPNE "); goto pfx;
	case 0xf3: snprintf(prefix,sizeof(prefix),"REP "); goto pfx;
	default: break;
	}

	switch (b) {

	/* ALU r/m,r and r,r/m and acc,imm */
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
	case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
	case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
	case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
	case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
	case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
		int a = (b>>3)&7, sub = b&7, w = sub&1;
		if (sub < 4) {
			char ea[64]; int reg = modrm_ea(&st, ea, w);
			if (sub & 2)
				snprintf(op,sizeof(op),"%s %s, %s", alu[a], w?r16[reg]:r8[reg], ea);
			else
				snprintf(op,sizeof(op),"%s %s, %s", alu[a], ea, w?r16[reg]:r8[reg]);
		} else if (sub == 4) {
			snprintf(op,sizeof(op),"%s AL, %02Xh", alu[a], fetch(&st));
		} else {
			snprintf(op,sizeof(op),"%s AX, %04Xh", alu[a], fetch16(&st));
		}
		break;
	}

	case 0x06: snprintf(op,sizeof(op),"PUSH ES"); break;
	case 0x07: snprintf(op,sizeof(op),"POP ES"); break;
	case 0x0e: snprintf(op,sizeof(op),"PUSH CS"); break;

	case 0x0f: { /* V20 extensions */
		uint8_t b2 = fetch(&st);
		if (b2 >= 0x10 && b2 <= 0x1f) {
			static constexpr const char *bop[]={"TEST1","CLR1","SET1","NOT1"};
			char ea[64]; modrm_ea(&st, ea, b2&1);
			if (b2 & 8)
				snprintf(op,sizeof(op),"%s %s, %02Xh", bop[(b2>>1)&3], ea, fetch(&st));
			else
				snprintf(op,sizeof(op),"%s %s, CL", bop[(b2>>1)&3], ea);
		} else if (b2==0x20) snprintf(op,sizeof(op),"ADD4S");
		else if (b2==0x22) snprintf(op,sizeof(op),"SUB4S");
		else if (b2==0x26) snprintf(op,sizeof(op),"CMP4S");
		else if (b2==0x28) snprintf(op,sizeof(op),"ROL4");
		else if (b2==0x2a) snprintf(op,sizeof(op),"ROR4");
		else if (b2==0xff) snprintf(op,sizeof(op),"BRKEM %02Xh", fetch(&st));
		else snprintf(op,sizeof(op),"DB 0Fh, %02Xh", b2);
		break;
	}

	case 0x16: snprintf(op,sizeof(op),"PUSH SS"); break;
	case 0x17: snprintf(op,sizeof(op),"POP SS"); break;
	case 0x1e: snprintf(op,sizeof(op),"PUSH DS"); break;
	case 0x1f: snprintf(op,sizeof(op),"POP DS"); break;
	case 0x27: snprintf(op,sizeof(op),"DAA"); break;
	case 0x2f: snprintf(op,sizeof(op),"DAS"); break;
	case 0x37: snprintf(op,sizeof(op),"AAA"); break;
	case 0x3f: snprintf(op,sizeof(op),"AAS"); break;

	case 0x40: case 0x41: case 0x42: case 0x43:
	case 0x44: case 0x45: case 0x46: case 0x47:
		snprintf(op,sizeof(op),"INC %s", r16[b&7]); break;
	case 0x48: case 0x49: case 0x4a: case 0x4b:
	case 0x4c: case 0x4d: case 0x4e: case 0x4f:
		snprintf(op,sizeof(op),"DEC %s", r16[b&7]); break;
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
		snprintf(op,sizeof(op),"PUSH %s", r16[b&7]); break;
	case 0x58: case 0x59: case 0x5a: case 0x5b:
	case 0x5c: case 0x5d: case 0x5e: case 0x5f:
		snprintf(op,sizeof(op),"POP %s", r16[b&7]); break;
	case 0x60: snprintf(op,sizeof(op),"PUSHA"); break;
	case 0x61: snprintf(op,sizeof(op),"POPA"); break;
	case 0x62: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"BOUND %s, %s",r16[reg],ea); break; }

	case 0x68: snprintf(op,sizeof(op),"PUSH %04Xh", fetch16(&st)); break;
	case 0x69: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"IMUL %s, %s, %04Xh",r16[reg],ea,fetch16(&st)); break; }
	case 0x6a: snprintf(op,sizeof(op),"PUSH %02Xh", fetch(&st)); break;
	case 0x6b: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"IMUL %s, %s, %02Xh",r16[reg],ea,fetch(&st)); break; }

	case 0x6c: snprintf(op,sizeof(op),"INSB"); break;
	case 0x6d: snprintf(op,sizeof(op),"INSW"); break;
	case 0x6e: snprintf(op,sizeof(op),"OUTSB"); break;
	case 0x6f: snprintf(op,sizeof(op),"OUTSW"); break;

	case 0x70: case 0x71: case 0x72: case 0x73:
	case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7a: case 0x7b:
	case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
		int8_t d = (int8_t)fetch(&st);
		snprintf(op,sizeof(op),"%s %04Xh", cc[b&0xf], (uint16_t)(ip + st.pos + d));
		break;
	}

	case 0x80: case 0x81: case 0x82: case 0x83: {
		int w = b&1;
		char ea[64]; int reg=modrm_ea(&st, ea, w);
		if (b==0x81)
			snprintf(op,sizeof(op),"%s %s, %04Xh", alu[reg], ea, fetch16(&st));
		else if (b==0x83)
			snprintf(op,sizeof(op),"%s %s, %02Xh", alu[reg], ea, fetch(&st));
		else
			snprintf(op,sizeof(op),"%s %s, %02Xh", alu[reg], ea, fetch(&st));
		break;
	}

	case 0x84: { char ea[64]; int reg=modrm_ea(&st, ea,0); snprintf(op,sizeof(op),"TEST %s, %s",ea,r8[reg]); break; }
	case 0x85: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"TEST %s, %s",ea,r16[reg]); break; }
	case 0x86: { char ea[64]; int reg=modrm_ea(&st, ea,0); snprintf(op,sizeof(op),"XCHG %s, %s",ea,r8[reg]); break; }
	case 0x87: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"XCHG %s, %s",ea,r16[reg]); break; }
	case 0x88: { char ea[64]; int reg=modrm_ea(&st, ea,0); snprintf(op,sizeof(op),"MOV %s, %s",ea,r8[reg]); break; }
	case 0x89: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"MOV %s, %s",ea,r16[reg]); break; }
	case 0x8a: { char ea[64]; int reg=modrm_ea(&st, ea,0); snprintf(op,sizeof(op),"MOV %s, %s",r8[reg],ea); break; }
	case 0x8b: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"MOV %s, %s",r16[reg],ea); break; }
	case 0x8c: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"MOV %s, %s",ea,sr[reg&3]); break; }
	case 0x8d: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"LEA %s, %s",r16[reg],ea); break; }
	case 0x8e: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"MOV %s, %s",sr[reg&3],ea); break; }
	case 0x8f: { char ea[64]; modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"POP %s",ea); break; }

	case 0x90: snprintf(op,sizeof(op),"NOP"); break;
	case 0x91: case 0x92: case 0x93: case 0x94:
	case 0x95: case 0x96: case 0x97:
		snprintf(op,sizeof(op),"XCHG AX, %s", r16[b&7]); break;
	case 0x98: snprintf(op,sizeof(op),"CBW"); break;
	case 0x99: snprintf(op,sizeof(op),"CWD"); break;
	case 0x9a: { uint16_t o=fetch16(&st); uint16_t s=fetch16(&st); snprintf(op,sizeof(op),"CALL %04X:%04Xh",s,o); break; }
	case 0x9b: snprintf(op,sizeof(op),"WAIT"); break;
	case 0x9c: snprintf(op,sizeof(op),"PUSHF"); break;
	case 0x9d: snprintf(op,sizeof(op),"POPF"); break;
	case 0x9e: snprintf(op,sizeof(op),"SAHF"); break;
	case 0x9f: snprintf(op,sizeof(op),"LAHF"); break;

	case 0xa0: snprintf(op,sizeof(op),"MOV AL, [%04Xh]", fetch16(&st)); break;
	case 0xa1: snprintf(op,sizeof(op),"MOV AX, [%04Xh]", fetch16(&st)); break;
	case 0xa2: snprintf(op,sizeof(op),"MOV [%04Xh], AL", fetch16(&st)); break;
	case 0xa3: snprintf(op,sizeof(op),"MOV [%04Xh], AX", fetch16(&st)); break;
	case 0xa4: snprintf(op,sizeof(op),"MOVSB"); break;
	case 0xa5: snprintf(op,sizeof(op),"MOVSW"); break;
	case 0xa6: snprintf(op,sizeof(op),"CMPSB"); break;
	case 0xa7: snprintf(op,sizeof(op),"CMPSW"); break;
	case 0xa8: snprintf(op,sizeof(op),"TEST AL, %02Xh", fetch(&st)); break;
	case 0xa9: snprintf(op,sizeof(op),"TEST AX, %04Xh", fetch16(&st)); break;
	case 0xaa: snprintf(op,sizeof(op),"STOSB"); break;
	case 0xab: snprintf(op,sizeof(op),"STOSW"); break;
	case 0xac: snprintf(op,sizeof(op),"LODSB"); break;
	case 0xad: snprintf(op,sizeof(op),"LODSW"); break;
	case 0xae: snprintf(op,sizeof(op),"SCASB"); break;
	case 0xaf: snprintf(op,sizeof(op),"SCASW"); break;

	case 0xb0: case 0xb1: case 0xb2: case 0xb3:
	case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		snprintf(op,sizeof(op),"MOV %s, %02Xh", r8[b&7], fetch(&st)); break;
	case 0xb8: case 0xb9: case 0xba: case 0xbb:
	case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		snprintf(op,sizeof(op),"MOV %s, %04Xh", r16[b&7], fetch16(&st)); break;

	case 0xc0: case 0xc1: { int w=b&1; char ea[64]; int reg=modrm_ea(&st, ea,w); snprintf(op,sizeof(op),"%s %s, %02Xh",shf[reg],ea,fetch(&st)); break; }
	case 0xc2: snprintf(op,sizeof(op),"RET %04Xh", fetch16(&st)); break;
	case 0xc3: snprintf(op,sizeof(op),"RET"); break;
	case 0xc4: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"LES %s, %s",r16[reg],ea); break; }
	case 0xc5: { char ea[64]; int reg=modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"LDS %s, %s",r16[reg],ea); break; }
	case 0xc6: { char ea[64]; modrm_ea(&st, ea,0); snprintf(op,sizeof(op),"MOV %s, %02Xh",ea,fetch(&st)); break; }
	case 0xc7: { char ea[64]; modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"MOV %s, %04Xh",ea,fetch16(&st)); break; }
	case 0xc8: { uint16_t sz=fetch16(&st); uint8_t lv=fetch(&st); snprintf(op,sizeof(op),"ENTER %04Xh, %02Xh",sz,lv); break; }
	case 0xc9: snprintf(op,sizeof(op),"LEAVE"); break;
	case 0xca: snprintf(op,sizeof(op),"RETF %04Xh", fetch16(&st)); break;
	case 0xcb: snprintf(op,sizeof(op),"RETF"); break;
	case 0xcc: snprintf(op,sizeof(op),"INT 3"); break;
	case 0xcd: snprintf(op,sizeof(op),"INT %02Xh", fetch(&st)); break;
	case 0xce: snprintf(op,sizeof(op),"INTO"); break;
	case 0xcf: snprintf(op,sizeof(op),"IRET"); break;

	case 0xd0: case 0xd1: { int w=b&1; char ea[64]; int reg=modrm_ea(&st, ea,w); snprintf(op,sizeof(op),"%s %s, 1",shf[reg],ea); break; }
	case 0xd2: case 0xd3: { int w=b&1; char ea[64]; int reg=modrm_ea(&st, ea,w); snprintf(op,sizeof(op),"%s %s, CL",shf[reg],ea); break; }
	case 0xd4: snprintf(op,sizeof(op),"AAM %02Xh", fetch(&st)); break;
	case 0xd5: snprintf(op,sizeof(op),"AAD %02Xh", fetch(&st)); break;
	case 0xd6: snprintf(op,sizeof(op),"SALC"); break;
	case 0xd7: snprintf(op,sizeof(op),"XLAT"); break;

	case 0xd8: case 0xd9: case 0xda: case 0xdb:
	case 0xdc: case 0xdd: case 0xde: case 0xdf:
		{ char ea[64]; modrm_ea(&st, ea,1); snprintf(op,sizeof(op),"ESC %d, %s",b-0xd8,ea); break; }

	case 0xe0: { int8_t d=(int8_t)fetch(&st); snprintf(op,sizeof(op),"LOOPNE %04Xh",(uint16_t)(ip+st.pos+d)); break; }
	case 0xe1: { int8_t d=(int8_t)fetch(&st); snprintf(op,sizeof(op),"LOOPE %04Xh",(uint16_t)(ip+st.pos+d)); break; }
	case 0xe2: { int8_t d=(int8_t)fetch(&st); snprintf(op,sizeof(op),"LOOP %04Xh",(uint16_t)(ip+st.pos+d)); break; }
	case 0xe3: { int8_t d=(int8_t)fetch(&st); snprintf(op,sizeof(op),"JCXZ %04Xh",(uint16_t)(ip+st.pos+d)); break; }

	case 0xe4: snprintf(op,sizeof(op),"IN AL, %02Xh", fetch(&st)); break;
	case 0xe5: snprintf(op,sizeof(op),"IN AX, %02Xh", fetch(&st)); break;
	case 0xe6: snprintf(op,sizeof(op),"OUT %02Xh, AL", fetch(&st)); break;
	case 0xe7: snprintf(op,sizeof(op),"OUT %02Xh, AX", fetch(&st)); break;

	case 0xe8: { int16_t d=(int16_t)fetch16(&st); snprintf(op,sizeof(op),"CALL %04Xh",(uint16_t)(ip+st.pos+d)); break; }
	case 0xe9: { int16_t d=(int16_t)fetch16(&st); snprintf(op,sizeof(op),"JMP %04Xh",(uint16_t)(ip+st.pos+d)); break; }
	case 0xea: { uint16_t o=fetch16(&st); uint16_t s=fetch16(&st); snprintf(op,sizeof(op),"JMP %04X:%04Xh",s,o); break; }
	case 0xeb: { int8_t d=(int8_t)fetch(&st); snprintf(op,sizeof(op),"JMP SHORT %04Xh",(uint16_t)(ip+st.pos+d)); break; }

	case 0xec: snprintf(op,sizeof(op),"IN AL, DX"); break;
	case 0xed: snprintf(op,sizeof(op),"IN AX, DX"); break;
	case 0xee: snprintf(op,sizeof(op),"OUT DX, AL"); break;
	case 0xef: snprintf(op,sizeof(op),"OUT DX, AX"); break;

	case 0xf4: snprintf(op,sizeof(op),"HLT"); break;
	case 0xf5: snprintf(op,sizeof(op),"CMC"); break;

	case 0xf6: case 0xf7: {
		int w = b&1;
		char ea[64]; int reg = modrm_ea(&st, ea, w);
		static constexpr const char *g3[] = {"TEST","TEST","NOT","NEG","MUL","IMUL","DIV","IDIV"};
		if (reg <= 1) {
			if (w) snprintf(op,sizeof(op),"%s %s, %04Xh", g3[reg], ea, fetch16(&st));
			else   snprintf(op,sizeof(op),"%s %s, %02Xh", g3[reg], ea, fetch(&st));
		} else {
			snprintf(op,sizeof(op),"%s %s", g3[reg], ea);
		}
		break;
	}

	case 0xf8: snprintf(op,sizeof(op),"CLC"); break;
	case 0xf9: snprintf(op,sizeof(op),"STC"); break;
	case 0xfa: snprintf(op,sizeof(op),"CLI"); break;
	case 0xfb: snprintf(op,sizeof(op),"STI"); break;
	case 0xfc: snprintf(op,sizeof(op),"CLD"); break;
	case 0xfd: snprintf(op,sizeof(op),"STD"); break;

	case 0xfe: {
		char ea[64]; int reg = modrm_ea(&st, ea, 0);
		if (reg==0) snprintf(op,sizeof(op),"INC %s",ea);
		else if (reg==1) snprintf(op,sizeof(op),"DEC %s",ea);
		else snprintf(op,sizeof(op),"DB FEh /%d %s",reg,ea);
		break;
	}
	case 0xff: {
		char ea[64]; int reg = modrm_ea(&st, ea, 1);
		static constexpr const char *g5[] = {"INC","DEC","CALL","CALL FAR","JMP","JMP FAR","PUSH","???"};
		snprintf(op,sizeof(op),"%s %s", g5[reg], ea);
		break;
	}

	default:
		snprintf(op,sizeof(op),"DB %02Xh", b);
		break;
	}

	if (prefix[0])
		snprintf(buf, (size_t)bufsz, "%s%s", prefix, op);
	else
		snprintf(buf, (size_t)bufsz, "%s", op);

	return st.pos;
}

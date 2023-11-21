#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "common.h"
#include "arena.h"
#include "ast.h"
#include "frontend.h"
#include "compile.h"
#include "eval.h"
#include "report.h"
#include "zcode.h"
#include "blorb.h"
#include "backend_z.h"

#define TWEAK_BINSEARCH 16

#define NZOBJFLAG 48

struct var {
	struct var	*next;
	struct word	*name;
	int16_t		slot;
	int8_t		used_in_subgoal;
	int8_t		persistent;
	int8_t		used;
	int8_t		occurrences;
	int8_t		remaining_occurrences;
	int8_t		still_in_reg;
};

struct dictword {
	uint16_t	encoded[3];
	uint16_t	n_essential;
	struct word	*word;
};

struct backend_pred {
	int		global_label;
	int		trace_output_label;
	uint16_t	user_global;
	uint16_t	user_flag_mask;
	int		object_flag;
	int		propbase_label;
};

struct backend_wobj {
	uint16_t	*encoded_name;
	int		n_encoded;
	uint16_t	addr_objtable;
	uint16_t	addr_proptable;
	uint8_t		npropword[64];
	uint16_t	*propword[64];
	int		initialparent;
};

struct global_string {
	struct global_string	*next;
	uint8_t			*zscii;
	int			nchar;
	uint16_t		global_label;
};

struct datatable {
	uint16_t		label;
	uint16_t		length;
	uint8_t			*data;
};

struct wordtable {
	uint16_t		label;
	uint16_t		length;
	uint16_t		*words;
};

uint16_t extended_zscii[69] = {
	// These unicode chars map to zscii characters 155..223 in order.
	0x0e4, 0x0f6, 0x0fc, 0x0c4, 0x0d6, 0x0dc, 0x0df, 0x0bb, 0x0ab, 0x0eb,
	0x0ef, 0x0ff, 0x0cb, 0x0cf, 0x0e1, 0x0e9, 0x0ed, 0x0f3, 0x0fa, 0x0fd,
	0x0c1, 0x0c9, 0x0cd, 0x0d3, 0x0da, 0x0dd, 0x0e0, 0x0e8, 0x0ec, 0x0f2,
	0x0f9, 0x0c0, 0x0c8, 0x0cc, 0x0d2, 0x0d9, 0x0e2, 0x0ea, 0x0ee, 0x0f4,
	0x0fb, 0x0c2, 0x0ca, 0x0ce, 0x0d4, 0x0db, 0x0e5, 0x0c5, 0x0f8, 0x0d8,
	0x0e3, 0x0f1, 0x0f5, 0x0c3, 0x0d1, 0x0d5, 0x0e6, 0x0c6, 0x0e7, 0x0c7,
	0x0fe, 0x0f0, 0x0de, 0x0d0, 0x0a3, 0x153, 0x152, 0x0a1, 0x0bf
};

#define ENVF_ENV		0x010
#define ENVF_CUT_SAVED		0x020
#define ENVF_SIMPLE_SAVED	0x040
#define ENVF_SIMPLEREF_SAVED	0x080
#define ENVF_CAN_BE_MULTI	0x100
#define ENVF_CAN_BE_SIMPLE	0x200
#define ENVF_MAYBE_FOR_WORDS	0x400
#define ENVF_ARITY_MASK		0x00f

#define TAIL_CONT	0xffff

#define NSTOPCHAR strlen(STOPCHARS)

#define BUCKETS 512

struct routine **routines;
int next_routine_num;

struct routine *routinehash[BUCKETS];

static uint8_t *zcore;

static int ndict;
static struct dictword *dictionary;

static uint16_t *global_labels;
static int next_global_label = G_FIRST_FREE, nalloc_global_label;

static struct global_string *stringhash[BUCKETS];

static int next_temp, max_temp = 0;

static int next_user_global = 0, user_global_base, user_flags_global, next_user_flag = 0;
static int next_flag = 0;
static uint16_t *extflagreaders = 0;

static uint16_t undoflag_global, undoflag_mask;

static uint16_t *glbflag_global, *glbflag_mask;

int next_free_prop = 1;
uint16_t propdefault[64];

static int tracing_enabled;

static struct datatable *datatable;
static int ndatatable;

static struct wordtable *wordtable;
static int nwordtable;

static struct backend_wobj *backendwobj;

void set_global_label(uint16_t lab, uint16_t val) {
	assert(lab < nalloc_global_label);
	global_labels[lab] = val;
}

uint16_t make_global_label() {
	uint16_t lab = next_global_label++;
	int nn;

	if(lab >= nalloc_global_label) {
		nn = (lab * 2) + 8;
		global_labels = realloc(global_labels, nn * sizeof(uint16_t));
		memset(global_labels + nalloc_global_label, 0, (nn - nalloc_global_label) * sizeof(uint16_t));
		nalloc_global_label = nn;
	}

	return lab;
}

uint16_t f_make_routine_labels(int n, int line) {
	int i;

	routines = realloc(routines, (next_routine_num + n) * sizeof(struct routine *));
	for(i = 0; i < n; i++) {
		routines[next_routine_num + i] = calloc(1, sizeof(struct routine));
		routines[next_routine_num + i]->aline = line;
	}

	i = next_routine_num;
	next_routine_num += n;

	return i;
}

#define make_routine_label() f_make_routine_labels(1, __LINE__)
#define make_routine_labels(n) f_make_routine_labels(n, __LINE__)

uint32_t hashbytes(uint8_t *data, int n) {
	int i;
	uint32_t h = 0;

	for(i = 0; i < n; i++) {
		if(h & 1) {
			h = (h >> 1) ^ 0x81324bba;
		} else {
			h >>= 1;
		}
		h += data[i];
	}

	return h;
}

uint16_t resolve_rnum(uint16_t num) {
	struct routine *r = routines[num], *r2;
	int i, j;
	uint16_t specified, actual;
	int h;

	if(r->actual_routine == 0xffff) {
		r->actual_routine = 0xfffe;
		for(i = 0; i < r->ninstr; i++) {
			for(j = 0; j < 4; j++) {
				if((r->instr[i].oper[j] & 0xf0000) == ROUTINE(0)) {
					specified = r->instr[i].oper[j] & 0xffff;
					actual = resolve_rnum(specified);
					if(actual == 0xfffe
					|| (r->instr[i].op == Z_CALLVS && j == 0)) {
						routines[specified]->actual_routine = specified;
					} else {
						if(actual == routines[R_FAIL_PRED]->actual_routine) {
							if(r->instr[i].op == Z_RET) {
								assert(j == 0);
								r->instr[i].op = Z_RFALSE;
								r->instr[i].oper[j] = 0;
							} else {
								r->instr[i].oper[j] = SMALL(0);
							}
						} else {
							r->instr[i].oper[j] = ROUTINE(actual);
						}
					}
				}
			}
		}
		if(r->actual_routine == 0xfffe
		&& r->instr[0].op == Z_RET
		&& (r->instr[0].oper[0] & 0xf0000) == ROUTINE(0)) {
			r->actual_routine = r->instr[0].oper[0] & 0xffff;
		} else {
			h = hashbytes((uint8_t *) r->instr, r->ninstr * sizeof(struct zinstr)) & (BUCKETS - 1);
			if(r->actual_routine == 0xfffe) {
				for(r2 = routinehash[h]; r2; r2 = r2->next_in_hash) {
					if(r->ninstr == r2->ninstr
					&& !memcmp(r->instr, r2->instr, r->ninstr * sizeof(struct zinstr))) {
						break;
					}
				}
				if(r2) {
					r->actual_routine = r2->actual_routine;
				} else {
					r->actual_routine = num;
					r->next_in_hash = routinehash[h];
					routinehash[h] = r;
				}
			} else {
				r->next_in_hash = routinehash[h];
				routinehash[h] = r;
			}
		}
	}

	return r->actual_routine;
}

static uint8_t unicode_to_zscii(uint16_t uchar) {
	int i;

	if(uchar < 128) {
		if(uchar >= 16 && uchar <= 19) {
			uchar += 129 - 16;
		}
		return uchar;
	} else {
		for(i = 0; i < sizeof(extended_zscii) / 2; i++) {
			if(extended_zscii[i] == uchar) break;
		}
		if(i >= sizeof(extended_zscii) / 2) {
			report(LVL_ERR, 0, "Unsupported unicode character U+%04x in dictionary word context.", uchar);
			exit(1);
		} else {
			return 155 + i;
		}
	}
}

static int utf8_to_zscii(uint8_t *dest, int ndest, char *src, uint32_t *special) {
	uint8_t ch;
	uint32_t uchar;
	int outpos = 0, inpos = 0, i;

	/* Stops on end of input, special unicode char, or full output. */
	/* The output is always null-terminated. */
	/* Returns number of utf8 bytes consumed. */

	for(;;) {
		if(outpos >= ndest - 1) {
			dest[outpos] = 0;
			if(special) *special = 0;
			return inpos;
		}
		ch = src[inpos];
		if(!ch) {
			dest[outpos] = 0;
			if(special) *special = 0;
			return inpos;
		}
		inpos++;
		if(ch & 0x80) {
			int nbyte = 0;
			int mask = 0x40;
			while(ch & mask) {
				nbyte++;
				mask >>= 1;
			}
			uchar = ch & (mask - 1);
			while(nbyte--) {
				ch = src[inpos++];
				if((ch & 0xc0) != 0x80) {
					report(LVL_ERR, 0, "Invalid UTF-8 sequence in source code file. ('%s')", src);
					exit(1);
				}
				uchar <<= 6;
				uchar |= ch & 0x3f;
			}
		} else {
			uchar = ch;
		}
		if(uchar < 128) {
			if(uchar >= 16 && uchar <= 19) {
				uchar += 129 - 16;
			}
			dest[outpos++] = uchar;
		} else {
			for(i = 0; i < sizeof(extended_zscii) / 2; i++) {
				if(extended_zscii[i] == uchar) break;
			}
			if(i >= sizeof(extended_zscii) / 2) {
				dest[outpos] = 0;
				if(special) *special = uchar;
				return inpos;
			} else {
				dest[outpos++] = 155 + i;
			}
			// In the future, we should convert extended characters to lowercase
			// for dictionary words. For now, story authors are expected to include
			// an explicit lowercase alias in the code where necessary.
		}
	}
}

static int encode_chars(uint8_t *dest, int ndest, uint16_t *for_dict, uint8_t *src) {
	int n = 0;
	char *str, *a2 = "\r0123456789.,!?_#'\"/\\-:()";
	uint8_t zscii;

	/* Converts from 8-bit zscii to pentets. */

	while((zscii = *src++)) {
		if(n >= ndest) return n;
		if(zscii == ' ') {
			dest[n++] = 0;
		} else if(zscii >= 'a' && zscii <= 'z') {
			dest[n++] = 6 + zscii - 'a';
		} else if(zscii >= 'A' && zscii <= 'Z') {
			if(for_dict) {
				dest[n++] = 6 + zscii - 'A';
			} else {
				dest[n++] = 4;
				if(n >= ndest) return n;
				dest[n++] = 6 + zscii - 'A';
			}
		} else if(zscii < 128 && (str = strchr(a2, (char) zscii))) {
			dest[n++] = 5;
			if(n >= ndest) return n;
			dest[n++] = 7 + (str - a2);
		} else {
			dest[n++] = 5;
			if(n >= ndest) return n;
			dest[n++] = 6;
			if(n >= ndest) return n;
			dest[n++] = (zscii >> 5) & 0x1f;
			if(n >= ndest) return n;
			dest[n++] = zscii & 0x1f;
		}
		if(for_dict) {
			(*for_dict)++;
		}
	}

	return n;
}

int pack_pentets(uint16_t *dest, uint8_t *pentets, int n) {
	int count = 0;

	while(n > 3) {
		*dest++ = (pentets[0] << 10) | (pentets[1] << 5) | pentets[2];
		count++;
		pentets += 3;
		n -= 3;
	}
	if(n == 1) {
		*dest = (pentets[0] << 10) | (5 << 5) | 5 | 0x8000;
		count++;
	} else if(n == 2) {
		*dest = (pentets[0] << 10) | (pentets[1] << 5) | 5 | 0x8000;
		count++;
	} else if(n == 3) {
		*dest = (pentets[0] << 10) | (pentets[1] << 5) | pentets[2] | 0x8000;
		count++;
	} else {
		*dest = 0x94a5; // 1 00101 00101 00101
		count++;
	}

	return count;
}

struct global_string *find_global_string(uint8_t *zscii) {
	int i;
	uint32_t h = 0;
	struct global_string *gs;

	for(i = 0; zscii[i]; i++) {
		if(h & 1) {
			h = (h >> 1) ^ 0x8abcd123;
		} else {
			h >>= 1;
		}
		h += zscii[i];
	}

	h &= BUCKETS - 1;

	for(gs = stringhash[h]; gs; gs = gs->next) {
		if(gs->nchar == i
		&& !memcmp(gs->zscii, zscii, i)) {
			return gs;
		}
	}

	gs = malloc(sizeof(*gs));
	gs->next = stringhash[h];
	stringhash[h] = gs;
	gs->nchar = i;
	gs->zscii = malloc(i + 1);
	memcpy(gs->zscii, zscii, i);
	gs->zscii[i] = 0;
	gs->global_label = make_global_label();

	return gs;
}

void dump_routine(struct routine *r) {
	int i, j;
	struct zinstr *zi;

	printf("Routine:\n");
	for(i = 0; i < r->ninstr; i++) {
		zi = &r->instr[i];

		printf(" %03x", zi->op);
		for(j = 0; j < 4; j++) {
			if(zi->oper[j]) {
				printf(" %05x", zi->oper[j]);
			} else {
				printf(" -----");
			}
		}
		if(zi->store) {
			printf(" --> %02x", zi->store);
		}
		if(zi->branch) {
			printf(" ? %02x", zi->branch);
		}
		if(zi->string) {
			printf(" \"%s\"", zi->string);
		}
		printf("\n");
	}
}

int oper_size(uint32_t oper) {
	switch(oper >> 16) {
	case 5:
	case 6:
	case 7:
	case 8:
		return 1;
	case 1:
	case 2:
	case 3:
	case 4:
		return 2;
	default:
		return 0;
	}
}

int typebits(uint32_t oper) {
	switch(oper >> 16) {
	case 0:
		return 3;
	case 5:
	case 8:
		return 1;
	case 6:
	case 7:
		return 2;
	default:
		return 0;
	}
}

int assemble_oper(uint32_t org, uint32_t oper, struct routine *r) {
	uint16_t value;
	int32_t diff;

	switch(oper >> 16) {
	case 5:
		zcore[org] = oper & 0xff;
		return 1;
	case 6:
		value = oper & 0xff;
		if(value >= 1 && value <= 0xf) {
			if(value > r->nlocal) {
				dump_routine(r);
				assert(value <= r->nlocal);
			}
		}
		zcore[org] = value;
		return 1;
	case 7:
	case 8:
		zcore[org] = 0x10 + user_global_base + (oper & 0xff);
		return 1;
	case 1:
		zcore[org + 0] = oper >> 8;
		zcore[org + 1] = oper & 0xff;
		return 2;
	case 2:
		value = 0;
		if((oper & 0xffff) < next_global_label) value = global_labels[oper & 0xffff];
		if(!value) {
			report(LVL_WARN, 0, "Internal inconsistency: Undefined global label %d at %05x", oper & 0xffff, org);
			value = 0xdead;
		}
		zcore[org + 0] = value >> 8;
		zcore[org + 1] = value & 0xff;
		return 2;
	case 3:
		value = oper & 0xffff;
		assert(value < next_routine_num);
		assert(routines[value]->actual_routine == value);
		if(!routines[value]->address
		&& routines[value]->actual_routine != routines[R_FAIL_PRED]->actual_routine) {
			report(LVL_WARN, 0, "Internal inconsistency: Undefined routine number %d", oper & 0xffff);
			value = 0xdead;
		} else {
			value = routines[value]->address;
		}
		zcore[org + 0] = value >> 8;
		zcore[org + 1] = value & 0xff;
		return 2;
	case 4:
		if((oper & 0xffff) >= r->nalloc_lab
		|| r->local_labels[oper & 0xffff] == 0xffffffff) {
			report(LVL_ERR, 0, "Internal inconsistency: Local label %d not found", oper & 0xffff);
			exit(1);
		}
		diff = r->local_labels[oper & 0xffff] - (org + 2) + 2;
		if(diff < -0x8000 || diff > 0x7fff) {
			report(LVL_ERR, 0, "Relative jump offset is too large");
			exit(1);
		}
		value = diff;
		zcore[org + 0] = value >> 8;
		zcore[org + 1] = value & 0xff;
		return 2;
	}

	return 0;
}

void assemble(uint32_t org, struct routine *r) {
	int i, n, pc;
	uint8_t pentets[MAXSTRING];
	uint16_t words[(MAXSTRING + 2) / 3];
	uint16_t op, posflag;
	struct zinstr *zi;

	for(pc = 0; pc < r->ninstr; pc++) {
		zi = &r->instr[pc];
		if(zi->op == OP_NOP) continue;
		posflag = (!(zi->op & OP_NOT)) << 7;
		op = zi->op & ~(OP_NOT | OP_FAR);
		if(op & OP_LABEL(0)) {
			// skip
		} else if(op & OP_EXT) {
			zcore[org++] = 0xbe;
			zcore[org++] = op & 0xff;
			zcore[org++] =
				(typebits(zi->oper[0]) << 6) |
				(typebits(zi->oper[1]) << 4) |
				(typebits(zi->oper[2]) << 2) |
				(typebits(zi->oper[3]) << 0);
			for(i = 0; i < 4; i++) {
				org += assemble_oper(org, zi->oper[i], r);
			}
		} else if(op & 0x80) {
			if((op & 0x30) == 0x30) {
				zcore[org++] = op;
				if(op == Z_PRINTLIT) {
					n = encode_chars(pentets, MAXSTRING, 0, (uint8_t *) zi->string);
					assert(n < MAXSTRING);
					n = pack_pentets(words, pentets, n);
					for(i = 0; i < n; i++) {
						zcore[org++] = words[i] >> 8;
						zcore[org++] = words[i] & 0xff;
					}
				}
			} else {
				zcore[org++] = op | (typebits(zi->oper[0]) << 4);
				org += assemble_oper(org, zi->oper[0], r);
			}
		} else {
			if(op < 0x20 && zi->oper[0] >= 0x50000 && zi->oper[1] >= 0x50000 && !zi->oper[2]) {
				zcore[org++] = (op & 0x1f)
					| ((zi->oper[0] >= 0x60000) << 6)
					| ((zi->oper[1] >= 0x60000) << 5);
				org += assemble_oper(org, zi->oper[0], r);
				org += assemble_oper(org, zi->oper[1], r);
			} else {
				zcore[org++] = 0xc0 | (op & 0x3f);
				zcore[org++] =
					(typebits(zi->oper[0]) << 6) |
					(typebits(zi->oper[1]) << 4) |
					(typebits(zi->oper[2]) << 2) |
					(typebits(zi->oper[3]) << 0);
				for(i = 0; i < 4; i++) {
					org += assemble_oper(org, zi->oper[i], r);
				}
			}
		}
		if(zi->store) {
			if(zi->store == REG_PUSH) {
				zcore[org++] = 0;
			} else if(zi->store & DEST_USERGLOBAL(0)) {
				zcore[org++] = 0x10 + user_global_base + (zi->store & 0xff);
			} else {
				zcore[org++] = zi->store;
			}
		}
		if(zi->branch) {
			if(zi->branch == RFALSE) {
				zcore[org++] = posflag | 0x40;
			} else if(zi->branch == RTRUE) {
				zcore[org++] = posflag | 0x41;
			} else {
				if(zi->branch >= r->nalloc_lab
				|| r->local_labels[zi->branch] == 0xffffffff) {
					report(LVL_ERR, 0, "Internal inconsistency: Unknown local label %d", zi->branch);
					exit(1);
				}
				if(zi->op & OP_FAR) {
					int32_t diff = r->local_labels[zi->branch] - (org + 2) + 2;
					if(diff < -0x2000 || diff >= 0x1fff) {
						report(LVL_ERR, 0, "Branch offset too large (%x).", diff);
						exit(1);
					}
					zcore[org++] = posflag | ((diff >> 8) & 0x3f);
					zcore[org++] = diff & 0xff;
				} else {
					uint16_t diff = r->local_labels[zi->branch] - (org + 1) + 2;
					assert(diff >= 2);
					assert(diff < 64);
					zcore[org++] = posflag | 0x40 | diff;
				}
			}
		}
	}
}

int pass1(struct routine *r, uint32_t org) {
	int size, lab;
	struct zinstr *zi;
	int i, n, need_recheck = 1, pc;
	uint8_t pentets[MAXSTRING];
	uint16_t op;

	assert(r->next_label <= 0x1000);

	do {
		size = 1;
		for(pc = 0; pc < r->ninstr; pc++) {
			zi = &r->instr[pc];
			if(zi->op == OP_NOP) continue;
			op = zi->op & ~(OP_NOT | OP_FAR);
			if(op & OP_LABEL(0)) {
				lab = op & 0xfff;
				if(lab >= r->nalloc_lab) {
					r->local_labels = realloc(r->local_labels, (lab + 1) * sizeof(uint32_t));
					while(r->nalloc_lab < lab + 1) {
						r->local_labels[r->nalloc_lab++] = 0xffffffff;
					}
				}
				r->local_labels[lab] = org + size;
			} else if(op & OP_EXT) {
				size += 3;
				for(i = 0; i < 4; i++) {
					size += oper_size(zi->oper[i]);
				}
			} else if(op & 0x80) {
				if((op & 0x30) == 0x30) {
					size++;
					if(op == Z_PRINTLIT) {
						n = encode_chars(pentets, MAXSTRING, 0, (uint8_t *) zi->string);
						assert(n <= MAXSTRING);
						size += ((n + 2) / 3) * 2;
					}
				} else {
					size++;
					size += oper_size(zi->oper[0]);
				}
			} else {
				if(op < 0x20 && zi->oper[0] >= 0x50000 && zi->oper[1] >= 0x50000 && !zi->oper[2]) {
					size += 3;
				} else {
					size += 2;
					for(i = 0; i < 4; i++) {
						size += oper_size(zi->oper[i]);
					}
				}
			}
			if(zi->store) size++;
			if(zi->branch) {
				if(zi->branch == RFALSE || zi->branch == RTRUE) {
					size += 1;
				} else if(zi->branch >= r->nalloc_lab || r->local_labels[zi->branch] == 0xffffffff) {
					if(zi->op & OP_FAR) {
						size += 2;
					} else {
						size += 1;
					}
					if(!need_recheck) {
						printf("%d\n", zi->branch);
						dump_routine(r);
						assert(0);
					}
				} else {
					if(zi->op & OP_FAR) {
						int32_t diff = r->local_labels[zi->branch] - (org + size + 2) + 2;
						if(diff < -0x2000 || diff >= 0x1fff) {
							if(r->ninstr + 2 < r->nalloc_instr) {
								r->nalloc_instr = r->ninstr + 16;
								r->instr = realloc(r->instr, r->nalloc_instr * sizeof(struct zinstr));
							}
							r->ninstr += 2;
							memmove(r->instr + pc + 3, r->instr + pc + 1, (r->ninstr - pc - 3) * sizeof(struct zinstr));
							memset(&r->instr[pc + 1], 0, 2 * sizeof(*zi));
							r->instr[pc + 1].op = Z_JUMP;
							r->instr[pc + 1].oper[0] = REL_LABEL(r->instr[pc + 0].branch);
							r->instr[pc + 2].op = OP_LABEL(r->next_label);
							r->instr[pc + 0].branch = r->next_label++;
							r->instr[pc + 0].op ^= OP_NOT;
							r->instr[pc + 0].op &= ~OP_FAR;
							need_recheck = 2;
							r->nalloc_lab = 0;
							break;
						} else {
							size += 2;
						}
					} else {
						int32_t diff = r->local_labels[zi->branch] - (org + size + 1) + 2;
						if(diff < 2 || diff > 63) {
							zi->op |= OP_FAR;
							size += 2;
							for(i = 0; i < r->nalloc_lab; i++) {
								if(r->local_labels[i] != 0xffffffff
								&& r->local_labels[i] >= org + size) {
									r->local_labels[i]++;
								}
							}
							need_recheck = 2;
						} else {
							size += 1;
						}
					}
				}
			}
		}
	} while(need_recheck--);

	return size;
}

int cmp_dictword(const void *a, const void *b) {
	const struct dictword *aa = (const struct dictword *) a;
	const struct dictword *bb = (const struct dictword *) b;
	int i;

	for(i = 0; i < 3; i++) {
		if(aa->encoded[i] < bb->encoded[i]) return -1;
		if(aa->encoded[i] > bb->encoded[i]) return 1;
	}

	return 0;
}

void prepare_dictionary_z(struct program *prg) {
	int i, n;
	uint8_t pentets[9];
	uint8_t zbuf[MAXSTRING];
	uint32_t uchar;
	char chbuf[2];
	struct word *w;
	char runtime[7] = {8, 13, 32, 16, 17, 18, 19};

	for(i = 0; i < 7; i++) {
		chbuf[0] = runtime[i];
		chbuf[1] = 0;
		w = find_word(prg, chbuf);
		ensure_dict_word(prg, w);
	}

	if(prg->ndictword >= 0x1e00) {
		report(LVL_ERR, 0, "Too many dictionary words.");
		exit(1);
	}

	prg->dictmap = arena_calloc(&prg->arena, prg->ndictword * sizeof(uint16_t));

	ndict = prg->ndictword;
	dictionary = calloc(ndict, sizeof(*dictionary));

	for(i = 0; i < ndict; i++) {
		w = prg->dictwordnames[i];
		dictionary[i].word = w;
		assert(w->name[0]);
		if(w->name[0] >= 16 && w->name[0] <= 19 && !w->name[1]) {
			zbuf[0] = w->name[0] - 16 + 129;
			zbuf[1] = 0;
		} else {
			(void) utf8_to_zscii(zbuf, sizeof(zbuf), w->name, &uchar);
			if(uchar) {
				report(
					LVL_ERR,
					0,
					"Unsupported character U+%04x in dictionary word '@%s'.",
					uchar,
					w->name);
				exit(1);
			}
		}
		assert(zbuf[0]);
		n = encode_chars(pentets, 9, &dictionary[i].n_essential, zbuf);
		memset(pentets + n, 5, 9 - n);
		pack_pentets(dictionary[i].encoded, pentets, 9);
	}

	qsort(dictionary, ndict, sizeof(struct dictword), cmp_dictword);

	for(i = 0; i < ndict; ) {
		w = dictionary[i].word;
		if(dictionary[i].n_essential == 1) {
			(void) utf8_to_zscii(zbuf, sizeof(zbuf), w->name, &uchar);
			assert(!uchar);
			prg->dictmap[w->dict_id] = 0x3e00 | zbuf[0];
			memmove(dictionary + i, dictionary + i + 1, (ndict - i - 1) * sizeof(struct dictword));
			ndict--;
		} else {
			prg->dictmap[w->dict_id] = 0x2000 | i;
			if(i < ndict - 1 && !memcmp(dictionary[i].encoded, dictionary[i + 1].encoded, 6)) {
				report(LVL_INFO, 0, "Consolidating dictionary words \"%s\" and \"%s\".",
					dictionary[i].word->name,
					dictionary[i + 1].word->name);
				prg->dictmap[dictionary[i + 1].word->dict_id] = 0x2000 | i;
				memmove(dictionary + i, dictionary + i + 1, (ndict - i - 1) * sizeof(struct dictword));
				ndict--;
			} else {
				i++;
			}
		}
	}
}

void init_backend_wobj(struct program *prg, int id, struct backend_wobj *wobj, int strip) {
	uint8_t pentets[256];
	int n;
	uint8_t zbuf[MAXSTRING];
	uint32_t uchar;

	if(strip) {
		pentets[0] = 5;
		n = 1;
	} else {
		n = utf8_to_zscii(zbuf, sizeof(zbuf), prg->worldobjnames[id]->name, &uchar);
		if(uchar) {
			report(
				LVL_ERR,
				0,
				"Unsupported character U+%04x in object name '#%s'",
				uchar,
				prg->worldobjnames[id]->name);
			exit(1);
		}
		n = encode_chars(pentets, sizeof(pentets), 0, zbuf);
		if(n == sizeof(pentets)) {
			report(
				LVL_ERR,
				0,
				"Object name too long: '#%s'",
				prg->worldobjnames[id]->name);
			exit(1);
		}
	}
	wobj->n_encoded = (n + 2) / 3;
	wobj->encoded_name = malloc(wobj->n_encoded * 2);
	pack_pentets(wobj->encoded_name, pentets, n);
}

struct backend_pred *init_backend_pred(struct predicate *pred) {
	if(!pred->backend) {
		pred->backend = calloc(1, sizeof(struct backend_pred));
	}
	return pred->backend;
}

void init_abbrev(uint16_t addr_abbrevstr, uint16_t addr_abbrevtable) {
	uint8_t pentets[3];
	uint16_t words[1];
	int i;

	encode_chars(pentets, sizeof(pentets), 0, (uint8_t *) "foo");
	pack_pentets(words, pentets, 3);
	zcore[addr_abbrevstr + 0] = words[0] >> 8;
	zcore[addr_abbrevstr + 1] = words[0] & 0xff;

	for(i = 0; i < 96; i++) {
		zcore[addr_abbrevtable + i * 2 + 0] = addr_abbrevstr >> 9;
		zcore[addr_abbrevtable + i * 2 + 1] = (addr_abbrevstr >> 1) & 0xff;
	}
}

struct routine *make_routine(uint16_t lab, int nlocal) {
	struct routine *r;

	assert(lab < next_routine_num);
	r = routines[lab];
	assert(!r->ninstr);
	r->nlocal = nlocal;
	r->next_label = 2;
	r->actual_routine = 0xffff;
	return r;
}

struct zinstr *append_instr(struct routine *r, uint16_t op) {
	struct zinstr *zi;

	if(r->ninstr >= r->nalloc_instr) {
		r->nalloc_instr = (r->ninstr * 2) + 8;
		r->instr = realloc(r->instr, r->nalloc_instr * sizeof(struct zinstr));
	}
	zi = &r->instr[r->ninstr++];
	memset(zi, 0, sizeof(*zi));
	zi->op = op;

	return zi;
}

static uint16_t dict_id_tag(struct program *prg, uint16_t dict_id) {
	assert(prg->dictmap[dict_id]);
	return prg->dictmap[dict_id];
}

static uint16_t dictword_tag(struct program *prg, struct word *w) {
	if(!(w->flags & WORDF_DICT)) {
		printf("%s\n", w->name);
		assert(0);
	}
	return dict_id_tag(prg, w->dict_id);
}

void compile_put_ast_in_reg(struct routine *r, struct astnode *an, int target_reg, struct var *vars);

uint8_t *stash_lingering(struct var *vars) {
	int i = 0, n = 0;
	struct var *v;
	uint8_t *buf;

	for(v = vars; v; v = v->next) {
		n++;
	}
	buf = malloc(n);
	for(v = vars; v; v = v->next) {
		buf[i++] = v->still_in_reg;
	}

	return buf;
}

void reapply_lingering(struct var *vars, uint8_t *stash) {
	int i = 0;
	struct var *v;

	for(v = vars; v; v = v->next) {
		v->still_in_reg = stash[i++];
	}
}

void clear_lingering(struct var *vars) {
	while(vars) {
		vars->still_in_reg = 0;
		vars = vars->next;
	}
}

static uint32_t compile_dictword(struct program *prg, struct routine *r, struct word *w) {
	int n, i, t, first;
	uint8_t zbuf[MAXSTRING];
	uint32_t uchar;
	struct zinstr *zi;
	uint16_t tagged;
	int zdict;

	assert(w->flags & WORDF_DICT);
	tagged = prg->dictmap[w->dict_id];

	if((tagged & 0xff00) == 0x3e00) {
		return LARGE(tagged);
	}

	if((tagged & 0xe000) != 0x2000) {
		printf("%04x %d '%s'\n", tagged, w->dict_id, w->name);
	}
	assert((tagged & 0xe000) == 0x2000);

	zdict = tagged & 0x1fff;
	assert(zdict < ndict);

	n = utf8_to_zscii(zbuf, sizeof(zbuf), w->name, &uchar);
	assert(!w->name[n]);
	assert(!uchar);
	if(strlen((char *) zbuf) == dictionary[zdict].n_essential) {
		return LARGE(tagged);
	} else {
		t = next_temp++;
		first = 1;
		for(i = strlen((char *) zbuf) - 1; i >= dictionary[zdict].n_essential; i--) {
			zi = append_instr(r, Z_CALLVS);
			zi->oper[0] = ROUTINE(R_PUSH_PAIR_VV);
			zi->oper[1] = LARGE(0x3e00 | zbuf[i]);
			zi->oper[2] = first? VALUE(REG_NIL) : VALUE(REG_X + t);
			zi->store = REG_X + t;
			first = 0;
		}
		zi = append_instr(r, Z_CALLVS);
		zi->oper[0] = ROUTINE(R_PUSH_PAIR_VV);
		zi->oper[1] = LARGE(tagged);
		zi->oper[2] = VALUE(REG_X + t);
		zi->store = REG_X + t;
		zi = append_instr(r, Z_OR);
		zi->oper[0] = VALUE(REG_X + t);
		zi->oper[1] = VALUE(REG_E000);
		zi->store = REG_X + t;
#if 0
		printf("creating extended: ");
		for(i = 0; zbuf[i]; i++) {
			if(i == dictionary[zdict].n_essential) {
				printf("+");
			}
			if(zbuf[i] < 128) {
				fputc(zbuf[i], stdout);
			} else {
				printf("\\%02x", zbuf[i]);
			}
		}
		printf("\n");
#endif
		return VALUE(REG_X + t);
	}
}

static void add_to_buf(char **buf, int *nalloc, int *pos, char ch) {
	if(*pos >= *nalloc) {
		*nalloc = (*pos) * 2 + 32;
		*buf = realloc(*buf, *nalloc);
	}
	(*buf)[(*pos)++] = ch;
}

static int decode_word_output(struct program *prg, char **bufptr, struct cinstr *instr, int *p_space, int include_ints) {
	int post_space = 0;
	int ninstr = 0;
	char last = 0;
	char *buf = 0;
	int i, j;
	int nalloc = 0, pos = 0;
	struct cinstr *ci;
	struct word *w;
	char numbuf[9];

	for(;;) {
		ci = &instr[ninstr];
		if(ci->op == I_PRINT_WORDS) {
			for(i = 0; i < 3; i++) {
				if(ci->oper[i].tag == OPER_WORD) {
					w = prg->allwords[ci->oper[i].value];
					if(post_space == 2
					|| (post_space == 1 && !strchr(NO_SPACE_BEFORE, w->name[0]))) {
						add_to_buf(&buf, &nalloc, &pos, ' ');
					}
					for(j = 0; w->name[j]; j++) {
						last = w->name[j];
						add_to_buf(&buf, &nalloc, &pos, last);
					}
					post_space = !strchr(NO_SPACE_AFTER, last);
				}
			}
			ninstr++;
		} else if(ci->op == I_PRINT_VAL && include_ints && ci->oper[0].tag == VAL_NUM) {
			snprintf(numbuf, sizeof(numbuf), "%d", ci->oper[0].value);
			if(post_space) {
				add_to_buf(&buf, &nalloc, &pos, ' ');
			}
			for(i = 0; numbuf[i]; i++) {
				last = numbuf[i];
				add_to_buf(&buf, &nalloc, &pos, last);
			}
			post_space = 1;
			ninstr++;
		} else if(ci->op == I_BUILTIN && prg->predicates[ci->oper[2].value]->builtin == BI_NOSPACE) {
			if(post_space == 1) post_space = 0;
			ninstr++;
		} else if(ci->op == I_BUILTIN && prg->predicates[ci->oper[2].value]->builtin == BI_SPACE) {
			post_space = 2;
			ninstr++;
		} else {
			break;
		}
	}

	add_to_buf(&buf, &nalloc, &pos, 0);
	*bufptr = buf;
	if(p_space) *p_space = post_space;
	return ninstr;
}

static void generate_output_from_utf8(struct program *prg, struct routine *r, int pre_space, int post_space, char *utf8) {
	uint8_t zbuf[MAXSTRING];
	int pos;
	int n;
	uint16_t stringlabel;
	uint32_t variant;
	struct zinstr *zi;
	uint32_t uchar;

	for(pos = 0; utf8[pos]; pos += n) {
		uchar = 0;
		n = utf8_to_zscii(zbuf, sizeof(zbuf), utf8 + pos, &uchar);
		if(n && *zbuf) {
			stringlabel = find_global_string(zbuf)->global_label;
		} else {
			stringlabel = 0;
		}

		if(uchar) {
			if(stringlabel) {
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(pre_space? R_SPACE_PRINT_NOSPACE : R_NOSPACE_PRINT_NOSPACE);
				zi->oper[1] = REF(stringlabel);
			} else if(pre_space) {
				zi = append_instr(r, Z_CALL1N);
				zi->oper[0] = ROUTINE(R_SYNC_SPACE);
			}
			zi = append_instr(r, Z_CALL2N);
			zi->oper[0] = ROUTINE(R_UNICODE);
			zi->oper[1] = SMALL_OR_LARGE(uchar & 0xffff);
			pre_space = 1;
			if(!utf8[pos + n]) {
				/* String ended with a unicode character */
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_SPACE);
				zi->oper[1] = SMALL(!post_space);
			}
		} else if(utf8[pos + n]) {
			/* String too long (for runtime uppercase buffer) */
			assert(n);
			assert(stringlabel);
			zi = append_instr(r, Z_CALL2N);
			zi->oper[0] = ROUTINE(pre_space? R_SPACE_PRINT_NOSPACE : R_NOSPACE_PRINT_NOSPACE);
			zi->oper[1] = REF(stringlabel);
			pre_space = 0;
		} else {
			if(n) {
				if(!pre_space && !post_space) {
					variant = ROUTINE(R_NOSPACE_PRINT_NOSPACE);
				} else if(!pre_space && post_space) {
					variant = ROUTINE(R_NOSPACE_PRINT_AUTO);
				} else if(pre_space && !post_space) {
					variant = ROUTINE(R_SPACE_PRINT_NOSPACE);
				} else {
					variant = VALUE(REG_R_SPA);
				}
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = variant;
				zi->oper[1] = REF(stringlabel);
			} else {
				assert(!pre_space);
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_SPACE);
				zi->oper[1] = SMALL(!post_space);
			}
		}
	}
	if(post_space == 2) {
		zi = append_instr(r, Z_CALL1N);
		zi->oper[0] = ROUTINE(R_SPACE);
	}
}

static int generate_output(struct program *prg, struct routine *r, struct cinstr *instr, int dry_run, int include_ints) {
	char *utf8;
	int pre_space;
	int post_space;
	int ninstr;

	pre_space = !strchr(NO_SPACE_BEFORE, prg->allwords[instr[0].oper[0].value]->name[0]);
	ninstr = decode_word_output(prg, &utf8, instr, &post_space, include_ints);

	if(!dry_run) {
		generate_output_from_utf8(prg, r, pre_space, post_space, utf8);
	}

	free(utf8);
	return ninstr;
}

void compile_extflag_reader(uint16_t routine_label, uint16_t array_label) {
	// Read external fixed flag byte from a hardcoded array.
	// The single parameter is the object, and it must be bound.
	// The return value is the byte value from the array.

	struct routine *r;
	struct zinstr *zi;

	r = make_routine(routine_label, 1);

	// Deref the object.

	zi = append_instr(r, Z_JGE);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = VALUE(REG_C000);
	zi->branch = 2;
	zi = append_instr(r, OP_LABEL(1));
	zi = append_instr(r, Z_ADD);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = VALUE(REG_LOCAL+0);
	zi->store = REG_LOCAL+0;
	zi = append_instr(r, Z_LOADW);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = SMALL(0);
	zi->store = REG_LOCAL+0;
	zi = append_instr(r, Z_JL);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = VALUE(REG_C000);
	zi->branch = 1;
	zi = append_instr(r, OP_LABEL(2));

	// Check the type. All flags are clear for non-objects.

	zi = append_instr(r, Z_JLE);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = SMALL(0);
	zi->branch = RFALSE;
	zi = append_instr(r, Z_JGE);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = VALUE(REG_NIL);
	zi->branch = RFALSE;

	// Read the array byte (1-indexed)

	zi = append_instr(r, Z_LOADB);
	zi->oper[0] = REF(array_label);
	zi->oper[1] = VALUE(REG_LOCAL+0);
	zi->store = REG_LOCAL+0;

	zi = append_instr(r, Z_RET);
	zi->oper[0] = VALUE(REG_LOCAL+0);
}

uint16_t alloc_user_flag(uint16_t *mask) {
	if(next_user_flag < 16) {
		*mask = 1 << (next_user_flag++);
	} else {
		user_flags_global = next_user_global++;
		*mask = 1;
		next_user_flag = 1;
	}
	return user_flags_global;
}

void compile_trace_output(struct predname *predname, uint16_t label) {
	int i, j, arg = 0;
	char buf[128];
	uint8_t zbuf[128];
	int bufpos = 0;
	uint16_t lab;
	struct routine *r;
	struct zinstr *zi;
	uint32_t uchar;

	r = make_routine(label, 0);
	for(i = 0; i < predname->nword; i++) {
		if(predname->words[i]) {
			if(i && (bufpos < sizeof(buf) - 1)) {
				buf[bufpos++] = ' ';
			}
			for(j = 0; predname->words[i]->name[j]; j++) {
				if(bufpos < sizeof(buf) - 1) {
					buf[bufpos++] = predname->words[i]->name[j];
				}
			}
		} else {
			if(bufpos) {
				buf[bufpos] = 0;
				utf8_to_zscii(zbuf, sizeof(zbuf), buf, &uchar);
				if(uchar) {
					report(LVL_ERR, 0, "Unsupported character U+%04x in part of predicate name %s", uchar, predname->printed_name);
					exit(1);
				}
				lab = find_global_string(zbuf)->global_label;
				zi = append_instr(r, Z_PRINTPADDR);
				zi->oper[0] = REF(lab);
				bufpos = 0;
			}
			if(i) {
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_SPACE);
				zi->oper[1] = SMALL(0);
			}
			zi = append_instr(r, Z_CALL2N);
			zi->oper[0] = ROUTINE(R_TRACE_VALUE);
			zi->oper[1] = VALUE(REG_A + arg++);
		}
	}
	if(bufpos) {
		buf[bufpos] = 0;
		utf8_to_zscii(zbuf, sizeof(zbuf), buf, &uchar);
		if(uchar) {
			report(LVL_ERR, 0, "Unsupported character U+%04x in part of predicate name %s", uchar, predname->printed_name);
			exit(1);
		}
		lab = find_global_string(zbuf)->global_label;
		zi = append_instr(r, Z_PRINTPADDR);
		zi->oper[0] = REF(lab);
	}
	append_instr(r, Z_RFALSE);
	//append_instr(r, Z_END);
}

static uint16_t tag_eval_value(value_t v, struct program *prg) {
	switch(v.tag) {
	case VAL_NUM:
		assert(v.value >= 0);
		assert(v.value <= 0x3fff);
		return 0x4000 + v.value;
	case VAL_OBJ:
		assert(v.value < prg->nworldobj);
		assert(v.value < 0x1ffe);
		return 1 + v.value;
	case VAL_DICT:
		assert(prg->dictmap[v.value] != 0xffff);
		return prg->dictmap[v.value];
	case VAL_NIL:
		return 0x1fff;
	case VAL_RAW:
		return v.value;
	}
	printf("%d\n", v.tag);
	assert(0); exit(1);
}

static int render_eval_value(uint8_t *buffer, int nword, value_t v, struct eval_state *es, struct predname *predname, int rec_depth) {
	int count, size = 1, n;
	uint16_t value;

	// Simple elements (including the empty list) are serialised as themselves.
	// Proper lists are serialised as the elements, followed by c000+n.
	// Improper lists are serialised as the elements, followed by the improper tail element, followed by e000+n.

	if(rec_depth > 8) {
		report(
			LVL_ERR,
			0,
			"Initial value of global variable %s is too complex.",
			predname->printed_name);
		exit(1);
	}

	switch(v.tag) {
	case VAL_NUM:
	case VAL_OBJ:
	case VAL_DICT:
	case VAL_NIL:
		value = tag_eval_value(v, es->program);
		break;
	case VAL_PAIR:
		count = 0;
		for(;;) {
			n = render_eval_value(buffer, nword, eval_gethead(v, es), es, predname, rec_depth + 1);
			size += n;
			nword -= n;
			buffer += 2 * n;
			count++;
			v = eval_gettail(v, es);
			if(v.tag == VAL_NIL) {
				value = 0xc000 | count;
				break;
			} else if(v.tag != VAL_PAIR) {
				n = render_eval_value(buffer, nword, v, es, predname, rec_depth + 1);
				size += n;
				nword -= n;
				buffer += 2 * n;
				value = 0xe000 | count;
				break;
			}
		}
		break;
	case VAL_REF:
		report(
			LVL_ERR,
			0,
			"Initial value of global variable %s must be bound.",
			predname->printed_name);
		exit(1);
	default:
		assert(0);
		exit(1);
	}

	if(nword <= 0) {
		report(
			LVL_ERR,
			0,
			"Cannot fit initial values into long-term heap. Use commandline option -L to enlarge it.");
		exit(1);
	}

	buffer[0] = value >> 8;
	buffer[1] = value & 0xff;
	return size;
}

static uint32_t generate_value(struct routine *r, value_t v, struct program *prg, int t) {
	struct zinstr *zi;

	if(v.tag == OPER_VAR) {
		zi = append_instr(r, Z_LOADW);
		zi->oper[0] = VALUE(REG_ENV);
		zi->oper[1] = SMALL(3 + v.value);
		zi->store = REG_X + t;
		return VALUE(REG_X + t);
	} else if(v.tag == OPER_ARG) {
		return VALUE(REG_A + v.value);
	} else if(v.tag == OPER_TEMP) {
		return VALUE(REG_X + v.value);
	} else if(v.tag == VAL_NIL) {
		return VALUE(REG_NIL);
	} else if(v.tag == VAL_DICT) {
		return compile_dictword(prg, r, prg->dictwordnames[v.value]);
	} else {
		return SMALL_OR_LARGE(tag_eval_value(v, prg));
	}
}

static void generate_store(struct routine *r, value_t dest, uint32_t oper) {
	struct zinstr *zi;

	if(dest.tag == OPER_VAR) {
		zi = append_instr(r, Z_STOREW);
		zi->oper[0] = VALUE(REG_ENV);
		zi->oper[1] = SMALL(3 + dest.value);
		zi->oper[2] = oper;
	} else if(dest.tag == OPER_TEMP) {
		zi = append_instr(r, Z_STORE);
		zi->oper[0] = SMALL(REG_X + dest.value);
		zi->oper[1] = oper;
	} else {
		assert(dest.tag == OPER_ARG);
		zi = append_instr(r, Z_STORE);
		zi->oper[0] = SMALL(REG_A + dest.value);
		zi->oper[1] = oper;
	}
}

static int generate_pre_ref(struct routine *r, value_t dest, int *state, int t) {
	if(dest.tag == OPER_VAR) {
		*state = t;
		return REG_X + *state;
	} else if(dest.tag == OPER_TEMP) {
		return REG_X + dest.value;
	} else {
		assert(dest.tag == OPER_ARG);
		return REG_A + dest.value;
	}
}

static void generate_post_ref(struct routine *r, value_t dest, int *state) {
	struct zinstr *zi;

	if(dest.tag == OPER_VAR) {
		zi = append_instr(r, Z_STOREW);
		zi->oper[0] = VALUE(REG_ENV);
		zi->oper[1] = SMALL(3 + dest.value);
		zi->oper[2] = VALUE(REG_X + *state);
	}
}

struct index_slot {
	uint16_t	key;
	uint8_t		local;
	uint8_t		fail;
	uint16_t	label;
};

static int cmp_index_slot(const void *a, const void *b) {
	const struct index_slot *aa = a;
	const struct index_slot *bb = b;

	return aa->key - bb->key;
}

static uint32_t constant_oper(uint16_t value) {
	if(value == 0x1fff) {
		return VALUE(REG_NIL);
	} else if(value == 0x4000) {
		return VALUE(REG_4000);
	} else {
		return SMALL_OR_LARGE(value);
	}
}

static void binary_search(struct index_slot *table, int n, struct routine *r, uint16_t endlab) {
	uint16_t ll;
	int i, j, pos;
	struct zinstr *zi;
	int iptr[TWEAK_BINSEARCH];

	memset(iptr, 0xff, sizeof(iptr));

	if(n >= TWEAK_BINSEARCH) {
		pos = n / 2;
		ll = r->next_label++;
		zi = append_instr(r, Z_JGE);
		zi->oper[0] = VALUE(REG_IDX);
		zi->oper[1] = SMALL_OR_LARGE(table[pos].key);
		zi->branch = ll;
		binary_search(table, pos, r, endlab);
		zi = append_instr(r, OP_LABEL(ll));
		binary_search(table + pos, n - pos, r, endlab);
	} else {
		for(i = 0; i < n; i++) {
			if(table[i].fail) {
				zi = append_instr(r, Z_JE);
				zi->oper[0] = VALUE(REG_IDX);
				zi->oper[1] = constant_oper(table[i].key);
				if(i < n - 1) {
					zi->branch = RFALSE;
				} else {
					zi->op ^= OP_NOT;
					zi->branch = endlab;
					zi = append_instr(r, Z_RFALSE);
				}
			} else if(table[i].local) {
				for(j = 0; j < i; j++) {
					if(iptr[j] >= 0
					&& table[j].local
					&& table[j].label == table[i].label
					&& !r->instr[iptr[j]].oper[3]) {
						break;
					}
				}
				if(j == i) {
					iptr[i] = r->ninstr;
					zi = append_instr(r, Z_JE);
					zi->oper[0] = VALUE(REG_IDX);
					zi->oper[1] = constant_oper(table[i].key);
					zi->branch = table[i].label;
				} else if(!r->instr[iptr[j]].oper[2]) {
					r->instr[iptr[j]].oper[2] = constant_oper(table[i].key);
				} else {
					r->instr[iptr[j]].oper[3] = constant_oper(table[i].key);
				}
				if(i == n - 1) {
					if(endlab == RFALSE) {
						zi = append_instr(r, Z_RFALSE);
					} else {
						zi = append_instr(r, Z_JUMP);
						zi->oper[0] = REL_LABEL(endlab);
					}
				}
			} else {
				if(i < n - 1) {
					ll = r->next_label++;
				} else {
					ll = endlab;
				}
				zi = append_instr(r, Z_JNE);
				zi->oper[0] = VALUE(REG_IDX);
				zi->oper[1] = constant_oper(table[i].key);
				zi->branch = ll;
				zi = append_instr(r, Z_RET);
				zi->oper[0] = ROUTINE(table[i].label);
				if(i < n - 1) {
					zi = append_instr(r, OP_LABEL(ll));
				}
			}
		}
	}
}

static void generate_index(
	struct program *prg,
	struct predicate *pred,
	struct cinstr *instr,
	int ninstr,
	struct routine *r,
	int r_id,
	uint16_t *rlabel,
	uint16_t *llabel,
	uint8_t *encountered,
	uint16_t *rstack,
	int *rsp,
	uint16_t endlab)
{
	struct index_slot table[ninstr];
	int i;

	memset(table, 0, ninstr * sizeof(struct index_slot));

	for(i = 0; i < ninstr; i++) {
		table[i].key = tag_eval_value(instr[i].oper[0], prg);
		if(instr[i].oper[1].tag == OPER_FAIL) {
			table[i].fail = 1;
		} else {
			assert(instr[i].oper[1].tag == OPER_RLAB);
			if(pred->routines[instr[i].oper[1].value].reftrack == r_id) {
				table[i].local = 1;
				table[i].label = llabel[instr[i].oper[1].value];
				if(!encountered[instr[i].oper[1].value]) {
					rstack[(*rsp)++] = instr[i].oper[1].value;
					encountered[instr[i].oper[1].value] = 1;
				}
			} else {
				assert(pred->routines[instr[i].oper[1].value].reftrack == instr[i].oper[1].value);
				table[i].label = rlabel[instr[i].oper[1].value];
			}
		}
	}

	qsort(table, ninstr, sizeof(struct index_slot), cmp_index_slot);

	binary_search(table, ninstr, r, endlab);
}

static void generate_proceed(struct routine *r, int query_kind) {
	uint16_t ll;
	struct zinstr *zi;

	if(query_kind == 1) {
		zi = append_instr(r, Z_STORE);
		zi->oper[0] = SMALL(REG_CHOICE);
		zi->oper[1] = VALUE(REG_SIMPLE);
	} else if(query_kind == 0) {
		ll = r->next_label++;
		zi = append_instr(r, Z_JZ);
		zi->oper[0] = VALUE(REG_SIMPLE);
		zi->branch = ll;
		zi = append_instr(r, Z_STORE);
		zi->oper[0] = SMALL(REG_CHOICE);
		zi->oper[1] = VALUE(REG_SIMPLE);
		zi = append_instr(r, OP_LABEL(ll));
	}
	zi = append_instr(r, Z_RET);
	zi->oper[0] = VALUE(REG_CONT);
}

static int generate_wordmap(struct wordmap *map, int *nptr) {
	int n, j, k, id, len;
	uint8_t data[1 + 2 * MAXWORDMAP];
	uint16_t words[map->nmap * 2];

	n = map->nmap;
	if(map->map[n - 1].key == 0xffff) n--;
	assert(n > 0);
	*nptr = n;
	for(j = 0; j < n; j++) {
		words[2 * j + 0] = map->map[j].key;
		if(map->map[j].count > MAXWORDMAP) {
			words[2 * j + 1] = 0;
		} else if(map->map[j].count == 1) {
			words[2 * j + 1] = 1 + map->map[j].onumtable[0];
		} else {
			len = 1;
			for(k = 0; k < map->map[j].count; k++) {
				id = 1 + map->map[j].onumtable[k];
				if(id < 0xe0) {
					data[len++] = id;
				} else {
					assert(id <= 0x1fff);
					data[len++] = 0xe0 | (id >> 8);
					data[len++] = id & 0xff;
				}
			}
			data[0] = len - 1;
			for(k = 0; k < ndatatable; k++) {
				if(datatable[k].length == len
				&& !memcmp(datatable[k].data, data, len)) {
					break;
				}
			}
			if(k == ndatatable) {
				datatable = realloc(datatable, ++ndatatable * sizeof(*datatable));
				datatable[k].label = make_global_label();
				datatable[k].length = len;
				datatable[k].data = malloc(len);
				memcpy(datatable[k].data, data, len);
			}
			words[2 * j + 1] = 0x8000 | datatable[k].label;
		}
	}
	for(j = 0; j < nwordtable; j++) {
		if(wordtable[j].length == n * 2
		&& !memcmp(wordtable[j].words, words, n * 2 * sizeof(uint16_t))) {
			break;
		}
	}
	if(j == nwordtable) {
		wordtable = realloc(wordtable, ++nwordtable * sizeof(*wordtable));
		wordtable[j].label = make_global_label();
		wordtable[j].length = n * 2;
		wordtable[j].words = malloc(n * 2 * sizeof(uint16_t));
		memcpy(wordtable[j].words, words, n * 2 * sizeof(uint16_t));
	}
	return j;
}

static void generate_code(struct program *prg, struct routine *r, struct predicate *pred, int r_id, uint16_t *rlabel) {
	int i, j, k, sub_r_id;
	uint16_t rstack[pred->nroutine];
	uint8_t encountered[pred->nroutine];
	int rsp = 0;
	int sel, n, id;
	int refstate0 = 0, refstate1 = 0, refstate2 = 0, ref0, ref1, ref2;
	struct zinstr *zi;
	struct comp_routine *cr;
	struct cinstr *ci;
	uint16_t mask;
	uint16_t ll, ll2;
	uint16_t llabel[pred->nroutine];
	uint32_t o0, o1, o2, o3, oper[3] = {0};
	int t1, t2, t3;
	struct word *w;
	struct wordmap *map;

#if 0
	zi = append_instr(r, Z_PRINTLIT);
	zi->string = pred->predname->printed_name;
	zi = append_instr(r, Z_PRINTLIT);
	zi->string = "\r";
#endif

	next_temp = 0;
	memset(llabel, 0, pred->nroutine * sizeof(uint16_t));
	for(i = 0; i < pred->nroutine; i++) {
		if(pred->routines[i].reftrack == r_id) {
			llabel[i] = r->next_label++;
			for(j = 0; j < pred->routines[i].ninstr; j++) {
				ci = &pred->routines[i].instr[j];
				for(k = 0; k < 3; k++) {
					if(ci->oper[k].tag == OPER_TEMP) {
						if(next_temp <= ci->oper[k].value) {
							next_temp = ci->oper[k].value + 1;
						}
					}
				}
			}
		}
	}

	t1 = next_temp++;
	t2 = next_temp++;
	t3 = next_temp++;

	memset(encountered, 0, pred->nroutine);
	rstack[rsp++] = r_id;
	encountered[r_id] = 1;
	while(rsp) {
		sub_r_id = rstack[--rsp];
		cr = &pred->routines[sub_r_id];
		zi = append_instr(r, OP_LABEL(llabel[sub_r_id]));
		for(i = 0; i < cr->ninstr; i++) {
			ci = &cr->instr[i];
			switch(ci->op) {
			case I_ALLOCATE:
				assert(ci->oper[0].tag == OPER_NUM);
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(R_ALLOCATE_S);
				zi->oper[1] = SMALL_OR_LARGE(6 + 2 * ci->oper[0].value);
				break;
			case I_ASSIGN:
				if(ci->oper[1].tag == OPER_VAR && ci->oper[0].tag == OPER_ARG) {
					zi = append_instr(r, Z_LOADW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[1].value);
					zi->store = REG_A + ci->oper[0].value;
				} else if(ci->oper[1].tag == OPER_VAR && ci->oper[0].tag == OPER_TEMP) {
					zi = append_instr(r, Z_LOADW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[1].value);
					zi->store = REG_X + ci->oper[0].value;
				} else {
					o1 = generate_value(r, ci->oper[1], prg, t1);
					generate_store(r, ci->oper[0], o1);
				}
				break;
			case I_BEGIN_AREA:
				assert(ci->oper[0].tag == OPER_BOX);
				if(ci->subop == AREA_TOP) {
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = ROUTINE(R_BEGIN_STATUS);
					zi->oper[1] = SMALL_OR_LARGE(
						prg->boxclasses[ci->oper[0].value].height |
						((prg->boxclasses[ci->oper[0].value].flags & BOXF_RELHEIGHT)? 0x8000 : 0));
					zi->oper[2] = SMALL(prg->boxclasses[ci->oper[0].value].style);
				} else {
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_BEGIN_NOSTATUS);
				}
				break;
			case I_BEGIN_BOX:
				assert(ci->oper[0].tag == OPER_BOX);
				if(ci->subop == BOX_SPAN) {
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_BEGIN_SPAN);
					zi->oper[1] = SMALL(prg->boxclasses[ci->oper[0].value].style);
				} else {
					zi = append_instr(r, Z_CALLVN);
					if(prg->boxclasses[ci->oper[0].value].flags & BOXF_FLOATLEFT) {
						zi->oper[0] = ROUTINE(R_BEGIN_BOX_LEFT);
						zi->oper[1] = SMALL_OR_LARGE(
							prg->boxclasses[ci->oper[0].value].width |
							((prg->boxclasses[ci->oper[0].value].flags & BOXF_RELWIDTH)? 0x8000 : 0));
						zi->oper[2] = SMALL(prg->boxclasses[ci->oper[0].value].style);
						zi->oper[3] = SMALL_OR_LARGE(prg->boxclasses[ci->oper[0].value].margintop);
					} else if(prg->boxclasses[ci->oper[0].value].flags & BOXF_FLOATRIGHT) {
						zi->oper[0] = ROUTINE(R_BEGIN_BOX_RIGHT);
						zi->oper[1] = SMALL_OR_LARGE(
							prg->boxclasses[ci->oper[0].value].width |
							((prg->boxclasses[ci->oper[0].value].flags & BOXF_RELWIDTH)? 0x8000 : 0));
						zi->oper[2] = SMALL(prg->boxclasses[ci->oper[0].value].style);
						zi->oper[3] = SMALL_OR_LARGE(prg->boxclasses[ci->oper[0].value].margintop);
					} else {
						zi->oper[0] = ROUTINE(R_BEGIN_BOX);
						zi->oper[1] = SMALL(prg->boxclasses[ci->oper[0].value].style);
						zi->oper[2] = SMALL_OR_LARGE(prg->boxclasses[ci->oper[0].value].margintop);
					}
				}
				break;
			case I_BEGIN_LINK:
			case I_BEGIN_LINK_RES:
			case I_BEGIN_SELF_LINK:
				zi = append_instr(r, Z_INC);
				zi->oper[0] = SMALL(REG_NSPAN);
				break;
			case I_BUILTIN:
				assert(ci->oper[2].tag == OPER_PRED);
				id = prg->predicates[ci->oper[2].value]->builtin;
				switch(id) {
				case BI_BOLD:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_ENABLE_STYLE);
					zi->oper[1] = SMALL(2);
					break;
				case BI_CLEAR:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_CLEAR);
					zi->oper[1] = SMALL(0);
					break;
				case BI_CLEAR_ALL:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_CLEAR);
					zi->oper[1] = VALUE(REG_FFFF);
					break;
				case BI_CLEAR_LINKS:
				case BI_CLEAR_DIV:
				case BI_CLEAR_OLD:
					break;
				case BI_COMPILERVERSION:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_COMPILERVERSION);
					break;
				case BI_FIXED:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_ENABLE_STYLE);
					zi->oper[1] = SMALL(8);
					break;
				case BI_ITALIC:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_ENABLE_STYLE);
					zi->oper[1] = SMALL(4);
					break;
				case BI_LINE:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_LINE);
					break;
				case BI_MEMSTATS:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_MEMSTATS);
					break;
				case BI_NOSPACE:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_NOSPACE);
					break;
				case BI_PAR:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_PAR);
					break;
				case BI_PROGRESS_BAR:
					o1 = generate_value(r, ci->oper[0], prg, t1);
					o2 = generate_value(r, ci->oper[1], prg, t2);
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_PROGRESS_BAR);
					zi->oper[1] = o1;
					zi->oper[2] = o2;
					break;
				case BI_REVERSE:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_ENABLE_STYLE);
					zi->oper[1] = SMALL(1);
					break;
				case BI_ROMAN:
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_SET_STYLE);
					zi->oper[1] = SMALL(0);
					break;
				case BI_SCRIPT_OFF:
					zi = append_instr(r, Z_OUTPUT_STREAM);
					zi->oper[0] = LARGE(0xfffe);
					break;
				case BI_SCRIPT_ON:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_SCRIPT_ON);
					break;
				case BI_SERIALNUMBER:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_SERIALNUMBER);
					break;
				case BI_SPACE:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_SPACE);
					break;
				case BI_SPACE_N:
					o1 = generate_value(r, ci->oper[0], prg, t1);
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_SPACE_N);
					zi->oper[1] = o1;
					break;
				case BI_TRACE_OFF:
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_TRACING);
					zi->oper[1] = SMALL(0);
					break;
				case BI_TRACE_ON:
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_TRACING);
					zi->oper[1] = SMALL(1);
					break;
				case BI_UNSTYLE:
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_RESET_STYLE);
					break;
				case BI_UPPER:
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_UPPER);
					zi->oper[1] = SMALL(1);
					break;
				default:
					printf("unimplemented builtin %d %s\n", id, prg->predicates[ci->oper[2].value]->printed_name);
					assert(0); exit(1);
				}
				break;
			case I_CHECK_INDEX:
				n = 1;
				while(cr->instr[i + n].op == I_CHECK_INDEX) n++;
				if(cr->instr[i + n].op == I_JUMP && cr->instr[i + n].oper[0].tag == OPER_FAIL) {
					ll = RFALSE;
				} else {
					ll = r->next_label++;
				}
				generate_index(prg, pred, &cr->instr[i], n, r, r_id, rlabel, llabel, encountered, rstack, &rsp, ll);
				if(ll == RFALSE) {
					i++;
				} else {
					zi = append_instr(r, OP_LABEL(ll));
				}
				i += n - 1;
				break;
			case I_CHECK_WORDMAP:
				assert(ci->oper[0].tag == OPER_NUM);
				assert(ci->oper[2].tag == OPER_PRED);
				assert(ci->oper[0].value < prg->predicates[ci->oper[2].value]->pred->nwordmap);
				map = &prg->predicates[ci->oper[2].value]->pred->wordmaps[ci->oper[0].value];
				id = generate_wordmap(map, &n);
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_WORDMAP);
				zi->oper[1] = REF(wordtable[id].label);
				zi->oper[2] = SMALL_OR_LARGE(n);
				zi->oper[3] = VALUE(REG_IDX);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JNZ);
				zi->oper[0] = VALUE(REG_TEMP);
				if(ci->oper[1].tag == OPER_FAIL) {
					zi->branch = RFALSE;
				} else {
					assert(ci->oper[1].tag == OPER_RLAB);
					if(pred->routines[ci->oper[1].value].reftrack == r_id) {
						zi->branch = llabel[ci->oper[1].value];
						if(!encountered[ci->oper[1].value]) {
							rstack[rsp++] = ci->oper[1].value;
							encountered[ci->oper[1].value] = 1;
						}
					} else {
						ll = r->next_label++;
						zi->op ^= OP_NOT;
						zi->branch = ll;
						zi = append_instr(r, Z_RET);
						zi->oper[0] = ROUTINE(rlabel[ci->oper[1].value]);
						zi = append_instr(r, OP_LABEL(ll));
					}
				}
				break;
			case I_CLRALL_OFLAG:
				assert(ci->oper[0].tag == OPER_OFLAG);
				assert(prg->objflagpred[ci->oper[0].value]->pred->flags & PREDF_DYN_LINKAGE);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_CLRALL_LISTFLAG);
				zi->oper[1] = SMALL(((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->object_flag);
				zi->oper[2] = SMALL_USERGLOBAL(((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->user_global);
				zi->oper[3] = REF(((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->propbase_label);
				break;
			case I_CLRALL_OVAR:
				assert(ci->oper[0].tag == OPER_OVAR);
				assert(ci->oper[0].value != DYN_HASPARENT);
				id = ((struct backend_pred *) prg->objvarpred[ci->oper[0].value]->pred->backend)->propbase_label;
				ll = r->next_label++;
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_X + t1);
				zi->oper[1] = SMALL(1);
				zi = append_instr(r, OP_LABEL(ll));
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_SET_LONGTERM_VAR);
				zi->oper[1] = REF(id);
				zi->oper[2] = VALUE(REG_X + t1);
				zi->oper[3] = SMALL(0);
				zi = append_instr(r, Z_INC_JLE);
				zi->oper[0] = SMALL(REG_X + t1);
				zi->oper[1] = SMALL_OR_LARGE(prg->nworldobj);
				zi->branch = ll;
				break;
			case I_COLLECT_BEGIN:
				zi = append_instr(r, Z_CALL1N);
				zi->oper[0] = ROUTINE(ci->subop? R_ACCUM_BEGIN : R_COLLECT_BEGIN);
				break;
			case I_COLLECT_CHECK:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(R_COLLECT_CHECK);
				zi->oper[1] = o1;
				break;
			case I_COLLECT_END_R:
				zi = append_instr(r, Z_CALL1S);
				zi->oper[0] = ROUTINE(ci->subop? R_ACCUM_END : R_COLLECT_END);
				if(ci->oper[0].tag == OPER_VAR) {
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_STOREW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[0].value);
					zi->oper[2] = VALUE(REG_TEMP);
				} else if(ci->oper[0].tag == OPER_ARG) {
					zi->store = REG_A + ci->oper[0].value;
				} else {
					assert(ci->oper[0].tag == OPER_TEMP);
					zi->store = REG_X + ci->oper[0].value;
				}
				break;
			case I_COLLECT_END_V:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL1S);
				zi->oper[0] = ROUTINE(ci->subop? R_ACCUM_END : R_COLLECT_END);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_UNIFY);
				zi->oper[1] = o1;
				zi->oper[2] = VALUE(REG_TEMP);
				break;
			case I_COLLECT_MATCH_ALL:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(R_COLLECT_MATCH_ALL);
				zi->oper[1] = o1;
				break;
			case I_COLLECT_PUSH:
				if(ci->subop) {
					if(ci->oper[0].tag == VAL_NUM && ci->oper[0].value == 1) {
						zi = append_instr(r, Z_CALL1N);
						zi->oper[0] = ROUTINE(R_ACCUM_INC);
					} else {
						o1 = generate_value(r, ci->oper[0], prg, t1);
						zi = append_instr(r, Z_CALL2N);
						zi->oper[0] = ROUTINE(R_ACCUM_ADD);
						zi->oper[1] = o1;
					}
				} else if(ci->oper[0].tag == VAL_OBJ) {
					if(cr->instr[i + 1].op == I_COLLECT_PUSH
					&& cr->instr[i + 1].oper[0].tag == VAL_OBJ) {
						if(cr->instr[i + 2].op == I_COLLECT_PUSH
						&& cr->instr[i + 2].oper[0].tag == VAL_OBJ) {
							zi = append_instr(r, Z_CALLVN);
							zi->oper[0] = ROUTINE(R_AUX_PUSH3);
							zi->oper[1] = SMALL_OR_LARGE(1 + ci->oper[0].value);
							zi->oper[2] = SMALL_OR_LARGE(1 + cr->instr[i + 1].oper[0].value);
							zi->oper[3] = SMALL_OR_LARGE(1 + cr->instr[i + 2].oper[0].value);
							i += 2;
						} else {
							zi = append_instr(r, Z_CALLVN);
							zi->oper[0] = ROUTINE(R_AUX_PUSH2);
							zi->oper[1] = SMALL_OR_LARGE(1 + ci->oper[0].value);
							zi->oper[2] = SMALL_OR_LARGE(1 + cr->instr[i + 1].oper[0].value);
							i++;
						}
					} else {
						zi = append_instr(r, Z_CALL2N);
						zi->oper[0] = ROUTINE(R_AUX_PUSH1);
						zi->oper[1] = SMALL_OR_LARGE(1 + ci->oper[0].value);
					}
				} else {
					o1 = generate_value(r, ci->oper[0], prg, t1);
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_COLLECT_PUSH);
					zi->oper[1] = o1;
				}
				break;
			case I_COMPUTE_V:
			case I_COMPUTE_R:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				o1 = generate_value(r, ci->oper[1], prg, t2);
				zi = append_instr(r, Z_CALLVS);
				switch(ci->subop) {
				case BI_PLUS:
					zi->oper[0] = ROUTINE(R_PLUS);
					break;
				case BI_MINUS:
					zi->oper[0] = ROUTINE(R_MINUS);
					break;
				case BI_TIMES:
					zi->oper[0] = ROUTINE(R_TIMES);
					break;
				case BI_DIVIDED:
					zi->oper[0] = ROUTINE(R_DIVIDED);
					break;
				case BI_MODULO:
					zi->oper[0] = ROUTINE(R_MODULO);
					break;
				case BI_RANDOM:
					zi->oper[0] = ROUTINE(R_RANDOM);
					break;
				default:
					assert(0); exit(1);
				}
				zi->oper[1] = o0;
				zi->oper[2] = o1;
				if(ci->op == I_COMPUTE_R) {
					if(ci->oper[2].tag == OPER_VAR) {
						zi->store = REG_TEMP;
						zi = append_instr(r, Z_STOREW);
						zi->oper[0] = VALUE(REG_ENV);
						zi->oper[1] = SMALL(3 + ci->oper[2].value);
						zi->oper[2] = VALUE(REG_TEMP);
					} else if(ci->oper[2].tag == OPER_ARG) {
						zi->store = REG_A + ci->oper[2].value;
					} else {
						assert(ci->oper[2].tag == OPER_TEMP);
						zi->store = REG_X + ci->oper[2].value;
					}
				} else {
					zi->store = REG_TEMP;
					o2 = generate_value(r, ci->oper[2], prg, t1);
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = VALUE(REG_R_USIMPLE);
					zi->oper[1] = VALUE(REG_TEMP);
					zi->oper[2] = o2;
				}
				break;
			case I_CUT_CHOICE:
				zi = append_instr(r, Z_LOADW);
				zi->oper[0] = VALUE(REG_CHOICE);
				zi->oper[1] = SMALL(CHOICE_CHOICE);
				zi->store = REG_CHOICE;
				break;
			case I_DEALLOCATE:
				zi = append_instr(r, Z_CALL1N);
				zi->oper[0] = ROUTINE(R_DEALLOCATE_S);
				break;
			case I_EMBED_RES:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(R_EMBED_RES);
				zi->oper[1] = o1;
				break;
			case I_END_AREA:
				assert(ci->oper[0].tag == OPER_BOX);
				zi = append_instr(r, Z_CALL1N);
				zi->oper[0] = ROUTINE(R_END_STATUS);
				break;
			case I_END_BOX:
				assert(ci->oper[0].tag == OPER_BOX);
				if(ci->subop == BOX_SPAN) {
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_END_SPAN);
				} else if(prg->boxclasses[ci->oper[0].value].flags & (BOXF_FLOATLEFT | BOXF_FLOATRIGHT)) {
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_END_BOX_FLOAT);
					zi->oper[1] = SMALL_OR_LARGE(prg->boxclasses[ci->oper[0].value].marginbottom);
				} else {
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_END_BOX);
					zi->oper[1] = SMALL_OR_LARGE(prg->boxclasses[ci->oper[0].value].marginbottom);
				}
				break;
			case I_END_LINK:
			case I_END_LINK_RES:
			case I_END_SELF_LINK:
				zi = append_instr(r, Z_DEC);
				zi->oper[0] = SMALL(REG_NSPAN);
				break;
			case I_FIRST_CHILD:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF_OBJ_FAIL);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_GET_CHILD_N);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->branch = RFALSE;
				assert(ci->oper[1].tag == OPER_TEMP);
				zi->store = REG_X + ci->oper[1].value;
				break;
			case I_FIRST_OFLAG:
				assert(ci->oper[0].tag == OPER_OFLAG);
				id = ((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->user_global;
				zi = append_instr(r, Z_JZ);
				zi->oper[0] = USERGLOBAL(id);
				zi->branch = RFALSE;
				generate_store(r, ci->oper[1], USERGLOBAL(id));
				break;
			case I_FOR_WORDS:
				if(ci->subop) {
					zi = append_instr(r, Z_INC);
					zi->oper[0] = SMALL(REG_FORWORDS);
				} else {
					zi = append_instr(r, Z_DEC);
					zi->oper[0] = SMALL(REG_FORWORDS);
				}
				break;
			case I_GET_OVAR_R:
				assert(ci->oper[0].tag == OPER_OVAR);
				o1 = generate_value(r, ci->oper[1], prg, t1);
				if(ci->oper[1].tag != VAL_OBJ) {
					zi = append_instr(r, Z_CALL2S);
					zi->oper[0] = ROUTINE(R_DEREF_OBJ_FAIL);
					zi->oper[1] = o1;
					zi->store = REG_TEMP;
					o1 = VALUE(REG_TEMP);
				}
				if(ci->oper[0].value == DYN_HASPARENT) {
					zi = append_instr(r, Z_GET_PARENT);
					zi->oper[0] = o1;
					if(ci->oper[2].tag == OPER_VAR) {
						zi->store = REG_TEMP;
						o1 = VALUE(zi->store);
						zi = append_instr(r, Z_STOREW);
						zi->oper[0] = VALUE(REG_ENV);
						zi->oper[1] = SMALL(3 + ci->oper[2].value);
						zi->oper[2] = VALUE(REG_TEMP);
					} else if(ci->oper[2].tag == OPER_TEMP) {
						zi->store = REG_X + ci->oper[2].value;
						o1 = VALUE(zi->store);
					} else {
						assert(ci->oper[2].tag == OPER_ARG);
						zi->store = REG_A + ci->oper[2].value;
						o1 = VALUE(zi->store);
					}
					zi = append_instr(r, Z_JZ);
					zi->oper[0] = o1;
					zi->branch = RFALSE;
				} else {
					id = ((struct backend_pred *) prg->objvarpred[ci->oper[0].value]->pred->backend)->propbase_label;
					zi = append_instr(r, Z_LOADW);
					zi->oper[0] = REF(id);
					zi->oper[1] = o1;
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_CALL2S);
					zi->oper[0] = ROUTINE(R_GET_LONGTERM_VAR);
					zi->oper[1] = VALUE(REG_TEMP);
					if(ci->oper[2].tag == OPER_VAR) {
						zi->store = REG_TEMP;
						zi = append_instr(r, Z_STOREW);
						zi->oper[0] = VALUE(REG_ENV);
						zi->oper[1] = SMALL(3 + ci->oper[2].value);
						zi->oper[2] = VALUE(REG_TEMP);
					} else if(ci->oper[2].tag == OPER_TEMP) {
						zi->store = REG_X + ci->oper[2].value;
					} else {
						assert(ci->oper[2].tag == OPER_ARG);
						zi->store = REG_A + ci->oper[2].value;
					}
				}
				break;
			case I_GET_OVAR_V:
				assert(ci->oper[0].tag == OPER_OVAR);
				o2 = generate_value(r, ci->oper[2], prg, t2);
				o1 = generate_value(r, ci->oper[1], prg, t1);
				if(ci->oper[1].tag != VAL_OBJ) {
					zi = append_instr(r, Z_CALL2S);
					zi->oper[0] = ROUTINE(R_DEREF_OBJ_FAIL);
					zi->oper[1] = o1;
					zi->store = REG_TEMP;
					o1 = VALUE(REG_TEMP);
				}
				if(ci->oper[0].value == DYN_HASPARENT) {
					zi = append_instr(r, Z_GET_PARENT);
					zi->oper[0] = o1;
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_JZ);
					zi->oper[0] = VALUE(REG_TEMP);
					zi->branch = RFALSE;
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = VALUE(REG_R_USIMPLE);
					zi->oper[1] = VALUE(REG_TEMP);
					zi->oper[2] = o2;
				} else {
					id = ((struct backend_pred *) prg->objvarpred[ci->oper[0].value]->pred->backend)->propbase_label;
					zi = append_instr(r, Z_LOADW);
					zi->oper[0] = REF(id);
					zi->oper[1] = o1;
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_CALL2S);
					zi->oper[0] = ROUTINE(R_GET_LONGTERM_VAR);
					zi->oper[1] = VALUE(REG_TEMP);
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = ROUTINE(R_UNIFY);
					zi->oper[1] = VALUE(REG_TEMP);
					zi->oper[2] = o2;
				}
				break;
			case I_GET_GVAR_R:
				assert(ci->oper[0].tag == OPER_GVAR);
				id = ((struct backend_pred *) prg->globalvarpred[ci->oper[0].value]->pred->backend)->user_global;
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_GET_LONGTERM_VAR);
				zi->oper[1] = USERGLOBAL(id);
				if(ci->oper[1].tag == OPER_VAR) {
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_STOREW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[1].value);
					zi->oper[2] = VALUE(REG_TEMP);
				} else if(ci->oper[1].tag == OPER_TEMP) {
					zi->store = REG_X + ci->oper[1].value;
				} else {
					assert(ci->oper[1].tag == OPER_ARG);
					zi->store = REG_A + ci->oper[1].value;
				}
				break;
			case I_GET_GVAR_V:
				assert(ci->oper[0].tag == OPER_GVAR);
				id = ((struct backend_pred *) prg->globalvarpred[ci->oper[0].value]->pred->backend)->user_global;
				o1 = generate_value(r, ci->oper[1], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_GET_LONGTERM_VAR);
				zi->oper[1] = USERGLOBAL(id);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_UNIFY);
				zi->oper[1] = o1;
				zi->oper[2] = VALUE(REG_TEMP);
				break;
			case I_GET_INPUT:
				zi = append_instr(r, Z_CALL1S);
				zi->oper[0] = ROUTINE(R_GET_INPUT);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_UNIFY);
				zi->oper[1] = VALUE(REG_TEMP);
				zi->oper[2] = VALUE(REG_A + 0);
				generate_proceed(r, 0);
				break;
			case I_GET_KEY:
				zi = append_instr(r, Z_CALL1S);
				zi->oper[0] = ROUTINE(R_GET_KEY);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = VALUE(REG_R_USIMPLE);
				zi->oper[1] = VALUE(REG_TEMP);
				zi->oper[2] = VALUE(REG_A + 0);
				generate_proceed(r, 0);
				break;
			case I_GET_PAIR_RR:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				ref1 = generate_pre_ref(r, ci->oper[1], &refstate1, t2);
				ref2 = generate_pre_ref(r, ci->oper[2], &refstate2, t3);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_GET_PAIR_RR);
				zi->oper[1] = o0;
				zi->oper[2] = SMALL(ref1);
				zi->oper[3] = SMALL(ref2);
				generate_post_ref(r, ci->oper[1], &refstate1);
				generate_post_ref(r, ci->oper[2], &refstate2);
				break;
			case I_GET_PAIR_RV:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				ref1 = generate_pre_ref(r, ci->oper[1], &refstate1, t2);
				o2 = generate_value(r, ci->oper[2], prg, t3);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_GET_PAIR_RV);
				zi->oper[1] = o0;
				zi->oper[2] = SMALL(ref1);
				zi->oper[3] = o2;
				generate_post_ref(r, ci->oper[1], &refstate1);
				break;
			case I_GET_PAIR_VR:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				o1 = generate_value(r, ci->oper[1], prg, t2);
				ref2 = generate_pre_ref(r, ci->oper[2], &refstate2, t3);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_GET_PAIR_VR);
				zi->oper[1] = o0;
				zi->oper[2] = o1;
				zi->oper[3] = SMALL(ref2);
				generate_post_ref(r, ci->oper[2], &refstate2);
				break;
			case I_GET_PAIR_VV:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				o1 = generate_value(r, ci->oper[1], prg, t2);
				o2 = generate_value(r, ci->oper[2], prg, t3);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_GET_PAIR_VV);
				zi->oper[1] = o0;
				zi->oper[2] = o1;
				zi->oper[3] = o2;
				break;
			case I_IF_BOUND:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JGE);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = VALUE(REG_C000);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_CAN_EMBED:
				zi = append_instr(r, Z_RFALSE);
				break;
			case I_IF_GFLAG:
				assert(ci->oper[0].tag == OPER_GFLAG);
				id = glbflag_global[ci->oper[0].value];
				mask = glbflag_mask[ci->oper[0].value];
				zi = append_instr(r, Z_TEST);
				zi->oper[0] = USERGLOBAL(id);
				zi->oper[1] = SMALL_OR_LARGE(mask);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_GREATER:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				o2 = generate_value(r, ci->oper[1], prg, t2);
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_GREATER_THAN);
				zi->oper[1] = o1;
				zi->oper[2] = o2;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JNZ);
				zi->oper[0] = VALUE(REG_TEMP);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_GVAR_EQ:
				assert(ci->oper[0].tag == OPER_GVAR);
				id = ((struct backend_pred *) prg->globalvarpred[ci->oper[0].value]->pred->backend)->user_global;
				if(ci->oper[1].tag == VAL_NONE) {
					o1 = SMALL(0);
				} else {
					o1 = generate_value(r, ci->oper[1], prg, t1);
				}
				zi = append_instr(r, Z_JE);
				zi->oper[0] = USERGLOBAL(id);
				zi->oper[1] = o1;
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_HAVE_UNDO:
				zi = append_instr(r, Z_LOADB);
				zi->oper[0] = SMALL(0);
				zi->oper[1] = SMALL(0x11);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_TEST);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = SMALL(0x10);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_HAVE_QUIT:
				if(!ci->subop) {
					if(ci->implicit == 0xffff) {
						zi = append_instr(r, Z_RFALSE);
					} else if(pred->routines[ci->implicit].reftrack == r_id) {
						zi = append_instr(r, Z_JUMP);
						zi->oper[0] = REL_LABEL(llabel[ci->implicit]);
						if(!encountered[ci->implicit]) {
							rstack[rsp++] = ci->implicit;
							encountered[ci->implicit] = 1;
						}
					} else {
						zi = append_instr(r, Z_RET);
						zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					}
				}
				break;
			case I_IF_HAVE_STATUS:
				assert(ci->oper[0].tag == VAL_RAW);
				if((ci->oper[0].value == 0) ^ !!ci->subop) {
					if(ci->implicit == 0xffff) {
						zi = append_instr(r, Z_RFALSE);
					} else if(pred->routines[ci->implicit].reftrack == r_id) {
						zi = append_instr(r, Z_JUMP);
						zi->oper[0] = REL_LABEL(llabel[ci->implicit]);
						if(!encountered[ci->implicit]) {
							rstack[rsp++] = ci->implicit;
							encountered[ci->implicit] = 1;
						}
					} else {
						zi = append_instr(r, Z_RET);
						zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					}
				}
				break;
			case I_IF_MATCH:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				if(ci->oper[1].tag == VAL_DICT) {
					// Check unboxed o1 against the mandatory part.
					o2 = LARGE(prg->dictmap[ci->oper[1].value]);
				} else {
					o2 = generate_value(r, ci->oper[1], prg, t2);
				}
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF_UNBOX);
				zi->oper[1] = o1;
				zi->store = REG_X + t1;
				zi = append_instr(r, Z_JE);
				zi->oper[0] = VALUE(REG_X + t1);
				zi->oper[1] = o2;
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_NIL:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JE);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = VALUE(REG_NIL);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_NUM:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JGE);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = VALUE(REG_4000);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_OBJ:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_ADD);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = LARGE(0x6001);	// takes 1..1ffe to 6002..7fff
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JG);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = LARGE(0x6001);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_OFLAG:
				assert(ci->oper[0].tag == OPER_OFLAG);
				id = ((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->object_flag;
				if(ci->oper[1].tag == VAL_OBJ && id < NZOBJFLAG) {
					zi = append_instr(r, Z_JA);
					zi->oper[0] = SMALL_OR_LARGE(ci->oper[1].value + 1);
					zi->oper[1] = SMALL(id);
				} else {
					o1 = generate_value(r, ci->oper[1], prg, t1);
					if(id < NZOBJFLAG) {
						zi = append_instr(r, Z_CALLVS);
						zi->oper[0] = ROUTINE(R_READ_FLAG);
						zi->oper[1] = o1;
						zi->oper[2] = SMALL(id);
						zi->store = REG_TEMP;
						zi = append_instr(r, Z_JNZ);
						zi->oper[0] = VALUE(REG_TEMP);
					} else {
						zi = append_instr(r, Z_CALL2S);
						zi->oper[0] = ROUTINE(extflagreaders[(id - NZOBJFLAG) / 8]);
						zi->oper[1] = o1;
						zi->store = REG_TEMP;
						zi = append_instr(r, Z_TEST);
						zi->oper[0] = VALUE(REG_TEMP);
						zi->oper[1] = SMALL(0x80 >> ((id - NZOBJFLAG) & 7));
					}
				}
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_OVAR_EQ:
				ll = r->next_label++;
				assert(ci->oper[0].tag == OPER_OVAR);
				o2 = generate_value(r, ci->oper[2], prg, t2);
				o1 = generate_value(r, ci->oper[1], prg, t1);
				if(ci->oper[1].tag != VAL_OBJ) {
					zi = append_instr(r, Z_CALL2S);
					zi->oper[0] = ROUTINE(R_DEREF_OBJ);
					zi->oper[1] = o1;
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_JZ);
					zi->oper[0] = VALUE(REG_TEMP);
					zi->branch = ll;
					o1 = VALUE(REG_TEMP);
				}
				if(ci->oper[0].value == DYN_HASPARENT) {
					zi = append_instr(r, Z_GET_PARENT);
					zi->oper[0] = o1;
					zi->store = REG_TEMP;
				} else {
					id = ((struct backend_pred *) prg->objvarpred[ci->oper[0].value]->pred->backend)->propbase_label;
					zi = append_instr(r, Z_LOADW);
					zi->oper[0] = REF(id);
					zi->oper[1] = o1;
					zi->store = REG_TEMP;
				}
				zi = append_instr(r, OP_LABEL(ll));
				zi = append_instr(r, Z_JE);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = o2;
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_PAIR:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_AND);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = VALUE(REG_E000);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JE);
				zi->oper[0] = VALUE(REG_TEMP);
				zi->oper[1] = VALUE(REG_C000);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_UNIFY:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				o2 = generate_value(r, ci->oper[1], prg, t2);
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_WOULD_UNIFY);
				zi->oper[1] = o1;
				zi->oper[2] = o2;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JNZ);
				zi->oper[0] = VALUE(REG_TEMP);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_WORD:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_IS_WORD);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JNZ);
				zi->oper[0] = VALUE(REG_TEMP);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_IF_UNKNOWN_WORD:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_IS_UNKNOWN_WORD);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_JNZ);
				zi->oper[0] = VALUE(REG_TEMP);
				if(ci->subop) zi->op ^= OP_NOT;
				if(ci->implicit == 0xffff) {
					zi->branch = RFALSE;
				} else if(pred->routines[ci->implicit].reftrack == r_id) {
					zi->branch = llabel[ci->implicit];
					if(!encountered[ci->implicit]) {
						rstack[rsp++] = ci->implicit;
						encountered[ci->implicit] = 1;
					}
				} else {
					ll = r->next_label++;
					zi->op ^= OP_NOT;
					zi->branch = ll;
					zi = append_instr(r, Z_RET);
					zi->oper[0] = ROUTINE(rlabel[ci->implicit]);
					zi = append_instr(r, OP_LABEL(ll));
				}
				break;
			case I_INVOKE_MULTI:
				assert(ci->oper[0].tag == OPER_PRED);
				id = ((struct backend_pred *) prg->predicates[ci->oper[0].value]->pred->backend)->global_label;
				if(!id) {
					printf("%s -> %s\n",
						pred->predname->printed_name,
						prg->predicates[ci->oper[0].value]->printed_name);
					assert(id);
				}
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_SIMPLE);
				zi->oper[1] = SMALL(0);
				zi = append_instr(r, Z_RET);
				zi->oper[0] = ROUTINE(id);
				break;
			case I_INVOKE_ONCE:
				assert(ci->oper[0].tag == OPER_PRED);
				id = ((struct backend_pred *) prg->predicates[ci->oper[0].value]->pred->backend)->global_label;
				if(!id) {
					printf("%s -> %s\n",
						pred->predname->printed_name,
						prg->predicates[ci->oper[0].value]->printed_name);
					assert(id);
				}
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_SIMPLE);
				zi->oper[1] = VALUE(REG_CHOICE);
				zi = append_instr(r, Z_RET);
				zi->oper[0] = ROUTINE(id);
				break;
			case I_INVOKE_TAIL_MULTI:
				assert(ci->oper[0].tag == OPER_PRED);
				id = ((struct backend_pred *) prg->predicates[ci->oper[0].value]->pred->backend)->global_label;
				if(!id) {
					printf("%s -> %s\n",
						pred->predname->printed_name,
						prg->predicates[ci->oper[0].value]->printed_name);
					assert(id);
				}
				zi = append_instr(r, Z_RET);
				zi->oper[0] = ROUTINE(id);
				break;
			case I_INVOKE_TAIL_ONCE:
				assert(ci->oper[0].tag == OPER_PRED);
				id = ((struct backend_pred *) prg->predicates[ci->oper[0].value]->pred->backend)->global_label;
				if(!id) {
					printf("%s -> %s\n",
						pred->predname->printed_name,
						prg->predicates[ci->oper[0].value]->printed_name);
					assert(id);
				}
				if(ci->subop == 2) {
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_SIMPLE);
					zi->oper[1] = VALUE(REG_CHOICE);
				} else if(ci->subop == 0) {
					ll = r->next_label++;
					zi = append_instr(r, Z_JNZ);
					zi->oper[0] = VALUE(REG_SIMPLE);
					zi->branch = ll;
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_SIMPLE);
					zi->oper[1] = VALUE(REG_CHOICE);
					zi = append_instr(r, OP_LABEL(ll));
				}
				zi = append_instr(r, Z_RET);
				zi->oper[0] = ROUTINE(id);
				break;
			case I_JOIN_WORDS:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_JOIN_WORDS);
				zi->oper[1] = o0;
				if(ci->oper[1].tag == OPER_VAR) {
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_STOREW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[1].value);
					zi->oper[2] = VALUE(REG_TEMP);
				} else if(ci->oper[1].tag == OPER_TEMP) {
					zi->store = REG_X + ci->oper[1].value;
				} else {
					assert(ci->oper[1].tag == OPER_ARG);
					zi->store = REG_A + ci->oper[1].value;
				}
				break;
			case I_JUMP:
				if(ci->oper[0].tag == OPER_FAIL) {
					zi = append_instr(r, Z_RFALSE);
				} else {
					assert(ci->oper[0].tag == OPER_RLAB);
					if(pred->routines[ci->oper[0].value].reftrack == r_id) {
						zi = append_instr(r, Z_JUMP);
						zi->oper[0] = REL_LABEL(llabel[ci->oper[0].value]);
						if(!encountered[ci->oper[0].value]) {
							rstack[rsp++] = ci->oper[0].value;
							encountered[ci->oper[0].value] = 1;
						}
					} else {
						assert(pred->routines[ci->oper[0].value].reftrack == ci->oper[0].value);
						zi = append_instr(r, Z_RET);
						zi->oper[0] = ROUTINE(rlabel[ci->oper[0].value]);
					}
				}
				break;
			case I_MAKE_PAIR_RR:
				ref0 = generate_pre_ref(r, ci->oper[0], &refstate0, t1);
				ref1 = generate_pre_ref(r, ci->oper[1], &refstate1, t2);
				ref2 = generate_pre_ref(r, ci->oper[2], &refstate2, t3);
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_PUSH_PAIR_RR);
				zi->oper[1] = SMALL(ref1);
				zi->oper[2] = SMALL(ref2);
				zi->store = ref0;
				generate_post_ref(r, ci->oper[0], &refstate0);
				generate_post_ref(r, ci->oper[1], &refstate1);
				generate_post_ref(r, ci->oper[2], &refstate2);
				break;
			case I_MAKE_PAIR_RV:
				ref0 = generate_pre_ref(r, ci->oper[0], &refstate0, t1);
				ref1 = generate_pre_ref(r, ci->oper[1], &refstate1, t2);
				o2 = generate_value(r, ci->oper[2], prg, t3);
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_PUSH_PAIR_RV);
				zi->oper[1] = SMALL(ref1);
				zi->oper[2] = o2;
				zi->store = ref0;
				generate_post_ref(r, ci->oper[0], &refstate0);
				generate_post_ref(r, ci->oper[1], &refstate1);
				break;
			case I_MAKE_PAIR_VR:
				ref0 = generate_pre_ref(r, ci->oper[0], &refstate0, t1);
				o1 = generate_value(r, ci->oper[1], prg, t2);
				ref2 = generate_pre_ref(r, ci->oper[2], &refstate2, t3);
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_PUSH_PAIR_VR);
				zi->oper[1] = o1;
				zi->oper[2] = SMALL(ref2);
				zi->store = ref0;
				generate_post_ref(r, ci->oper[0], &refstate0);
				generate_post_ref(r, ci->oper[2], &refstate2);
				break;
			case I_MAKE_PAIR_VV:
				ref0 = generate_pre_ref(r, ci->oper[0], &refstate0, t1);
				if(ci->oper[2].tag == VAL_NIL) {
					if(ci->oper[0].tag != OPER_VAR
					&& cr->instr[i + 1].op == I_MAKE_PAIR_VV
					&& ci->oper[0].tag == cr->instr[i + 1].oper[0].tag
					&& ci->oper[0].value == cr->instr[i + 1].oper[0].value
					&& ci->oper[0].tag == cr->instr[i + 1].oper[2].tag
					&& ci->oper[0].value == cr->instr[i + 1].oper[2].value) {
						if(cr->instr[i + 2].op == I_MAKE_PAIR_VV
						&& cr->instr[i + 1].oper[0].tag == cr->instr[i + 2].oper[0].tag
						&& cr->instr[i + 1].oper[0].value == cr->instr[i + 2].oper[0].value
						&& cr->instr[i + 1].oper[0].tag == cr->instr[i + 2].oper[2].tag
						&& cr->instr[i + 1].oper[0].value == cr->instr[i + 2].oper[2].value) {
							o1 = generate_value(r, ci->oper[1], prg, t2);
							o2 = generate_value(r, cr->instr[i + 1].oper[1], prg, t3);
							if(cr->instr[i + 2].oper[1].tag == OPER_VAR) {
								zi = append_instr(r, Z_LOADW);
								zi->oper[0] = VALUE(REG_ENV);
								zi->oper[1] = SMALL(3 + cr->instr[i + 2].oper[1].value);
								zi->store = REG_TEMP;
								o3 = VALUE(REG_TEMP);
							} else {
								o3 = generate_value(r, cr->instr[i + 2].oper[1], prg, 999);
							}
							zi = append_instr(r, Z_CALLVS);
							zi->oper[0] = ROUTINE(R_PUSH_LIST_VVV);
							zi->oper[1] = o3;
							zi->oper[2] = o2;
							zi->oper[3] = o1;
							zi->store = ref0;
							i += 2;
						} else {
							o1 = generate_value(r, ci->oper[1], prg, t2);
							o2 = generate_value(r, cr->instr[i + 1].oper[1], prg, t3);
							zi = append_instr(r, Z_CALLVS);
							zi->oper[0] = ROUTINE(R_PUSH_LIST_VV);
							zi->oper[1] = o2;
							zi->oper[2] = o1;
							zi->store = ref0;
							i++;
						}
					} else {
						o1 = generate_value(r, ci->oper[1], prg, t2);
						zi = append_instr(r, Z_CALL2S);
						zi->oper[0] = ROUTINE(R_PUSH_LIST_V);
						zi->oper[1] = o1;
						zi->store = ref0;
					}
				} else {
					o1 = generate_value(r, ci->oper[1], prg, t2);
					o2 = generate_value(r, ci->oper[2], prg, t3);
					zi = append_instr(r, Z_CALLVS);
					zi->oper[0] = ROUTINE(R_PUSH_PAIR_VV);
					zi->oper[1] = o1;
					zi->oper[2] = o2;
					zi->store = ref0;
				}
				generate_post_ref(r, ci->oper[0], &refstate0);
				break;
			case I_MAKE_VAR:
				if(ci->oper[0].tag == OPER_VAR) {
					n = 1;
					while(cr->instr[i + n].op == I_MAKE_VAR
					&& cr->instr[i + n].oper[0].tag == OPER_VAR
					&& cr->instr[i + n].oper[0].value == cr->instr[i + n - 1].oper[0].value + 1) {
						n++;
					}
					if(n == 1) {
						zi = append_instr(r, Z_CALL2N);
						zi->oper[0] = ROUTINE(R_PUSH_VAR_SETENV);
						zi->oper[1] = SMALL(3 + ci->oper[0].value);
					} else {
						zi = append_instr(r, Z_CALLVN);
						zi->oper[0] = ROUTINE(R_PUSH_VARS_SETENV);
						zi->oper[1] = SMALL(3 + ci->oper[0].value);
						zi->oper[2] = SMALL(3 + ci->oper[0].value + n - 1);
						i += n - 1;
					}
				} else if(cr->instr[i + 1].op == I_ASSIGN
				&& cr->instr[i + 1].oper[0].tag == OPER_VAR
				&& cr->instr[i + 1].oper[1].tag == ci->oper[0].tag
				&& cr->instr[i + 1].oper[1].value == ci->oper[0].value) {
					zi = append_instr(r, Z_CALL2S);
					zi->oper[0] = ROUTINE(R_PUSH_VAR_SETENV);
					zi->oper[1] = SMALL(3 + cr->instr[i + 1].oper[0].value);
					if(ci->oper[0].tag == OPER_ARG) {
						zi->store = REG_A + ci->oper[0].value;
					} else {
						assert(ci->oper[0].tag == OPER_TEMP);
						zi->store = REG_X + ci->oper[0].value;
					}
					i++;
				} else {
					zi = append_instr(r, Z_CALL1S);
					zi->oper[0] = ROUTINE(R_PUSH_VAR);
					if(ci->oper[0].tag == OPER_TEMP) {
						zi->store = REG_X + ci->oper[0].value;
					} else {
						assert(ci->oper[0].tag == OPER_ARG);
						zi->store = REG_A + ci->oper[0].value;
					}
				}
				break;
			case I_NEXT_CHILD_PUSH:
				assert(ci->oper[1].tag == OPER_RLAB);
				assert(pred->routines[ci->oper[1].value].reftrack == ci->oper[1].value);
				ll = r->next_label++;
				o1 = generate_value(r, ci->oper[0], prg, t1);
#if 0
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF_OBJ_FORCE);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				o1 = VALUE(REG_TEMP);
#endif
				zi = append_instr(r, Z_GET_SIBLING_N);
				zi->oper[0] = o1;
				zi->store = REG_A + 1;
				zi->branch = ll;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_TRY_ME_ELSE);
				zi->oper[1] = SMALL((2 + CHOICE_SIZEOF) * 2);
				zi->oper[2] = ROUTINE(rlabel[ci->oper[1].value]);
				zi = append_instr(r, OP_LABEL(ll));
				break;
			case I_NEXT_OBJ_PUSH:
				assert(ci->oper[1].tag == OPER_RLAB);
				assert(pred->routines[ci->oper[1].value].reftrack == ci->oper[1].value);
				ll = r->next_label++;
				o1 = generate_value(r, ci->oper[0], prg, t1);
#if 0
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF_OBJ_FORCE);
				zi->oper[1] = o1;
				zi->store = REG_A + 1;
#else
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_A + 1);
				zi->oper[1] = o1;
#endif
				zi = append_instr(r, Z_INC_JG);
				zi->oper[0] = SMALL(REG_A + 1);
				zi->oper[1] = REF(G_OBJECT_ID_END);
				zi->branch = ll;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_TRY_ME_ELSE);
				zi->oper[1] = SMALL((2 + CHOICE_SIZEOF) * 2);
				zi->oper[2] = ROUTINE(rlabel[ci->oper[1].value]);
				zi = append_instr(r, OP_LABEL(ll));
				break;
			case I_NEXT_OFLAG_PUSH:
				assert(ci->oper[0].tag == OPER_OFLAG);
				assert(ci->oper[2].tag == OPER_RLAB);
				assert(pred->routines[ci->oper[2].value].reftrack == ci->oper[2].value);
				id = ((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->propbase_label;
				ll = r->next_label++;
				o1 = generate_value(r, ci->oper[1], prg, t1);
#if 0
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF_OBJ_FORCE);
				zi->oper[1] = o1;
				zi->store = REG_TEMP;
				o1 = VALUE(REG_TEMP);
#endif
				zi = append_instr(r, Z_LOADW);
				zi->oper[0] = REF(id);
				zi->oper[1] = o1;
				zi->store = REG_A + 1;
				zi = append_instr(r, Z_JZ);
				zi->oper[0] = VALUE(REG_A + 1);
				zi->branch = ll;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_TRY_ME_ELSE);
				zi->oper[1] = SMALL((2 + CHOICE_SIZEOF) * 2);
				zi->oper[2] = ROUTINE(rlabel[ci->oper[2].value]);
				zi = append_instr(r, OP_LABEL(ll));
				break;
			case I_NOP:
			case I_NOP_DEBUG:
				break;
			case I_POP_CHOICE:
				assert(ci->oper[0].tag == OPER_NUM);
				if(cr->instr[i + 1].op == I_PUSH_CHOICE
				&& cr->instr[i + 1].oper[0].tag == OPER_NUM
				&& cr->instr[i + 1].oper[0].value <= ci->oper[0].value) {
					assert(cr->instr[i + 1].oper[1].tag == OPER_RLAB);
					assert(pred->routines[cr->instr[i + 1].oper[1].value].reftrack == cr->instr[i + 1].oper[1].value);
					if(ci->oper[0].value) {
						zi = append_instr(r, Z_CALLVN);
						zi->oper[0] = ROUTINE(R_RETRY_ME_ELSE);
						zi->oper[1] = SMALL(ci->oper[0].value);
						zi->oper[2] = ROUTINE(rlabel[cr->instr[i + 1].oper[1].value]);
					} else {
						zi = append_instr(r, Z_CALL2N);
						zi->oper[0] = ROUTINE(R_RETRY_ME_ELSE_0);
						zi->oper[1] = ROUTINE(rlabel[cr->instr[i + 1].oper[1].value]);
					}
					i++;
				} else {
					if(ci->oper[0].value) {
						zi = append_instr(r, Z_CALL2N);
						zi->oper[0] = ROUTINE(R_TRUST_ME);
						zi->oper[1] = SMALL(ci->oper[0].value);
					} else {
						zi = append_instr(r, Z_CALL1N);
						zi->oper[0] = ROUTINE(R_TRUST_ME_0);
					}
				}
				break;
			case I_POP_STOP:
				zi = append_instr(r, Z_SUB);
				zi->oper[0] = VALUE(REG_STOPAUX);
				zi->oper[1] = SMALL(1);
				zi->store = REG_COLL;
				zi = append_instr(r, Z_LOADW);
				zi->oper[0] = VALUE(REG_AUXBASE);
				zi->oper[1] = VALUE(REG_COLL);
				zi->store = REG_STOPCHOICE;
				zi = append_instr(r, Z_DEC);
				zi->oper[0] = SMALL(REG_COLL);
				zi = append_instr(r, Z_LOADW);
				zi->oper[0] = VALUE(REG_AUXBASE);
				zi->oper[1] = VALUE(REG_COLL);
				zi->store = REG_STOPAUX;
				break;
			case I_PREPARE_INDEX:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_DEREF_UNBOX);
				zi->oper[1] = o1;
				zi->store = REG_IDX;
				break;
			case I_PRINT_VAL:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(R_PRINT_OR_PUSH);
				zi->oper[1] = o1;
				break;
			case I_PRINT_WORDS:
				if(ci->subop == 1) {
					n = generate_output(prg, r, &cr->instr[i], 0, 1);
				} else if(ci->subop == 0) {
					// This can only happen for unreachable code.
					n = 1;
				} else {
					if(ci->subop == 3) {
						ll = r->next_label++;
						ll2 = r->next_label++;
						zi = append_instr(r, Z_JNZ);
						zi->oper[0] = VALUE(REG_FORWORDS);
						zi->branch = ll;
						n = generate_output(prg, r, &cr->instr[i], 0, 0);
						zi = append_instr(r, Z_JUMP);
						zi->oper[0] = REL_LABEL(ll2);
						zi = append_instr(r, OP_LABEL(ll));
					} else {
						assert(ci->subop == 2);
						n = generate_output(prg, r, &cr->instr[i], 1, 0);
						ll2 = 0; // prevent gcc warning
					}
					for(j = 0; j < n; j++) {
						if(cr->instr[i + j].op == I_PRINT_WORDS) {
							for(k = 0; k < 3 && cr->instr[i + j].oper[k].tag == OPER_WORD; k++) {
								w = prg->allwords[cr->instr[i + j].oper[k].value];
								oper[k] = LARGE(dictword_tag(prg, w));
							}
							if(k == 3) {
								zi = append_instr(r, Z_CALLVN);
								zi->oper[0] = ROUTINE(R_AUX_PUSH3);
								zi->oper[1] = oper[0];
								zi->oper[2] = oper[1];
								zi->oper[3] = oper[2];
							} else if(k == 2) {
								zi = append_instr(r, Z_CALLVN);
								zi->oper[0] = ROUTINE(R_AUX_PUSH2);
								zi->oper[1] = oper[0];
								zi->oper[2] = oper[1];
							} else {
								assert(k == 1);
								zi = append_instr(r, Z_CALL2N);
								zi->oper[0] = ROUTINE(R_AUX_PUSH1);
								zi->oper[1] = oper[0];
							}
						}
					}
					if(ci->subop == 3) {
						zi = append_instr(r, OP_LABEL(ll2));
					}
				}
				i += n - 1;
				break;
			case I_PROCEED:
				generate_proceed(r, ci->subop);
				break;
			case I_BREAKPOINT:
				generate_proceed(r, 0);
				break;
			case I_PUSH_CHOICE:
				assert(ci->oper[0].tag == OPER_NUM);
				assert(ci->oper[1].tag == OPER_RLAB);
				assert(pred->routines[ci->oper[1].value].reftrack == ci->oper[1].value);
				if(ci->oper[0].value) {
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = ROUTINE(R_TRY_ME_ELSE);
					zi->oper[1] = SMALL((ci->oper[0].value + CHOICE_SIZEOF) * 2);
					zi->oper[2] = ROUTINE(rlabel[ci->oper[1].value]);
				} else {
					zi = append_instr(r, Z_CALL2N);
					zi->oper[0] = ROUTINE(R_TRY_ME_ELSE_0);
					zi->oper[1] = ROUTINE(rlabel[ci->oper[1].value]);
				}
				break;
			case I_PUSH_STOP:
				assert(ci->oper[0].tag == OPER_RLAB);
				assert(pred->routines[ci->oper[0].value].reftrack == ci->oper[0].value);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_AUX_PUSH2);
				zi->oper[1] = VALUE(REG_STOPAUX);
				zi->oper[2] = VALUE(REG_STOPCHOICE);
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_STOPAUX);
				zi->oper[1] = VALUE(REG_COLL);
				zi = append_instr(r, Z_CALL2N);
				zi->oper[0] = ROUTINE(R_TRY_ME_ELSE_0);
				zi->oper[1] = ROUTINE(rlabel[ci->oper[0].value]);
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_STOPCHOICE);
				zi->oper[1] = VALUE(REG_CHOICE);
				break;
			case I_QUIT:
				zi = append_instr(r, Z_QUIT);
				break;
			case I_RESTART:
				zi = append_instr(r, Z_RESTART);
				break;
			case I_RESTORE:
				zi = append_instr(r, Z_CALL1N);
				zi->oper[0] = ROUTINE(R_LINE);
				zi = append_instr(r, Z_RESTORE);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_SPACE);
				zi->oper[1] = SMALL(4);
				break;
			case I_RESTORE_CHOICE:
				if(ci->oper[0].tag == OPER_VAR) {
					zi = append_instr(r, Z_LOADW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[0].value);
					zi->store = REG_CHOICE;
				} else {
					assert(ci->oper[0].tag == OPER_TEMP);
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_CHOICE);
					zi->oper[1] = VALUE(REG_X + ci->oper[0].value);
				}
				break;
			case I_SAVE:
				zi = append_instr(r, Z_CALL1S);
				zi->oper[0] = ROUTINE(R_SAVE);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = VALUE(REG_R_USIMPLE);
				zi->oper[1] = VALUE(REG_TEMP);
				zi->oper[2] = VALUE(REG_A + 0);
				generate_proceed(r, 0);
				break;
			case I_SAVE_CHOICE:
				if(ci->oper[0].tag == OPER_VAR) {
					zi = append_instr(r, Z_STOREW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[0].value);
					zi->oper[2] = VALUE(REG_CHOICE);
				} else if(ci->oper[0].tag == OPER_ARG) {
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_A + ci->oper[0].value);
					zi->oper[1] = VALUE(REG_CHOICE);
				} else {
					assert(ci->oper[0].tag == OPER_TEMP);
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_X + ci->oper[0].value);
					zi->oper[1] = VALUE(REG_CHOICE);
				}
				break;
			case I_SAVE_UNDO:
				zi = append_instr(r, Z_CALL1S);
				zi->oper[0] = ROUTINE(R_SAVE_UNDO);
				zi->store = REG_TEMP;
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = VALUE(REG_R_USIMPLE);
				zi->oper[1] = VALUE(REG_TEMP);
				zi->oper[2] = VALUE(REG_A + 0);
				ll = r->next_label++;
				generate_proceed(r, 0);
				break;
			case I_SELECT:
				assert(ci->oper[0].tag == OPER_NUM);
				n = ci->oper[0].value;
				if(ci->subop == SEL_P_RANDOM) {
					zi = append_instr(r, Z_RANDOM);
					zi->oper[0] = SMALL(n);
					zi->store = REG_IDX;
					zi = append_instr(r, Z_DEC);
					zi->oper[0] = SMALL(REG_IDX);
				} else {
					assert(ci->oper[1].tag == OPER_NUM);
					sel = ci->oper[1].value;
					zi = append_instr(r, Z_CALLVS);
					switch(ci->subop) {
					case SEL_STOPPING:
						zi->oper[0] = ROUTINE(R_SEL_STOPPING);
						break;
					case SEL_RANDOM:
						zi->oper[0] = ROUTINE(R_SEL_RANDOM);
						break;
					case SEL_T_RANDOM:
						zi->oper[0] = ROUTINE(R_SEL_T_RANDOM);
						break;
					case SEL_T_P_RANDOM:
						zi->oper[0] = ROUTINE(R_SEL_T_P_RANDOM);
						break;
					case SEL_CYCLING:
						zi->oper[0] = ROUTINE(R_SEL_CYCLING);
						break;
					default:
						assert(0); exit(1);
					}
					zi->oper[1] = SMALL(sel);
					zi->oper[2] = SMALL(n);
					zi->store = REG_IDX;
				}
				break;
			case I_SET_CONT:
				if(ci->oper[0].tag == OPER_RLAB) {
					assert(pred->routines[ci->oper[0].value].reftrack == ci->oper[0].value);
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_CONT);
					zi->oper[1] = ROUTINE(rlabel[ci->oper[0].value]);
				} else {
					assert(ci->oper[0].tag == OPER_FAIL);
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_CONT);
					zi->oper[1] = SMALL(0);
				}
				break;
			case I_SET_GFLAG:
				assert(ci->oper[0].tag == OPER_GFLAG);
				id = glbflag_global[ci->oper[0].value];
				mask = glbflag_mask[ci->oper[0].value];
				if(ci->subop) {
					zi = append_instr(r, Z_OR);
					zi->oper[0] = USERGLOBAL(id);
					zi->oper[1] = SMALL_OR_LARGE(mask);
					zi->store = DEST_USERGLOBAL(id);
				} else {
					zi = append_instr(r, Z_AND);
					zi->oper[0] = USERGLOBAL(id);
					zi->oper[1] = LARGE(0xffff ^ mask);
					zi->store = DEST_USERGLOBAL(id);
				}
				break;
			case I_SET_GVAR:
				assert(ci->oper[0].tag == OPER_GVAR);
				id = ((struct backend_pred *) prg->globalvarpred[ci->oper[0].value]->pred->backend)->user_global;
				if(ci->oper[1].tag == VAL_NONE) {
					o1 = SMALL(0);
				} else {
					o1 = generate_value(r, ci->oper[1], prg, t1);
				}
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_SET_LONGTERM_VAR);
				zi->oper[1] = REF(G_USER_GLOBALS);
				zi->oper[2] = SMALL(id);
				zi->oper[3] = o1;
				break;
			case I_SET_OFLAG:
				assert(ci->oper[0].tag == OPER_OFLAG);
				id = ((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->object_flag;
				assert(id < NZOBJFLAG);
				if(prg->objflagpred[ci->oper[0].value]->pred->flags & PREDF_DYN_LINKAGE) {
					o1 = generate_value(r, ci->oper[1], prg, t1);
					zi = append_instr(r, Z_STORE);
					zi->oper[0] = SMALL(REG_TEMP);
					zi->oper[1] = REF(((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->propbase_label);
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = ROUTINE(ci->subop? R_SET_LISTFLAG : R_RESET_LISTFLAG);
					zi->oper[1] = o1;
					zi->oper[2] = SMALL(id);
					zi->oper[3] = SMALL_USERGLOBAL(((struct backend_pred *) prg->objflagpred[ci->oper[0].value]->pred->backend)->user_global);
				} else {
					if(ci->oper[1].tag == VAL_OBJ) {
						zi = append_instr(r, ci->subop? Z_SET_ATTR : Z_CLEAR_ATTR);
						zi->oper[0] = SMALL_OR_LARGE(ci->oper[1].value + 1);
						zi->oper[1] = SMALL(id);
					} else {
						o1 = generate_value(r, ci->oper[1], prg, t1);
						zi = append_instr(r, Z_CALLVN);
						zi->oper[0] = ROUTINE(ci->subop? R_SET_FLAG : R_RESET_FLAG);
						zi->oper[1] = o1;
						zi->oper[2] = SMALL(id);
					}
				}
				break;
			case I_SET_OVAR:
				assert(ci->oper[0].tag == OPER_OVAR);
				if(ci->oper[0].value == DYN_HASPARENT) {
					o1 = generate_value(r, ci->oper[1], prg, t1);
					if(ci->oper[2].tag == VAL_NONE) {
						if(ci->oper[1].tag == VAL_OBJ) {
							zi = append_instr(r, Z_REMOVE_OBJ);
							zi->oper[0] = o1;
						} else {
							zi = append_instr(r, Z_CALL2N);
							zi->oper[0] = ROUTINE(R_RESET_PARENT);
							zi->oper[1] = o1;
						}
					} else {
						o2 = generate_value(r, ci->oper[2], prg, t2);
						if(ci->oper[1].tag == VAL_OBJ && ci->oper[2].tag == VAL_OBJ) {
							zi = append_instr(r, Z_INSERT_OBJ);
							zi->oper[0] = o1;
							zi->oper[1] = o2;
						} else {
							zi = append_instr(r, Z_CALLVN);
							zi->oper[0] = ROUTINE(R_SET_PARENT);
							zi->oper[1] = o1;
							zi->oper[2] = o2;
						}
					}
				} else {
					id = ((struct backend_pred *) prg->objvarpred[ci->oper[0].value]->pred->backend)->propbase_label;
					o1 = generate_value(r, ci->oper[1], prg, t1);
					if(ci->oper[2].tag == VAL_NONE) {
						o2 = SMALL(0);
					} else {
						o2 = generate_value(r, ci->oper[2], prg, t2);
					}
					zi = append_instr(r, Z_CALLVN);
					zi->oper[0] = ROUTINE(R_SET_LONGTERM_VAR);
					zi->oper[1] = REF(id);
					zi->oper[2] = o1;
					zi->oper[3] = o2;
				}
				break;
			case I_SPLIT_LIST:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				o1 = generate_value(r, ci->oper[1], prg, t2);
				o2 = generate_value(r, ci->oper[2], prg, t3);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_SPLIT_LIST);
				zi->oper[1] = o0;
				zi->oper[2] = o1;
				zi->oper[3] = o2;
				break;
			case I_SPLIT_WORD:
				o0 = generate_value(r, ci->oper[0], prg, t1);
				zi = append_instr(r, Z_CALL2S);
				zi->oper[0] = ROUTINE(R_SPLIT_WORD);
				zi->oper[1] = o0;
				if(ci->oper[1].tag == OPER_VAR) {
					zi->store = REG_TEMP;
					zi = append_instr(r, Z_STOREW);
					zi->oper[0] = VALUE(REG_ENV);
					zi->oper[1] = SMALL(3 + ci->oper[1].value);
					zi->oper[2] = VALUE(REG_TEMP);
				} else if(ci->oper[1].tag == OPER_TEMP) {
					zi->store = REG_X + ci->oper[1].value;
				} else {
					assert(ci->oper[1].tag == OPER_ARG);
					zi->store = REG_A + ci->oper[1].value;
				}
				break;
			case I_STOP:
				zi = append_instr(r, Z_STORE);
				zi->oper[0] = SMALL(REG_CHOICE);
				zi->oper[1] = VALUE(REG_STOPCHOICE);
				zi = append_instr(r, Z_RFALSE);
				break;
			case I_TRACEPOINT:
				if(ci->subop == TR_ENTER) {
					assert(ci->oper[2].tag == OPER_PRED);
					id = ((struct backend_pred *) prg->predicates[ci->oper[2].value]->pred->backend)->trace_output_label;
					if(id) {
						zi = append_instr(r, Z_CALLVN);
						zi->oper[0] = ROUTINE(R_TRACE_ENTER);
						zi->oper[1] = ROUTINE(id);
						zi->oper[2] = SMALL_OR_LARGE(ci->oper[1].value);
						zi->oper[3] = SMALL(ci->oper[0].value);
					}
				} else if(ci->subop == TR_QUERY) {
					assert(ci->oper[2].tag == OPER_PRED);
					if(prg->predicates[ci->oper[2].value]->builtin != BI_GETINPUT) {
						id = ((struct backend_pred *) prg->predicates[ci->oper[2].value]->pred->backend)->trace_output_label;
						if(id) {
							zi = append_instr(r, Z_CALLVN);
							zi->oper[0] = ROUTINE(R_TRACE_QUERY);
							zi->oper[1] = ROUTINE(id);
							zi->oper[2] = SMALL_OR_LARGE(ci->oper[1].value);
							zi->oper[3] = SMALL(ci->oper[0].value);
						}
					}
				} else if(ci->subop == TR_MQUERY) {
					assert(ci->oper[2].tag == OPER_PRED);
					id = ((struct backend_pred *) prg->predicates[ci->oper[2].value]->pred->backend)->trace_output_label;
					if(id) {
						zi = append_instr(r, Z_CALLVN);
						zi->oper[0] = ROUTINE(R_TRACE_QUERY);
						zi->oper[1] = ROUTINE(id);
						zi->oper[2] = SMALL_OR_LARGE(ci->oper[1].value);
						zi->oper[3] = SMALL(ci->oper[0].value);
					}
				}
				break;
			case I_TRANSCRIPT:
				if(ci->subop) {
					zi = append_instr(r, Z_CALL1N);
					zi->oper[0] = ROUTINE(R_SCRIPT_ON);
				} else {
					zi = append_instr(r, Z_OUTPUT_STREAM);
					zi->oper[0] = LARGE(0xfffe);
				}
				break;
			case I_UNDO:
				// This flag prevents any attempts to undo back to before the
				// first successfully saved undo point, since doing that
				// crashes frotz.
				zi = append_instr(r, Z_TESTN);
				zi->oper[0] = USERGLOBAL(undoflag_global);
				zi->oper[1] = SMALL_OR_LARGE(undoflag_mask);
				zi->branch = RFALSE;
				zi = append_instr(r, Z_RESTORE_UNDO);
				zi->store = REG_TEMP;
				break;
			case I_UNIFY:
				o1 = generate_value(r, ci->oper[0], prg, t1);
				o2 = generate_value(r, ci->oper[1], prg, t2);
				zi = append_instr(r, Z_CALLVN);
				zi->oper[0] = ROUTINE(R_UNIFY);
				zi->oper[1] = o1;
				zi->oper[2] = o2;
				break;
			default:
				printf("unimplemented op %s in R%d of %s\n",
					opinfo[ci->op].name,
					sub_r_id,
					pred->predname->printed_name);
				assert(0); exit(1);
			}
		}
	}

	if(next_temp > max_temp) max_temp = next_temp;
}

static void straighten_jumps(struct routine *r) {
	int i;
	uint16_t ll;

	for(i = 0; i < r->ninstr; i++) {
		if(r->instr[i].op == Z_JUMP) {
			ll = r->instr[i].oper[0] & 0xffff;
			if(i + 1 < r->ninstr && r->instr[i + 1].op == OP_LABEL(ll)) {
				r->instr[i].op = OP_NOP;
				r->instr[i].oper[0] = 0;
			}
		}
	}
}

void compile_predicate(struct predname *predname, struct program *prg) {
	struct predicate *pred = predname->pred;
	struct backend_pred *bp = pred->backend;
	struct routine *r;
	int i;

	if(bp->trace_output_label) {
		compile_trace_output(predname, bp->trace_output_label);
	}

	if(bp->global_label) {
		uint16_t rlabel[pred->nroutine];

		memset(rlabel, 0, pred->nroutine * sizeof(uint16_t));
		for(i = 0; i < pred->nroutine; i++) {
			if(pred->routines[i].reftrack == i) {
				if(i == pred->normal_entry) {
					rlabel[i] = bp->global_label;
				} else {
					rlabel[i] = make_routine_label();
				}
			}
		}
		for(i = 0; i < pred->nroutine; i++) {
			if(pred->routines[i].reftrack == i) {
				r = make_routine(rlabel[i], 0);
				generate_code(prg, r, pred, i, rlabel);
				straighten_jumps(r);
			}
		}
	}
}

void compile_endings_check(struct routine *r, struct endings_point *pt, int level, int have_allocated) {
	int i, j;
	struct zinstr *zi;
	uint16_t ll, ll2;
	uint8_t zscii;

	zi = append_instr(r, Z_DEC_JL);
	zi->oper[0] = SMALL(REG_LOCAL+1);
	zi->oper[1] = SMALL(0);
	zi->branch = RFALSE;
	zi = append_instr(r, Z_LOADB);
	zi->oper[0] = VALUE(REG_LOCAL+0);
	zi->oper[1] = VALUE(REG_LOCAL+1);
	zi->store = REG_LOCAL+2;
	for(i = 0; i < pt->nway; i++) {
		if(i == pt->nway - 1) {
			ll = RFALSE;
		} else {
			ll = r->next_label++;
		}
		zscii = unicode_to_zscii(pt->ways[i]->letter);
		if(verbose >= 2) {
			for(j = 0; j < level; j++) printf("    ");
			if(zscii >= 'a' && zscii <= 'z') {
				printf("If word[len - %d] == '%c'\n", level + 1, zscii);
			} else {
				printf("If word[len - %d] == zscii(%d)\n", level + 1, zscii);
			}
		}
		zi = append_instr(r, Z_JNE);
		zi->oper[0] = VALUE(REG_LOCAL+2);
		zi->oper[1] = SMALL(zscii);
		zi->branch = ll;
		if(pt->ways[i]->final) {
			if(!have_allocated) {
				if(verbose >= 2) {
					for(j = 0; j < level + 1; j++) printf("    ");
					printf("Allocate copy of length len - %d.\n", level + 1);
				}
				zi = append_instr(r, Z_CALLVS);
				zi->oper[0] = ROUTINE(R_COPY_INPUT_WORD);
				zi->oper[1] = VALUE(REG_LOCAL+0);
				zi->oper[2] = VALUE(REG_LOCAL+1);
				zi->store = REG_LOCAL+3;
			} else {
				zi = append_instr(r, Z_STOREB);
				zi->oper[0] = VALUE(REG_LOCAL+3);
				zi->oper[1] = SMALL(7);
				zi->oper[2] = VALUE(REG_LOCAL+1);
			}
			if(verbose >= 2) {
				for(j = 0; j < level + 1; j++) printf("    ");
				printf("Try with length len - %d.\n", level + 1);
			}
			zi = append_instr(r, Z_ADD);
			zi->oper[0] = VALUE(REG_LOCAL+3);
			zi->oper[1] = SMALL(6);
			zi->store = REG_LOCAL+2;
			zi = append_instr(r, Z_TOKENISE);
			zi->oper[0] = VALUE(REG_LOCAL+2);
			zi->oper[1] = VALUE(REG_LOCAL+3);
			zi = append_instr(r, Z_LOADW);
			zi->oper[0] = VALUE(REG_LOCAL+3);
			zi->oper[1] = SMALL(1);
			zi->store = REG_LOCAL+2;
			if(pt->ways[i]->more.nway) {
				ll2 = r->next_label++;
				zi = append_instr(r, Z_JZ);
				zi->oper[0] = VALUE(REG_LOCAL+2);
				zi->branch = ll2;
				zi = append_instr(r, Z_RET);
				zi->oper[0] = VALUE(REG_LOCAL+2);
				zi = append_instr(r, OP_LABEL(ll2));
			} else {
				zi = append_instr(r, Z_RET);
				zi->oper[0] = VALUE(REG_LOCAL+2);
			}
		}
		if(pt->ways[i]->more.nway) {
			compile_endings_check(r, &pt->ways[i]->more, level + 1, have_allocated || pt->ways[i]->final);
		}
		if(ll != RFALSE) {
			zi = append_instr(r, OP_LABEL(ll));
		}
	}
}

static void set_initial_reg(uint8_t *core, int reg, uint16_t value) {
	core[2 * (reg - 0x10) + 0] = value >> 8;
	core[2 * (reg - 0x10) + 1] = value & 0xff;
}

static void initial_values(struct program *prg, uint8_t *zcore, uint16_t addr_globals, uint16_t addr_lts, int ltssize, uint16_t *extflagarrays, int n_extflag) {
	struct eval_state es;
	int i, j, status, more;
	struct predname *predname;
	struct predicate *pred;
	struct backend_pred *bp;
	struct backend_wobj *wobj;
	uint8_t seen[prg->nworldobj];
	value_t eval_args[2];
	uint16_t addr, value;
	int lttop = 0;
	value_t obj1, obj2;

	init_evalstate(&es, prg);

	for(i = 0; i < prg->npredicate; i++) {
		predname = prg->predicates[i];
		pred = predname->pred;
		bp = pred->backend;
		if(pred->flags & PREDF_DYNAMIC) {
			if(predname->arity == 0) {
				eval_reinitialize(&es);
				if(eval_initial(&es, predname, 0)) {
					zcore[addr_globals + 2 * (user_global_base + bp->user_global) + 0] |= bp->user_flag_mask >> 8;
					zcore[addr_globals + 2 * (user_global_base + bp->user_global) + 1] |= bp->user_flag_mask & 0xff;
				}
				if(es.errorflag) exit(1);
			} else if(predname->arity == 1) {
				if(pred->flags & PREDF_GLOBAL_VAR) {
					eval_reinitialize(&es);
					eval_args[0] = eval_makevar(&es);
					if(eval_initial(&es, predname, eval_args)) {
						addr = addr_globals + 2 * (user_global_base + bp->user_global);
						if(eval_args[0].tag == VAL_REF) {
							report(
								LVL_ERR,
								0,
								"Initial value of global variable %s must be bound.",
								predname->printed_name);
							exit(1);
						} else if(eval_args[0].tag == VAL_PAIR) {
							value = 0x8000 | lttop;
							zcore[addr + 0] = value >> 8;
							zcore[addr + 1] = value & 0xff;
							value = addr;
							addr = addr_lts + lttop * 2;
							zcore[addr + 2] = value >> 8;
							zcore[addr + 3] = value & 0xff;
							value = 2 + render_eval_value(zcore + addr + 4, ltssize - lttop - 2, eval_args[0], &es, predname, 0);
							zcore[addr + 0] = value >> 8;
							zcore[addr + 1] = value & 0xff;
							lttop += value;
						} else {
							value = tag_eval_value(eval_args[0], prg);
							zcore[addr + 0] = value >> 8;
							zcore[addr + 1] = value & 0xff;
						}
					}
					if(es.errorflag) exit(1);
				} else {
					int last_wobj = 0;

					for(j = 0; j < prg->nworldobj; j++) {
						wobj = &backendwobj[j];
						if(pred->flags & PREDF_FIXED_FLAG) {
							assert(predname->nfixedvalue >= prg->nworldobj);
							status = predname->fixedvalues[j];
						} else {
							eval_reinitialize(&es);
							eval_args[0] = (value_t) {VAL_OBJ, j};
							status = eval_initial(&es, predname, eval_args);
							if(es.errorflag) exit(1);
						}
						if(status) {
							if(bp->object_flag >= NZOBJFLAG) {
								int num = (bp->object_flag - NZOBJFLAG) / 8;
								int mask = 0x80 >> ((bp->object_flag - NZOBJFLAG) & 7);
								assert(num < n_extflag);
								addr = global_labels[extflagarrays[num]] + 1 + j;
								zcore[addr] |= mask;
							} else {
								zcore[wobj->addr_objtable + bp->object_flag / 8] |= 0x80 >> (bp->object_flag & 7);
							}
							if(pred->flags & PREDF_DYN_LINKAGE) {
								addr = global_labels[bp->propbase_label] + 2 + j * 2;
								zcore[addr + 0] = last_wobj >> 8;
								zcore[addr + 1] = last_wobj & 0xff;
								last_wobj = j + 1;
							}
						}
					}
					if(pred->flags & PREDF_DYN_LINKAGE) {
						zcore[addr_globals + 2 * (user_global_base + bp->user_global) + 0] = last_wobj >> 8;
						zcore[addr_globals + 2 * (user_global_base + bp->user_global) + 1] = last_wobj & 0xff;
					}
				}
			} else if(predname->arity == 2) {
				for(j = 0; j < prg->nworldobj; j++) {
					wobj = &backendwobj[j];
					eval_reinitialize(&es);
					eval_args[0] = (value_t) {VAL_OBJ, j};
					eval_args[1] = eval_makevar(&es);
					if(predname->builtin != BI_HASPARENT) {
						if(eval_initial(&es, predname, eval_args)) {
							addr = global_labels[bp->propbase_label] + 2 + j * 2;
							if(eval_args[1].tag == VAL_REF) {
								report(
									LVL_ERR,
									0,
									"Initial value of dynamic per-object variable %s, for #%s, must be bound.",
									predname->printed_name,
									prg->worldobjnames[j]->name);
								exit(1);
							} else if(eval_args[1].tag == VAL_PAIR) {
								value = 0x8000 | lttop;
								zcore[addr + 0] = value >> 8;
								zcore[addr + 1] = value & 0xff;
								value = addr;
								addr = addr_lts + lttop * 2;
								zcore[addr + 2] = value >> 8;
								zcore[addr + 3] = value & 0xff;
								value = 2 + render_eval_value(
									zcore + addr + 4,
									ltssize - lttop - 2,
									eval_args[1],
									&es,
									predname,
									0);
								zcore[addr + 0] = value >> 8;
								zcore[addr + 1] = value & 0xff;
								lttop += value;
							} else {
								value = tag_eval_value(eval_args[1], prg);
								zcore[addr + 0] = value >> 8;
								zcore[addr + 1] = value & 0xff;
							}
						}
					}
					if(es.errorflag) exit(1);
				}
			}
		}
	}

	for(i = 0; i < prg->nworldobj; i++) {
		backendwobj[i].initialparent = -1;
	}

	eval_reinitialize(&es);
	eval_args[0] = eval_makevar(&es);
	eval_args[1] = eval_makevar(&es);
	more = eval_initial_multi(&es, find_builtin(prg, BI_HASPARENT), eval_args);
	while(more) {
		obj1 = eval_deref(eval_args[0], &es);
		if(obj1.tag == VAL_OBJ) {
			wobj = &backendwobj[obj1.value];
			if(wobj->initialparent < 0) {
				obj2 = eval_deref(eval_args[1], &es);
				if(obj2.tag != VAL_OBJ) {
					report(
						LVL_ERR,
						0,
						"Attempted to set the initial parent of #%s to a non-object.",
						prg->worldobjnames[obj1.value]->name);
					exit(1);
				}
				wobj->initialparent = obj2.value;
				memset(seen, 0, prg->nworldobj);
				for(i = obj1.value; i >= 0; i = backendwobj[i].initialparent) {
					if(seen[i]) {
						report(
							LVL_ERR,
							0,
							"Badly formed object tree! #%s is nested in itself.",
							prg->worldobjnames[i]->name);
						exit(1);
					}
					seen[i] = 1;
				}
				i = obj1.value;
				j = obj2.value;
				zcore[wobj->addr_objtable + 6] = (j + 1) >> 8;
				zcore[wobj->addr_objtable + 7] = (j + 1) & 0xff;
				addr = backendwobj[j].addr_objtable + 10;
				while(zcore[addr] | zcore[addr + 1]) {
					addr = backendwobj[((zcore[addr] << 8) | zcore[addr + 1]) - 1].addr_objtable + 8;
				}
				zcore[addr + 0] = (i + 1) >> 8;
				zcore[addr + 1] = (i + 1) & 0xff;
			}
		} else {
			report(
				LVL_ERR,
				0,
				"Initial parent defined with a non-object as the first parameter.");
			exit(1);
		}
		more = eval_initial_next(&es);
	}

	free_evalstate(&es);

	set_initial_reg(zcore + addr_globals, REG_LTTOP, lttop);
	set_initial_reg(zcore + addr_globals, REG_LTMAX, lttop);
}

void backend_z(
	char *filename,
	char *format,
	char *coverfname,
	char *coveralt,
	int heapsize,
	int auxsize,
	int ltssize,
	int strip,
	struct program *prg,
	struct arena *arena)
{
	int nglobal;
	uint16_t addr_abbrevtable, addr_abbrevstr, addr_objtable, addr_globals, addr_static;
	uint16_t addr_scratch, addr_heap, addr_heapend, addr_aux, addr_lts, addr_dictionary, addr_seltable;
	uint32_t org;
	uint32_t filesize;
	int i, j, k;
	struct backend_wobj *wobj;
	struct global_string *gs;
	struct predname *predname;
	struct predicate *pred;
	uint16_t checksum = 0;
	uint16_t entrypc, himem;
	int n_extflag = 0;
	uint16_t *extflagarrays = 0;
	struct routine *r;
	struct zinstr *zi;
	int zversion, packfactor;
	struct backend_pred *bp;

	if(!strcmp(format, "z5")) {
		zversion = 5;
		packfactor = 4;
	} else {
		zversion = 8;
		packfactor = 8;
	}

	assert(!next_routine_num);
	assert(nrtroutine == R_FIRST_FREE);
	next_routine_num = R_FIRST_FREE;
	routines = malloc(next_routine_num * sizeof(struct routine *));
	for(i = 0; i < nrtroutine; i++) {
		r = calloc(1, sizeof(*r));
		routines[rtroutines[i].rnumber] = r;
		r->nlocal = rtroutines[i].nlocal;
		r->next_label = 2;
		r->actual_routine = 0xffff;
		while(rtroutines[i].instr[r->ninstr].op != Z_END) r->ninstr++;
		r->instr = malloc(r->ninstr * sizeof(struct zinstr));
		memcpy(r->instr, rtroutines[i].instr, r->ninstr * sizeof(struct zinstr));
	}

	r = routines[R_SRCFILENAME];
	for(i = 0; i < nsourcefile; i++) {
		zi = append_instr(r, Z_JE);
		zi->oper[0] = VALUE(REG_LOCAL+0);
		zi->oper[1] = SMALL(i);
		zi->branch = i + 1;
	}
	zi = append_instr(r, Z_PRINTLIT);
	zi->string = "?";
	zi = append_instr(r, Z_RFALSE);
	for(i = 0; i < nsourcefile; i++) {
		zi = append_instr(r, OP_LABEL(i + 1));
		zi = append_instr(r, Z_PRINTLIT);
		zi->string = sourcefile[i];
		zi = append_instr(r, Z_RFALSE);
	}

	tracing_enabled = !(prg->optflags & OPTF_NO_TRACE);

	backendwobj = calloc(prg->nworldobj, sizeof(struct backend_wobj));
	for(i = 0; i < prg->nworldobj; i++) {
		init_backend_wobj(prg, i, &backendwobj[i], strip);
	}

	for(i = 0; i < prg->npredicate; i++) {
		init_backend_pred(prg->predicates[i]->pred);
	}

	(void) make_global_label(); // ensure that the array is allocated

	if(prg->endings_root.nway) {
		if(verbose >= 2) printf("Stemming algorithm:\n");
		compile_endings_check(routines[R_TRY_STEMMING], &prg->endings_root, 0, 0);
	} else {
		zi = append_instr(routines[R_TRY_STEMMING], Z_RFALSE);
	}

	if(prg->nresource) {
		zi = append_instr(routines[R_EMBED_RES], Z_AND);
		zi->oper[0] = VALUE(REG_LOCAL+0);
		zi->oper[1] = VALUE(REG_NIL);
		zi->store = REG_LOCAL+0;
		for(i = 0; i < prg->nresource; i++) {
			int len = 1 + strlen(prg->resources[i].alt) + 2;
			char withbrackets[len];

			snprintf(withbrackets, len, "[%s]", prg->resources[i].alt);
			if(i < prg->nresource - 1) {
				if(i) {
					zi = append_instr(routines[R_EMBED_RES], Z_JNE);
					zi->oper[0] = VALUE(REG_LOCAL+0);
					zi->oper[1] = SMALL_OR_LARGE(i);
				} else {
					zi = append_instr(routines[R_EMBED_RES], Z_JNZ);
					zi->oper[0] = VALUE(REG_LOCAL+0);
				}
				zi->branch = i + 1;
			}
			generate_output_from_utf8(prg, routines[R_EMBED_RES], 1, 1, withbrackets);
			zi = append_instr(routines[R_EMBED_RES], Z_RFALSE);
			if(i < prg->nresource - 1) {
				zi = append_instr(routines[R_EMBED_RES], OP_LABEL(i + 1));
			}
		}
	} else {
		zi = append_instr(routines[R_EMBED_RES], Z_RFALSE);
	}

	user_flags_global = next_user_global++;
	undoflag_global = alloc_user_flag(&undoflag_mask);
	assert(undoflag_global == 0);	// hardcoded in runtime_z.c
	assert(undoflag_mask == 1);	// hardcoded in runtime_z.c

	glbflag_global = malloc(prg->nglobalflag * sizeof(uint16_t));
	glbflag_mask = malloc(prg->nglobalflag * sizeof(uint16_t));

	for(i = 0; i < prg->nglobalflag; i++) {
		glbflag_global[i] = alloc_user_flag(&glbflag_mask[i]);
		predname = prg->globalflagpred[i];
		if(predname) {
			pred = predname->pred;
			bp = pred->backend;
			bp->user_global = glbflag_global[i];
			bp->user_flag_mask = glbflag_mask[i];
		}
	}

	for(i = 0; i < prg->nglobalvar; i++) {
		predname = prg->globalvarpred[i];
		pred = predname->pred;
		bp = pred->backend;
		bp->user_global = next_user_global++;
	}

	for(i = 0; i < prg->nobjflag; i++) {
		predname = prg->objflagpred[i];
		pred = predname->pred;
		bp = pred->backend;
		if(!(pred->flags & PREDF_FIXED_FLAG)) {
			if(verbose >= 2) {
				printf("Debug: Dynamic flag %d: %s", next_flag, predname->printed_name);
				if(pred->flags & PREDF_DYN_LINKAGE) {
					printf(", with linkage because of %s:%d",
						FILEPART(pred->dynamic->linkage_due_to_line),
						LINEPART(pred->dynamic->linkage_due_to_line));
				}
				printf("\n");
			}
			if(next_flag >= NZOBJFLAG) {
				report(LVL_ERR, 0, "Too many dynamic per-object flags! Max %d.", NZOBJFLAG);
				exit(1);
			}
			bp->object_flag = next_flag++;
			if(pred->flags & PREDF_DYN_LINKAGE) {
				bp->user_global = next_user_global++;
				bp->propbase_label = make_global_label();
				bp->global_label = make_routine_label();
				pred->flags |= PREDF_INVOKED_SIMPLE | PREDF_INVOKED_MULTI;
			}
		}
	}

	for(i = 0; i < prg->nobjvar; i++) {
		predname = prg->objvarpred[i];
		pred = predname->pred;
		bp = pred->backend;
		if(predname->builtin != BI_HASPARENT) {
			bp->propbase_label = make_global_label();
		}
	}

	for(i = 0; i < prg->npredicate; i++) {
		predname = prg->predicates[i];
		pred = predname->pred;
		bp = pred->backend;

		if((pred->flags & PREDF_INVOKED)
		|| (predname->builtin == BI_HASPARENT)
		|| (predname->builtin == BI_OBJECT)) {
			if(!bp->global_label) {
				bp->global_label = make_routine_label();
			}
		}

		if(tracing_enabled && bp->global_label) {
			bp->trace_output_label = make_routine_label();
		}

		if(pred->flags & PREDF_FIXED_FLAG) {
			assert(predname->arity == 1);
			assert(!(pred->flags & PREDF_DYN_LINKAGE));
			if(verbose >= 2) {
				printf("Debug: %s flag %d: %s\n",
					(next_flag >= NZOBJFLAG)? "Extended fixed" : "Fixed",
					next_flag,
					predname->printed_name);
			}
			bp->object_flag = next_flag++;
		}
	}

	if(next_flag > NZOBJFLAG) {
		n_extflag = (next_flag - NZOBJFLAG + 7) / 8;
		extflagreaders = malloc(n_extflag * sizeof(uint16_t));
		extflagarrays = malloc(n_extflag * sizeof(uint16_t));
		for(i = 0; i < n_extflag; i++) {
			extflagreaders[i] = make_routine_label();
			extflagarrays[i] = make_global_label();
			compile_extflag_reader(extflagreaders[i], extflagarrays[i]);
		}
	}

	for(i = 0; i < prg->npredicate; i++) {
		compile_predicate(prg->predicates[i], prg);
	}

#if 0
	printf("predicates compiled\n");
#endif

	nglobal = (REG_X - 0x10) + max_temp;
	user_global_base = nglobal;
	nglobal += next_user_global;

	resolve_rnum(R_ENTRY);
	for(i = 1; i < 64; i++) {
		if(propdefault[i] & 0x8000) {
			resolve_rnum(propdefault[i] & 0x7fff);
		}
	}
	for(i = 0; i < prg->nworldobj; i++) {
		wobj = &backendwobj[i];
		for(j = 63; j > 0; j--) {
			int n = wobj->npropword[j];
			for(k = 0; k < n; k++) {
				uint16_t value = wobj->propword[j][k];
				if(value & 0x8000) resolve_rnum(value & 0x7fff);
			}
		}
	}
	resolve_rnum(((struct backend_pred *) find_builtin(prg, BI_PROGRAM_ENTRY)->pred->backend)->global_label);
	resolve_rnum(((struct backend_pred *) find_builtin(prg, BI_ERROR_ENTRY)->pred->backend)->global_label);
	resolve_rnum(R_FAIL_PRED);

#if 0
	printf("routines traced\n");
#endif

	addr_heap = 0x0040;
	addr_heapend = addr_heap + 2 * heapsize;
	assert(addr_heapend <= 0x7ffe);

	org = addr_heapend;

	addr_aux = org;
	org += auxsize * 2;

	addr_lts = org;
	org += ltssize * 2;

	addr_objtable = org;
	org += 63 * 2;

	for(i = 0; i < prg->nworldobj; i++) {
		wobj = &backendwobj[i];
		wobj->addr_objtable = org;
		org += 14;
	}

	for(i = 0; i < prg->nworldobj; i++) {
		wobj = &backendwobj[i];

		wobj->addr_proptable = org;
		org += 1 + wobj->n_encoded * 2;
		for(j = 63; j >= 1; j--) {
			if(wobj->npropword[j] == 1) {
				org += 1 + 2;
			} else if(wobj->npropword[j]) {
				org += 2 + 2 * wobj->npropword[j];
			}
		}
		org++;
	}

	addr_seltable = org;
	org += prg->nselect;

	for(i = 0; i < n_extflag; i++) {
		set_global_label(extflagarrays[i], org - 1);
		org += prg->nworldobj;
	}

	if(org & 1) org++;

	addr_globals = org;
	org += nglobal * 2;

	for(i = 0; i < prg->npredicate; i++) {
		predname = prg->predicates[i];
		pred = predname->pred;
		bp = pred->backend;
		if((pred->flags & PREDF_DYNAMIC)
		&& predname->builtin != BI_HASPARENT
		&& (predname->arity == 2 || (pred->flags & PREDF_DYN_LINKAGE))) {
			assert(bp->propbase_label);
			set_global_label(bp->propbase_label, org - 2); // -2 because it will be indexed by 1-based object id
			org += prg->nworldobj * 2;
		} else {
			if(bp->propbase_label) printf("%s\n", predname->printed_name);
			assert(!bp->propbase_label);
		}
	}

	addr_abbrevstr = org;
	org += 2;

	addr_abbrevtable = org;
	org += 96 * 2;

	addr_scratch = org;	// 12 bytes of scratch area for decoding dictionary words etc.
	org += 12;

	if(org < addr_globals + 2*240) {
		// Gargoyle complains if there isn't room for 240 globals in dynamic memory.
		org = addr_globals + 2*240;
	}

	addr_static = org;

	for(i = 0; i < nwordtable; i++) {
		set_global_label(wordtable[i].label, org);
		org += wordtable[i].length * 2;
	}

	for(i = 0; i < ndatatable; i++) {
		set_global_label(datatable[i].label, org);
		org += datatable[i].length;
	}

	addr_dictionary = org;
	org += 4 + NSTOPCHAR + ndict * 6;

	if(org > 0xfff8) {
		report(LVL_ERR, 0, "Base memory exhausted. Decrease heap/aux/long-term size using commandline options -H, -A, and/or -L.");
		exit(1);
	}

	org = (org + 7) & ~7;
	himem = org;
	entrypc = org + 1;

	set_global_label(G_HEAPBASE, addr_heap);
	set_global_label(G_HEAPEND, addr_heapend);
	set_global_label(G_HEAPSIZE, (addr_heapend - addr_heap) / 2);
	set_global_label(G_AUXBASE, addr_aux);
	set_global_label(G_AUXSIZE, auxsize);
	set_global_label(G_LTBASE, addr_lts);
	set_global_label(G_LTBASE2, addr_lts + 2);
	set_global_label(G_LTSIZE, ltssize);
	set_global_label(G_USER_GLOBALS, addr_globals + user_global_base * 2);
	set_global_label(G_TEMPSPACE_REGISTERS, addr_globals + (REG_TEMP - 0x10) * 2);
	set_global_label(G_ARG_REGISTERS, addr_globals + (REG_A - 0x10) * 2);
	set_global_label(G_DICT_TABLE, addr_dictionary + 4 + NSTOPCHAR);
	set_global_label(G_OBJECT_ID_END, prg->nworldobj);
	set_global_label(G_MAINSTYLE, 0x80);
	set_global_label(G_SELTABLE, addr_seltable);
	set_global_label(G_SCRATCH, addr_scratch);

	assert(REG_SPACE == REG_TEMP + 1);

#if 0
	printf("pass 1 begins\n");
#endif

	org = entrypc - 1;
	for(i = 0; i < next_routine_num; i++) {
		if(i == R_TERPTEST) continue; // put a directly-called routine last, to help txd
		if(routines[i]->actual_routine == routines[R_FAIL_PRED]->actual_routine) {
			routines[i]->address = 0;
		} else if(routines[i]->actual_routine == i) {
			assert((org & (packfactor - 1)) == 0);
			routines[i]->address = org / packfactor;
			org += pass1(routines[i], org);
			org = (org + packfactor - 1) & ~(packfactor - 1);
		}
	}

	assert((org & (packfactor - 1)) == 0);
	routines[R_TERPTEST]->address = org / packfactor;
	org += pass1(routines[R_TERPTEST], org);
	org = (org + packfactor - 1) & ~(packfactor - 1);

#if 0
	printf("pass 1 complete\n");
#endif

	set_global_label(
		G_PROGRAM_ENTRY,
		routines[resolve_rnum(((struct backend_pred *) find_builtin(prg, BI_PROGRAM_ENTRY)->pred->backend)->global_label)]->address);
	set_global_label(
		G_ERROR_ENTRY,
		routines[resolve_rnum(((struct backend_pred *) find_builtin(prg, BI_ERROR_ENTRY)->pred->backend)->global_label)]->address);

	for(i = 0; i < BUCKETS; i++) {
		for(gs = stringhash[i]; gs; gs = gs->next) {
			uint8_t pentets[MAXSTRING * 3];
			uint16_t words[MAXSTRING];
			int n;

			set_global_label(gs->global_label, org / packfactor);

			n = encode_chars(pentets, sizeof(pentets), 0, gs->zscii);
			assert(n <= sizeof(pentets));
			n = pack_pentets(words, pentets, n);

			org += n * 2;
			org = (org + packfactor - 1) & ~(packfactor - 1);
		}
	}

	filesize = org;
	if(filesize >= ((zversion == 5)? (1UL<<18) : (1UL<<19))) {
		report(LVL_ERR, 0, "Story too big for selected output format!");
		exit(1);
	}
	zcore = calloc(1, filesize);

	zcore[0x00] = zversion;
	zcore[0x02] = prg->meta_release >> 8;
	zcore[0x03] = prg->meta_release & 0xff;
	zcore[0x04] = himem >> 8;
	zcore[0x05] = himem & 0xff;
	zcore[0x06] = entrypc >> 8;
	zcore[0x07] = entrypc & 0xff;
	zcore[0x08] = addr_dictionary >> 8;
	zcore[0x09] = addr_dictionary & 0xff;
	zcore[0x0a] = addr_objtable >> 8;
	zcore[0x0b] = addr_objtable & 0xff;
	zcore[0x0c] = addr_globals >> 8;
	zcore[0x0d] = addr_globals & 0xff;
	zcore[0x0e] = addr_static >> 8;
	zcore[0x0f] = addr_static & 0xff;
	zcore[0x11] = 0x10;	// flags2: need undo
	for(i = 0; i < 6; i++) zcore[0x12 + i] = prg->meta_serial[i];
	zcore[0x18] = addr_abbrevtable >> 8;
	zcore[0x19] = addr_abbrevtable & 0xff;
	zcore[0x1a] = (filesize / packfactor) >> 8;
	zcore[0x1b] = (filesize / packfactor) & 0xff;
	//zcore[0x2e] = addr_termchar >> 8;
	//zcore[0x2f] = addr_termchar & 0xff;
	zcore[0x39] = 'D';
	zcore[0x3a] = 'i';
	zcore[0x3b] = 'a';
	zcore[0x3c] = VERSION[0];
	zcore[0x3d] = VERSION[1];
	assert('/' == VERSION[2]);
	zcore[0x3e] = VERSION[3];
	zcore[0x3f] = VERSION[4];

	init_abbrev(addr_abbrevstr, addr_abbrevtable);

	for(i = 1; i < 64; i++) {
		uint16_t addr = addr_objtable + (i - 1) * 2;
		uint16_t value = propdefault[i];
		if(value & 0x8000) value = routines[resolve_rnum(value & 0x7fff)]->address;
		zcore[addr++] = value >> 8;
		zcore[addr++] = value & 0xff;
	}

	memset(zcore + addr_lts, 0x3f, ltssize * 2);

	initial_values(prg, zcore, addr_globals, addr_lts, ltssize, extflagarrays, n_extflag);

	for(i = 0; i < prg->nworldobj; i++) {
		wobj = &backendwobj[i];
		zcore[wobj->addr_objtable + 12] = wobj->addr_proptable >> 8;
		zcore[wobj->addr_objtable + 13] = wobj->addr_proptable & 0xff;
		zcore[wobj->addr_proptable + 0] = wobj->n_encoded;
		for(j = 0; j < wobj->n_encoded; j++) {
			zcore[wobj->addr_proptable + 1 + j * 2 + 0] = wobj->encoded_name[j] >> 8;
			zcore[wobj->addr_proptable + 1 + j * 2 + 1] = wobj->encoded_name[j] & 0xff;
		}
		uint16_t addr = wobj->addr_proptable + 1 + 2 * wobj->n_encoded;
		for(j = 63; j > 0; j--) {
			int n = wobj->npropword[j];
			if(n) {
				if(n == 1) {
					zcore[addr++] = 0x40 | j;
				} else {
					zcore[addr++] = 0x80 | j;
					zcore[addr++] = 0x80 | ((2 * n) & 63);
				}
				for(k = 0; k < n; k++) {
					uint16_t value = wobj->propword[j][k];
					if(value & 0x8000) value = routines[resolve_rnum(value & 0x7fff)]->address;
					zcore[addr++] = value >> 8;
					zcore[addr++] = value & 0xff;
				}
			}
		}
	}

	for(i = 0; i < nwordtable; i++) {
		uint16_t addr = global_labels[wordtable[i].label];
		//printf("WT %d ", wordtable[i].length);
		for(j = 0; j < wordtable[i].length; j++) {
			uint16_t value = wordtable[i].words[j];
			if(value & 0x8000) value = global_labels[value & 0x7fff];
			zcore[addr++] = value >> 8;
			zcore[addr++] = value & 0xff;
			//printf("%04x", wordtable[i].words[j]);
		}
		//printf("\n");
	}

	for(i = 0; i < ndatatable; i++) {
		uint16_t addr = global_labels[datatable[i].label];
		//printf("DT %d ", datatable[i].length);
		for(j = 0; j < datatable[i].length; j++) {
			zcore[addr++] = datatable[i].data[j];
			//printf("%02x", datatable[i].data[j]);
		}
		//printf("\n");
	}

	zcore[addr_dictionary + 0] = NSTOPCHAR;
	for(i = 0; i < NSTOPCHAR; i++) {
		zcore[addr_dictionary + 1 + i] = STOPCHARS[i];
	}
	zcore[addr_dictionary + 1 + NSTOPCHAR + 0] = 6;
	zcore[addr_dictionary + 1 + NSTOPCHAR + 1] = ndict >> 8;
	zcore[addr_dictionary + 1 + NSTOPCHAR + 2] = ndict & 0xff;
	for(i = 0; i < ndict; i++) {
		for(j = 0; j < 3; j++) {
			zcore[addr_dictionary + 4 + NSTOPCHAR + i * 6 + j * 2 + 0] = dictionary[i].encoded[j] >> 8;
			zcore[addr_dictionary + 4 + NSTOPCHAR + i * 6 + j * 2 + 1] = dictionary[i].encoded[j] & 0xff;
		}
	}

	set_initial_reg(zcore + addr_globals, REG_NIL, 0x1fff);
	set_initial_reg(zcore + addr_globals, REG_3FFF, 0x3fff);
	set_initial_reg(zcore + addr_globals, REG_FFFF, 0xffff);
	set_initial_reg(zcore + addr_globals, REG_2000, 0x2000);
	set_initial_reg(zcore + addr_globals, REG_4000, 0x4000);
	set_initial_reg(zcore + addr_globals, REG_8000, 0x8000);
	set_initial_reg(zcore + addr_globals, REG_C000, 0xc000);
	set_initial_reg(zcore + addr_globals, REG_E000, 0xe000);

	memset(zcore + addr_heap, 0x1f, heapsize * 2);
	memset(zcore + addr_aux, 0x3f, auxsize * 2);

	if(prg->meta_ifid) {
		assert(strlen(prg->meta_ifid) == 36);
		memcpy(zcore + addr_heap, "UUID://", 7);
		memcpy(zcore + addr_heap + 7, prg->meta_ifid, 36);
		memcpy(zcore + addr_heap + 7 + 36, "//", 3);
	}

	for(i = 0; i < next_routine_num; i++) {
		if(routines[i]->actual_routine == routines[R_FAIL_PRED]->actual_routine) {
			/* skip */
		} else if(routines[i]->actual_routine == i) {
			uint32_t addr = routines[i]->address * packfactor;
			zcore[addr++] = routines[i]->nlocal;
			assemble(addr, routines[i]);
		}
	}

	for(i = 0; i < BUCKETS; i++) {
		for(gs = stringhash[i]; gs; gs = gs->next) {
			uint8_t pentets[MAXSTRING * 3];
			uint16_t words[MAXSTRING];
			int n;
			uint32_t addr = global_labels[gs->global_label] * packfactor;

			n = encode_chars(pentets, sizeof(pentets), 0, gs->zscii);
			assert(n <= sizeof(pentets));
			n = pack_pentets(words, pentets, n);

			for(j = 0; j < n; j++) {
				zcore[addr++] = words[j] >> 8;
				zcore[addr++] = words[j] & 0xff;
			}
		}
	}

	for(i = 0x40; i < filesize; i++) {
		checksum += zcore[i];
	}
	zcore[0x1c] = checksum >> 8;
	zcore[0x1d] = checksum & 0xff;

	report(LVL_DEBUG, 0, "Heap: %d words", heapsize);
	report(LVL_DEBUG, 0, "Auxiliary heap: %d words", auxsize);
	report(LVL_DEBUG, 0, "Long-term heap: %d words", ltssize);
	report(LVL_DEBUG, 0, "Global registers used: %d of 240", nglobal);
	report(LVL_DEBUG, 0, "Properties used: %d of 63", next_free_prop - 1);
	if(next_flag > NZOBJFLAG) {
		report(LVL_DEBUG, 0, "Flags used: %d native, %d extended", NZOBJFLAG, next_flag - NZOBJFLAG);
	} else {
		report(LVL_DEBUG, 0, "Flags used: %d native", next_flag);
	}

	if(!strcmp(format, "z8") || !strcmp(format, "z5")) {
		FILE *f = fopen(filename, "wb");
		if(!f) {
			report(LVL_ERR, 0, "Error opening \"%s\" for output: %s", filename, strerror(errno));
			exit(1);
		}

		if(1 != fwrite(zcore, filesize, 1, f)) {
			report(LVL_ERR, 0, "Error writing to \"%s\": %s", filename, strerror(errno));
			exit(1);
		}

		fclose(f);

		if(coverfname || coveralt) {
			report(LVL_WARN, 0, "Ignoring cover image options for the %s output format.", format);
		}
	} else if(!strcmp(format, "zblorb")) {
		if(!prg->meta_ifid) {
			report(LVL_ERR, 0, "An IFID declaration is mandatory for the blorb output format.");
			exit(1);
		}
		emit_blorb(
			filename,
			zcore,
			filesize,
			prg,
			zversion,
			coverfname,
			coveralt);
	} else {
		assert(0);
		exit(1);
	}

	if(tracing_enabled) {
		report(LVL_NOTE, 0, "In this build, the code has been instrumented to allow tracing.");
	}
}

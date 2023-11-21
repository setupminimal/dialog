#include <stdint.h>
#include <string.h>

#include "aavm.h"

struct aaopinfo aaopinfo[256];

static struct aaopinfo aaopinfosrc[] = {
	{AA_NOP,		{}, 0,							"nop"},
	{AA_FAIL,		{}, 0,							"fail"},
	{AA_SET_CONT,		{AAO_CODE}, 0,						"set_cont"},
	{AA_PROCEED,		{}, 0,							"proceed"},
	{AA_JMP,		{AAO_CODE}, 0,						"jmp"},
	{AA_JMP_MULTI,		{AAO_CODE}, 0,						"jmp_multi"},
	{AA_JMPL_MULTI,		{AAO_CODE}, 0,						"jmpl_multi"},
	{AA_JMP_SIMPLE,		{AAO_CODE}, 0,						"jmp_simple"},
	{AA_JMPL_SIMPLE,	{AAO_CODE}, 0,						"jmpl_simple"},
	{AA_JMP_TAIL,		{AAO_CODE}, 0,						"jmp_tail"},
	{AA_TAIL,		{}, 0,							"tail"},
	{AA_PUSH_ENV,		{AAO_BYTE}, AAO_ZERO,					"push_env"},
	{AA_POP_ENV,		{}, 0,							"pop_env"},
	{AA_POP_ENV_PROCEED,	{}, 0,							"pop_env_proceed"},
	{AA_PUSH_CHOICE,	{AAO_BYTE, AAO_CODE}, AAO_ZERO,				"push_choice"},
	{AA_POP_CHOICE,		{AAO_BYTE}, AAO_ZERO,					"pop_choice"},
	{AA_POP_PUSH_CHOICE,	{AAO_BYTE, AAO_CODE}, AAO_ZERO,				"pop_push_choice"},
	{AA_CUT_CHOICE,		{}, 0,							"cut_choice"},
	{AA_GET_CHO,		{AAO_DEST}, 0,						"get_cho"},
	{AA_SET_CHO,		{AAO_VALUE}, 0,						"set_cho"},
	{AA_ASSIGN,		{AAO_VALUE, AAO_DEST}, AAO_VBYTE,			"assign"},
	{AA_MAKE_VAR,		{AAO_DEST}, 0,						"make_var"},
	{AA_MAKE_PAIR_D,	{AAO_DEST, AAO_DEST, AAO_DEST}, 0,			"make_pair"},
	{AA_MAKE_PAIR_WB,	{AAO_WORD, AAO_DEST, AAO_DEST}, AAO_VBYTE,		"make_pair"},
	{AA_AUX_PUSH_VAL,	{AAO_VALUE}, 0,						"aux_push_val"},
	{AA_AUX_PUSH_RAW_0,	{AAO_ZERO}, 0,						"aux_push_raw"},
	{AA_AUX_PUSH_RAW,	{AAO_WORD}, AAO_VBYTE,					"aux_push_raw"},
	{AA_STOP,		{}, 0,							"stop"},
	{AA_PUSH_STOP,		{AAO_CODE}, 0,						"push_stop"},
	{AA_POP_STOP,		{}, 0,							"pop_stop"},
	{AA_SPLIT_WORD,		{AAO_VALUE, AAO_DEST}, 0,				"split_word"},
	{AA_JOIN_WORDS,		{AAO_VALUE, AAO_DEST}, 0,				"join_words"},
	{AA_AUX_POP_VAL,	{AAO_DEST}, 0,						"aux_pop_val"},
	{AA_AUX_POP_LIST,	{AAO_DEST}, 0,						"aux_pop_list"},
	{AA_AUX_POP_LIST_CHK,	{AAO_VALUE}, 0,						"aux_pop_list_chk"},
	{AA_AUX_POP_LIST_MATCH,	{AAO_VALUE}, 0,						"aux_pop_list_match"},
	{AA_SPLIT_LIST,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"split_list"},
	{AA_LOAD_WORD,		{AAO_VALUE, AAO_INDEX, AAO_DEST}, AAO_ZERO,		"load_word"},
	{AA_LOAD_BYTE,		{AAO_VALUE, AAO_INDEX, AAO_DEST}, AAO_ZERO,		"load_byte"},
	{AA_LOAD_VAL,		{AAO_VALUE, AAO_INDEX, AAO_DEST}, AAO_ZERO,		"load_val"},
	{AA_STORE_WORD,		{AAO_VALUE, AAO_INDEX, AAO_VALUE}, AAO_ZERO,		"store_word"},
	{AA_STORE_BYTE,		{AAO_VALUE, AAO_INDEX, AAO_VALUE}, AAO_ZERO,		"store_byte"},
	{AA_STORE_VAL,		{AAO_VALUE, AAO_INDEX, AAO_VALUE}, AAO_ZERO,		"store_val"},
	{AA_SET_FLAG,		{AAO_VALUE, AAO_INDEX}, AAO_ZERO,			"set_flag"},
	{AA_RESET_FLAG,		{AAO_VALUE, AAO_INDEX}, AAO_ZERO,			"reset_flag"},
	{AA_UNLINK,		{AAO_VALUE, AAO_INDEX, AAO_INDEX, AAO_VALUE}, AAO_ZERO,	"unlink"},
	{AA_SET_PARENT_V,	{AAO_VALUE, AAO_VALUE}, AAO_VBYTE,			"set_parent"},
	{AA_SET_PARENT_B,	{AAO_VALUE, AAO_VBYTE}, AAO_VBYTE,			"set_parent"},
	{AA_IF_RAW_EQ,		{AAO_WORD, AAO_VALUE, AAO_CODE}, AAO_ZERO,		"if_raw_eq"},
	{AA_IF_BOUND,		{AAO_VALUE, AAO_CODE}, 0,				"if_bound"},
	{AA_IF_EMPTY,		{AAO_VALUE, AAO_CODE}, 0,				"if_empty"},
	{AA_IF_NUM,		{AAO_VALUE, AAO_CODE}, 0,				"if_num"},
	{AA_IF_PAIR,		{AAO_VALUE, AAO_CODE}, 0,				"if_pair"},
	{AA_IF_OBJ,		{AAO_VALUE, AAO_CODE}, 0,				"if_obj"},
	{AA_IF_WORD,		{AAO_VALUE, AAO_CODE}, 0,				"if_word"},
	{AA_IF_UWORD,		{AAO_VALUE, AAO_CODE}, 0,				"if_uword"},
	{AA_IF_UNIFY,		{AAO_VALUE, AAO_VALUE, AAO_CODE}, 0,			"if_unify"},
	{AA_IF_GT,		{AAO_VALUE, AAO_VALUE, AAO_CODE}, 0,			"if_gt"},
	{AA_IF_EQ,		{AAO_WORD, AAO_VALUE, AAO_CODE}, AAO_VBYTE,		"if_eq"},
	{AA_IF_MEM_EQ_1,	{AAO_VALUE, AAO_INDEX, AAO_VALUE, AAO_CODE}, AAO_ZERO,	"if_mem_eq"},
	{AA_IF_FLAG,		{AAO_VALUE, AAO_INDEX, AAO_CODE}, AAO_ZERO,		"if_flag"},
	{AA_IF_CWL,		{AAO_CODE}, 0,						"if_cwl"},
	{AA_IF_MEM_EQ_2,	{AAO_VALUE, AAO_INDEX, AAO_VBYTE, AAO_CODE}, AAO_ZERO,	"if_mem_eq"},
	{AA_IFN_RAW_EQ,		{AAO_WORD, AAO_VALUE, AAO_CODE}, AAO_ZERO,		"ifn_raw_eq"},
	{AA_IFN_BOUND,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_bound"},
	{AA_IFN_EMPTY,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_empty"},
	{AA_IFN_NUM,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_num"},
	{AA_IFN_PAIR,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_pair"},
	{AA_IFN_OBJ,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_obj"},
	{AA_IFN_WORD,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_word"},
	{AA_IFN_UWORD,		{AAO_VALUE, AAO_CODE}, 0,				"ifn_uword"},
	{AA_IFN_UNIFY,		{AAO_VALUE, AAO_VALUE, AAO_CODE}, 0,			"ifn_unify"},
	{AA_IFN_GT,		{AAO_VALUE, AAO_VALUE, AAO_CODE}, 0,			"ifn_gt"},
	{AA_IFN_EQ,		{AAO_WORD, AAO_VALUE, AAO_CODE}, AAO_VBYTE,		"ifn_eq"},
	{AA_IFN_MEM_EQ_1,	{AAO_VALUE, AAO_INDEX, AAO_VALUE, AAO_CODE}, AAO_ZERO,	"ifn_mem_eq"},
	{AA_IFN_FLAG,		{AAO_VALUE, AAO_INDEX, AAO_CODE}, AAO_ZERO,		"ifn_flag"},
	{AA_IFN_CWL,		{AAO_CODE}, 0,						"ifn_cwl"},
	{AA_IFN_MEM_EQ_2,	{AAO_VALUE, AAO_INDEX, AAO_VBYTE, AAO_CODE}, AAO_ZERO,	"ifn_mem_eq"},
	{AA_ADD_RAW,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"add_raw"},
	{AA_INC_RAW,		{AAO_VALUE, AAO_DEST}, 0,				"inc_raw"},
	{AA_SUB_RAW,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"sub_raw"},
	{AA_DEC_RAW,		{AAO_VALUE, AAO_DEST}, 0,				"dec_raw"},
	{AA_RAND_RAW,		{AAO_BYTE, AAO_DEST}, 0,				"rand_raw"},
	{AA_ADD_NUM,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"add_num"},
	{AA_INC_NUM,		{AAO_VALUE, AAO_DEST}, 0,				"inc_num"},
	{AA_SUB_NUM,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"sub_num"},
	{AA_DEC_NUM,		{AAO_VALUE, AAO_DEST}, 0,				"dec_num"},
	{AA_RAND_NUM,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"rand_num"},
	{AA_MUL_NUM,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"mul_num"},
	{AA_DIV_NUM,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"div_num"},
	{AA_MOD_NUM,		{AAO_VALUE, AAO_VALUE, AAO_DEST}, 0,			"mod_num"},
	{AA_PRINT_A_STR_A,	{AAO_STRING}, 0,					"print_a_str_a"},
	{AA_PRINT_N_STR_A,	{AAO_STRING}, 0,					"print_n_str_a"},
	{AA_PRINT_A_STR_N,	{AAO_STRING}, 0,					"print_a_str_n"},
	{AA_PRINT_N_STR_N,	{AAO_STRING}, 0,					"print_n_str_n"},
	{AA_NOSPACE,		{}, 0,							"nospace"},
	{AA_SPACE,		{}, 0,							"space"},
	{AA_LINE,		{}, 0,							"line"},
	{AA_PAR,		{}, 0,							"par"},
	{AA_SPACE_N,		{AAO_VALUE}, 0,						"space_n"},
	{AA_PRINT_VAL,		{AAO_VALUE}, 0,						"print_val"},
	{AA_ENTER_DIV,		{AAO_INDEX}, 0,						"enter_div"},
	{AA_LEAVE_DIV,		{}, 0,							"leave_div"},
	{AA_ENTER_STATUS_0,	{AAO_ZERO, AAO_INDEX}, 0,				"enter_status"},
	{AA_LEAVE_STATUS,	{}, 0,							"leave_status"},
	{AA_ENTER_LINK_RES,	{AAO_VALUE}, 0,						"enter_link_res"},
	{AA_LEAVE_LINK_RES,	{}, 0,							"leave_link_res"},
	{AA_ENTER_LINK,		{AAO_VALUE}, 0,						"enter_link"},
	{AA_LEAVE_LINK,		{}, 0,							"leave_link"},
	{AA_ENTER_SELF_LINK,	{}, 0,							"enter_self_link"},
	{AA_LEAVE_SELF_LINK,	{}, 0,							"leave_self_link"},
	{AA_SET_STYLE,		{AAO_BYTE}, 0,						"set_style"},
	{AA_RESET_STYLE,	{AAO_BYTE}, 0,						"reset_style"},
	{AA_EMBED_RES,		{AAO_VALUE}, 0,						"embed_res"},
	{AA_CAN_EMBED_RES,	{AAO_VALUE, AAO_DEST}, 0,				"can_embed_res"},
	{AA_PROGRESS,		{AAO_VALUE, AAO_VALUE}, 0,				"progress"},
	{AA_ENTER_SPAN,		{AAO_INDEX}, 0,						"enter_span"},
	{AA_LEAVE_SPAN,		{}, 0,							"leave_span"},
	{AA_ENTER_STATUS,	{AAO_BYTE, AAO_INDEX}, 0,				"enter_status"},
	{AA_EXT0,		{AAO_BYTE}, 0,						"ext"},
	{AA_SAVE,		{AAO_CODE}, 0,						"save"},
	{AA_SAVE_UNDO,		{AAO_CODE}, 0,						"save_undo"},
	{AA_GET_INPUT,		{AAO_DEST}, 0,						"get_input"},
	{AA_GET_KEY,		{AAO_DEST}, 0,						"get_key"},
	{AA_VM_INFO,		{AAO_BYTE, AAO_DEST}, 0,				"vm_info"},
	{AA_SET_IDX,		{AAO_VALUE}, 0,						"set_idx"},
	{AA_CHECK_EQ,		{AAO_WORD, AAO_CODE}, AAO_VBYTE,			"check_eq"},
	{AA_CHECK_GT_EQ,	{AAO_WORD, AAO_CODE, AAO_CODE}, AAO_VBYTE,		"check_gt_eq"},
	{AA_CHECK_GT,		{AAO_VALUE, AAO_CODE}, AAO_BYTE,			"check_gt"},
	{AA_CHECK_WORDMAP,	{AAO_INDEX, AAO_CODE}, 0,				"check_wordmap"},
	{AA_CHECK_EQ_2A,	{AAO_WORD, AAO_WORD, AAO_CODE}, 0,			"check_eq_2"},
	{AA_CHECK_EQ_2B,	{AAO_VBYTE, AAO_VBYTE, AAO_CODE}, 0,			"check_eq_2"},
	{AA_TRACEPOINT,		{AAO_STRING, AAO_STRING, AAO_STRING, AAO_WORD}, 0,	"tracepoint"},
};

char *aaext0name[AAEXT0_N] = {
	"quit",
	"restart",
	"restore",
	"undo",
	"unstyle",
	"print_serial",
	"clear",
	"clear_all",
	"script_on",
	"script_off",
	"trace_on",
	"trace_off",
	"inc_cwl",
	"dec_cwl",
	"uppercase",
	"clear_links",
	"clear_old",
	"clear_div",
};

void aavm_init() {
	int i;

	for(i = 0; i < sizeof(aaopinfosrc) / sizeof(*aaopinfosrc); i++) {
		memcpy(
			aaopinfo + aaopinfosrc[i].op,
			aaopinfosrc + i,
			sizeof(struct aaopinfo));
		if(aaopinfosrc[i].alt_oper0) {
			memcpy(
				aaopinfo + (aaopinfosrc[i].op | 0x80),
				aaopinfosrc + i,
				sizeof(struct aaopinfo));
			aaopinfo[aaopinfosrc[i].op | 0x80].oper[0] = aaopinfosrc[i].alt_oper0;
		}
	}
}
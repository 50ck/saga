#pragma once
// Stable UTF-8 MD4C ABI subset. Prefer the upstream development header.
// ABI reference: https://github.com/mity/md4c/blob/release-0.4.8/src/md4c.h
extern "C" {
enum MD_BLOCKTYPE {MD_BLOCK_DOC,MD_BLOCK_QUOTE,MD_BLOCK_UL,MD_BLOCK_OL,MD_BLOCK_LI,MD_BLOCK_HR,MD_BLOCK_H,MD_BLOCK_CODE,MD_BLOCK_HTML,MD_BLOCK_P,MD_BLOCK_TABLE,MD_BLOCK_THEAD,MD_BLOCK_TBODY,MD_BLOCK_TR,MD_BLOCK_TH,MD_BLOCK_TD};
enum MD_SPANTYPE {MD_SPAN_EM,MD_SPAN_STRONG,MD_SPAN_A,MD_SPAN_IMG,MD_SPAN_CODE,MD_SPAN_DEL};
enum MD_TEXTTYPE {MD_TEXT_NORMAL,MD_TEXT_NULLCHAR,MD_TEXT_BR,MD_TEXT_SOFTBR,MD_TEXT_ENTITY,MD_TEXT_CODE,MD_TEXT_HTML};
enum MD_ALIGN {MD_ALIGN_DEFAULT,MD_ALIGN_LEFT,MD_ALIGN_CENTER,MD_ALIGN_RIGHT};
struct MD_ATTRIBUTE {const char* text; unsigned size; const MD_TEXTTYPE* substr_types; const unsigned* substr_offsets;};
struct MD_BLOCK_OL_DETAIL {unsigned start; int is_tight; char mark_delimiter;};
struct MD_BLOCK_LI_DETAIL {int is_task; char task_mark; unsigned task_mark_offset;};
struct MD_BLOCK_H_DETAIL {unsigned level;};
struct MD_BLOCK_CODE_DETAIL {MD_ATTRIBUTE info,lang; char fence_char;};
struct MD_BLOCK_TD_DETAIL {MD_ALIGN align;};
struct MD_SPAN_A_DETAIL {MD_ATTRIBUTE href,title;};
struct MD_SPAN_IMG_DETAIL {MD_ATTRIBUTE src,title;};
struct MD_PARSER {unsigned abi_version,flags; int (*enter_block)(MD_BLOCKTYPE,void*,void*); int (*leave_block)(MD_BLOCKTYPE,void*,void*); int (*enter_span)(MD_SPANTYPE,void*,void*); int (*leave_span)(MD_SPANTYPE,void*,void*); int (*text)(MD_TEXTTYPE,const char*,unsigned,void*); void (*debug_log)(const char*,void*); void (*syntax)();};
int md_parse(const char*,unsigned,const MD_PARSER*,void*);
}

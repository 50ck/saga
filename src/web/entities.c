#include <lexbor/core/sbst.h>
#include <lexbor/html/tokenizer/res.h>

/* Lexbor's generated entity table has C initializers. Keep that boundary here. */
const char *saga_web_named_entity(const char *name, size_t length, size_t *output_length) {
  const lexbor_sbst_entry_static_t *root = &lxb_html_tokenizer_res_entities_sbst[1];
  const lexbor_sbst_entry_static_t *match = NULL;
  for (size_t i = 0; i < length; ++i) {
    match = lexbor_sbst_entry_static_find(lxb_html_tokenizer_res_entities_sbst, root,
                                          (lxb_char_t)name[i]);
    if (match == NULL)
      return NULL;
    root = &lxb_html_tokenizer_res_entities_sbst[match->next];
  }
  if (match == NULL || match->value == NULL)
    return NULL;
  *output_length = match->value_len;
  return match->value;
}

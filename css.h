#ifndef M2T_CSS_H
#define M2T_CSS_H

#include <stdbool.h>
#include <stddef.h>
#include <libcss/libcss.h>
#include <lexbor/html/html.h>

typedef struct CssDocument CssDocument;

/* Owns selection results stored in node->user; destroy before the DOM. */
CssDocument *css_document_create(lxb_dom_node_t *root, const char *source,
                                 size_t columns, bool color);
void css_document_destroy(CssDocument *document);
const css_computed_style *css_node_style(lxb_dom_node_t *node,
                                        css_pseudo_element pseudo);
bool css_node_omitted(lxb_dom_node_t *node);

#endif

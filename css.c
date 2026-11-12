#define _XOPEN_SOURCE 700
#include "css.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <lexbor/ns/ns.h>
#include <lexbor/dom/interfaces/character_data.h>

typedef struct NodeStyle {
    lxb_dom_node_t *node;
    struct NodeStyle *next;
    css_select_results *results;
    void *cache;
    lwc_string **classes;
    uint32_t count;
} NodeStyle;

typedef struct Sheet {
    css_stylesheet *sheet;
    struct Sheet *next;
} Sheet;

struct CssDocument {
    css_select_ctx *selection;
    css_unit_ctx units;
    css_media media;
    NodeStyle *nodes;
    Sheet *sheets;
};

static css_select_handler handler;

static bool whitespace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static const unsigned char *attr(lxb_dom_node_t *n, const char *name, size_t *len)
{
    return lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                         (const lxb_char_t *)name, strlen(name), len);
}

static bool has_attr(lxb_dom_node_t *n, const char *name)
{
    return lxb_dom_element_has_attribute(lxb_dom_interface_element(n),
                                         (const lxb_char_t *)name, strlen(name));
}

static bool string_equal(const unsigned char *s, size_t n, lwc_string *value,
                          bool caseless)
{
    if (!s || n != lwc_string_length(value)) return false;
    return caseless ? !strncasecmp((const char *)s, lwc_string_data(value), n) :
                      !memcmp(s, lwc_string_data(value), n);
}

static bool token(const unsigned char *s, size_t n, lwc_string *value)
{
    for (size_t i = 0; i < n;) {
        while (i < n && whitespace(s[i])) i++;
        size_t start = i;
        while (i < n && !whitespace(s[i])) i++;
        if (i > start && string_equal(s + start, i - start, value, false)) return true;
    }
    return false;
}

static lxb_dom_node_t *element_parent(lxb_dom_node_t *n)
{
    n = n->parent;
    return n && n->type == LXB_DOM_NODE_TYPE_ELEMENT ? n : NULL;
}

static lxb_dom_node_t *previous_element(lxb_dom_node_t *n)
{
    while ((n = n->prev) && n->type != LXB_DOM_NODE_TYPE_ELEMENT) {}
    return n;
}

static bool namespace_matches(lxb_dom_node_t *n, lwc_string *ns)
{
    if (ns && lwc_string_length(ns) == 1 && *lwc_string_data(ns) == '*') return true;
    if (!ns) return n->ns == LXB_NS__UNDEF;
    size_t len;
    const unsigned char *uri = lxb_ns_by_id(n->owner_document->ns, n->ns, &len);
    return string_equal(uri, len, ns, false);
}

static css_error node_name(void *pw, void *node, css_qname *name)
{
    (void)pw;
    lxb_dom_node_t *n = node;
    size_t len;
    const unsigned char *s = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    name->name = name->ns = NULL;
    if (lwc_intern_string((const char *)s, len, &name->name) != lwc_error_ok)
        return CSS_NOMEM;
    s = lxb_ns_by_id(n->owner_document->ns, n->ns, &len);
    if (s && lwc_intern_string((const char *)s, len, &name->ns) != lwc_error_ok) {
        lwc_string_unref(name->name);
        name->name = NULL;
        return CSS_NOMEM;
    }
    return CSS_OK;
}

static css_error node_classes(void *pw, void *node, lwc_string ***classes,
                              uint32_t *count)
{
    (void)pw;
    NodeStyle *data = ((lxb_dom_node_t *)node)->user;
    *classes = data->classes;
    *count = data->count;
    for (uint32_t i = 0; i < data->count; i++) lwc_string_ref(data->classes[i]);
    return CSS_OK;
}

static css_error node_id(void *pw, void *node, lwc_string **id)
{
    (void)pw;
    size_t len;
    const unsigned char *s = attr(node, "id", &len);
    *id = NULL;
    if (s && lwc_intern_string((const char *)s, len, id) != lwc_error_ok) return CSS_NOMEM;
    return CSS_OK;
}

static css_error has_name(void *pw, void *node, const css_qname *name, bool *match)
{
    (void)pw;
    lxb_dom_node_t *n = node;
    size_t len;
    const unsigned char *s = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    *match = namespace_matches(n, name->ns) &&
        ((lwc_string_length(name->name) == 1 && *lwc_string_data(name->name) == '*') ||
         string_equal(s, len, name->name, n->ns == LXB_NS_HTML));
    return CSS_OK;
}

static css_error parent_node(void *pw, void *node, void **parent)
{
    (void)pw;
    *parent = element_parent(node);
    return CSS_OK;
}

static css_error sibling_node(void *pw, void *node, void **sibling)
{
    (void)pw;
    *sibling = previous_element(node);
    return CSS_OK;
}

static css_error named_relative(void *pw, void *node, const css_qname *name,
                                void **result, bool parent, bool all)
{
    lxb_dom_node_t *n = node;
    *result = NULL;
    do {
        n = parent ? element_parent(n) : previous_element(n);
        if (!n) break;
        bool match;
        has_name(pw, n, name, &match);
        if (match) { *result = n; break; }
    } while (all);
    return CSS_OK;
}

static css_error named_ancestor(void *pw, void *n, const css_qname *q, void **r)
{ return named_relative(pw, n, q, r, true, true); }
static css_error named_parent(void *pw, void *n, const css_qname *q, void **r)
{ return named_relative(pw, n, q, r, true, false); }
static css_error named_sibling(void *pw, void *n, const css_qname *q, void **r)
{ return named_relative(pw, n, q, r, false, false); }
static css_error named_generic_sibling(void *pw, void *n, const css_qname *q, void **r)
{ return named_relative(pw, n, q, r, false, true); }

static css_error has_class(void *pw, void *node, lwc_string *name, bool *match)
{
    (void)pw;
    size_t len;
    const unsigned char *s = attr(node, "class", &len);
    *match = s && token(s, len, name);
    return CSS_OK;
}

static css_error has_id(void *pw, void *node, lwc_string *name, bool *match)
{
    (void)pw;
    size_t len;
    const unsigned char *s = attr(node, "id", &len);
    *match = string_equal(s, len, name, false);
    return CSS_OK;
}

static lxb_dom_attr_t *find_attribute(lxb_dom_node_t *n, const css_qname *q)
{
    lxb_dom_element_t *element = lxb_dom_interface_element(n);
    for (lxb_dom_attr_t *a = element->first_attr; a; a = a->next) {
        size_t len;
        const unsigned char *s = lxb_dom_attr_local_name(a, &len);
        /* Lexbor labels ordinary HTML attributes with the HTML namespace;
         * CSS treats these unprefixed attributes as having no namespace. */
        bool unqualified = !q->ns && !a->node.prefix &&
            (a->node.ns == LXB_NS__UNDEF || a->node.ns == LXB_NS_HTML);
        if ((unqualified || namespace_matches(&a->node, q->ns)) &&
            string_equal(s, len, q->name, n->ns == LXB_NS_HTML)) return a;
    }
    return NULL;
}

static css_error has_attribute(void *pw, void *node, const css_qname *q, bool *match)
{
    (void)pw;
    *match = find_attribute(node, q) != NULL;
    return CSS_OK;
}

enum AttributeMatch { EQUAL, DASH, INCLUDES, PREFIX, SUFFIX, SUBSTRING };

static css_error match_attribute(void *node, const css_qname *q, lwc_string *value,
                                 bool *match, enum AttributeMatch op)
{
    *match = false;
    lxb_dom_attr_t *a = find_attribute(node, q);
    if (!a) return CSS_OK;
    size_t len;
    const unsigned char *s = lxb_dom_attr_value(a, &len);
    if (!s) { s = (const unsigned char *)""; len = 0; }
    size_t n = lwc_string_length(value);
    const char *v = lwc_string_data(value);
    if (op == EQUAL) *match = string_equal(s, len, value, false);
    else if (!n) return CSS_OK;
    else if (op == INCLUDES) *match = token(s, len, value);
    else if (len >= n) {
        if (op == DASH) *match = !memcmp(s, v, n) && (len == n || s[n] == '-');
        if (op == PREFIX) *match = !memcmp(s, v, n);
        if (op == SUFFIX) *match = !memcmp(s + len - n, v, n);
        if (op == SUBSTRING)
            for (size_t i = 0; i <= len - n; i++)
                if (!memcmp(s + i, v, n)) { *match = true; break; }
    }
    return CSS_OK;
}

#define ATTRIBUTE_HANDLER(name, operation) \
static css_error name(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) \
{ (void)pw; return match_attribute(n, q, v, m, operation); }
ATTRIBUTE_HANDLER(attribute_equal, EQUAL)
ATTRIBUTE_HANDLER(attribute_dash, DASH)
ATTRIBUTE_HANDLER(attribute_includes, INCLUDES)
ATTRIBUTE_HANDLER(attribute_prefix, PREFIX)
ATTRIBUTE_HANDLER(attribute_suffix, SUFFIX)
ATTRIBUTE_HANDLER(attribute_substring, SUBSTRING)
#undef ATTRIBUTE_HANDLER

static css_error is_root(void *pw, void *node, bool *match)
{ (void)pw; *match = !element_parent(node); return CSS_OK; }

static css_error count_siblings(void *pw, void *node, bool same, bool after, int32_t *count)
{
    (void)pw;
    lxb_dom_node_t *origin = node;
    *count = 0;
    for (lxb_dom_node_t *n = after ? origin->next : origin->prev; n;
         n = after ? n->next : n->prev)
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            (!same || (n->ns == origin->ns && n->local_name == origin->local_name)) &&
            *count < INT32_MAX) (*count)++;
    return CSS_OK;
}

static css_error is_empty(void *pw, void *node, bool *match)
{
    (void)pw;
    *match = true;
    for (lxb_dom_node_t *n = ((lxb_dom_node_t *)node)->first_child; n; n = n->next)
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT ||
            (n->type == LXB_DOM_NODE_TYPE_TEXT &&
             lxb_dom_interface_character_data(n)->data.length)) { *match = false; break; }
    return CSS_OK;
}

static css_error is_link(void *pw, void *node, bool *match)
{
    (void)pw;
    lxb_dom_node_t *n = node;
    *match = (n->local_name == LXB_TAG_A || n->local_name == LXB_TAG_AREA ||
              n->local_name == LXB_TAG_LINK) && has_attr(n, "href");
    return CSS_OK;
}

static css_error inactive(void *pw, void *node, bool *match)
{ (void)pw; (void)node; *match = false; return CSS_OK; }

static bool control(lxb_dom_node_t *n)
{
    return n->local_name == LXB_TAG_INPUT || n->local_name == LXB_TAG_BUTTON ||
           n->local_name == LXB_TAG_SELECT || n->local_name == LXB_TAG_TEXTAREA ||
           n->local_name == LXB_TAG_OPTION || n->local_name == LXB_TAG_OPTGROUP ||
           n->local_name == LXB_TAG_FIELDSET;
}

static css_error is_disabled(void *pw, void *node, bool *match)
{
    (void)pw;
    lxb_dom_node_t *n = node;
    *match = control(n) && has_attr(n, "disabled");
    if (!control(n)) return CSS_OK;
    for (lxb_dom_node_t *p = element_parent(n); p; p = element_parent(p)) {
        if (p->local_name == LXB_TAG_OPTGROUP && n->local_name == LXB_TAG_OPTION &&
            has_attr(p, "disabled")) *match = true;
        if (p->local_name == LXB_TAG_FIELDSET && has_attr(p, "disabled")) {
            lxb_dom_node_t *legend = p->first_child;
            while (legend && (legend->type != LXB_DOM_NODE_TYPE_ELEMENT ||
                              legend->local_name != LXB_TAG_LEGEND)) legend = legend->next;
            lxb_dom_node_t *child = n;
            while (child && child != legend && child != p) child = child->parent;
            if (child != legend) *match = true;
        }
    }
    return CSS_OK;
}

static css_error is_enabled(void *pw, void *node, bool *match)
{
    is_disabled(pw, node, match);
    *match = control(node) && !*match;
    return CSS_OK;
}

static css_error is_checked(void *pw, void *node, bool *match)
{
    (void)pw;
    lxb_dom_node_t *n = node;
    *match = (n->local_name == LXB_TAG_INPUT && has_attr(n, "checked")) ||
             (n->local_name == LXB_TAG_OPTION && has_attr(n, "selected"));
    return CSS_OK;
}

static css_error is_lang(void *pw, void *node, lwc_string *lang, bool *match)
{
    (void)pw;
    *match = false;
    for (lxb_dom_node_t *n = node; n; n = element_parent(n)) {
        size_t len;
        const unsigned char *s = attr(n, "lang", &len);
        if (s) {
            size_t k = lwc_string_length(lang);
            *match = len >= k && !strncasecmp((const char *)s, lwc_string_data(lang), k) &&
                     (len == k || s[k] == '-');
            break;
        }
    }
    return CSS_OK;
}

static css_error hints(void *pw, void *node, uint32_t *count, css_hint **result)
{ (void)pw; (void)node; *count = 0; *result = NULL; return CSS_OK; }

static css_error defaults(void *pw, uint32_t property, css_hint *hint)
{
    (void)pw;
    memset(hint, 0, sizeof(*hint));
    switch (property) {
    case CSS_PROP_COLOR:
        /* Transparent is the terminal's unspecified default foreground. */
        hint->status = CSS_COLOR_COLOR;
        hint->data.color = 0;
        break;
    case CSS_PROP_FONT_FAMILY: hint->status = CSS_FONT_FAMILY_MONOSPACE; break;
    case CSS_PROP_QUOTES: hint->status = CSS_QUOTES_NONE; break;
    case CSS_PROP_VOICE_FAMILY: break;
    default: return CSS_INVALID;
    }
    return CSS_OK;
}

static css_error set_cache(void *pw, void *node, void *cache)
{
    (void)pw;
    ((NodeStyle *)((lxb_dom_node_t *)node)->user)->cache = cache;
    return CSS_OK;
}

static css_error get_cache(void *pw, void *node, void **cache)
{
    (void)pw;
    *cache = ((NodeStyle *)((lxb_dom_node_t *)node)->user)->cache;
    return CSS_OK;
}

static css_select_handler handler = {
    .handler_version = CSS_SELECT_HANDLER_VERSION_1,
    .node_name = node_name, .node_classes = node_classes, .node_id = node_id,
    .named_ancestor_node = named_ancestor, .named_parent_node = named_parent,
    .named_sibling_node = named_sibling, .named_generic_sibling_node = named_generic_sibling,
    .parent_node = parent_node, .sibling_node = sibling_node,
    .node_has_name = has_name, .node_has_class = has_class, .node_has_id = has_id,
    .node_has_attribute = has_attribute, .node_has_attribute_equal = attribute_equal,
    .node_has_attribute_dashmatch = attribute_dash,
    .node_has_attribute_includes = attribute_includes,
    .node_has_attribute_prefix = attribute_prefix, .node_has_attribute_suffix = attribute_suffix,
    .node_has_attribute_substring = attribute_substring,
    .node_is_root = is_root, .node_count_siblings = count_siblings, .node_is_empty = is_empty,
    .node_is_link = is_link, .node_is_visited = inactive, .node_is_hover = inactive,
    .node_is_active = inactive, .node_is_focus = inactive,
    .node_is_enabled = is_enabled, .node_is_disabled = is_disabled, .node_is_checked = is_checked,
    .node_is_target = inactive, .node_is_lang = is_lang,
    .node_presentational_hint = hints, .ua_default_for_property = defaults,
    .set_libcss_node_data = set_cache, .get_libcss_node_data = get_cache
};

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **result)
{
    (void)pw;
    const char *s = lwc_string_data(rel);
    size_t n = lwc_string_length(rel);
    if (!n || s[0] == '/' || memchr(s, ':', n)) {
        *result = lwc_string_ref(rel);
        return CSS_OK;
    }
    const char *slash = strrchr(base, '/');
    size_t prefix = slash ? (size_t)(slash - base) + 1 : 0;
    if (prefix > SIZE_MAX - n - 1) return CSS_NOMEM;
    char *path = malloc(prefix + n + 1);
    if (!path) return CSS_NOMEM;
    memcpy(path, base, prefix);
    memcpy(path + prefix, s, n);
    css_error error = lwc_intern_string(path, prefix + n, result) == lwc_error_ok ?
                      CSS_OK : CSS_NOMEM;
    free(path);
    return error;
}

static char *read_local(const char *path, size_t *len)
{
    *len = 0;
    if (strchr(path, ':') || !strncmp(path, "//", 2)) return NULL;
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    char *data = NULL;
    size_t cap = 0;
    for (;;) {
        if (*len == cap) {
            size_t next = cap ? cap * 2 : 4096;
            if (next < cap) { free(data); fclose(in); return NULL; }
            char *p = realloc(data, next);
            if (!p) { free(data); fclose(in); return NULL; }
            data = p;
            cap = next;
        }
        size_t n = fread(data + *len, 1, cap - *len, in);
        *len += n;
        if (!n) break;
    }
    if (ferror(in)) { free(data); data = NULL; *len = 0; }
    fclose(in);
    return data;
}

static css_stylesheet *parse_sheet(CssDocument *doc, const char *data, size_t len,
                                   const char *url, bool inline_style, unsigned depth)
{
    Sheet *owned = calloc(1, sizeof(*owned));
    if (!owned) return NULL;
    css_stylesheet_params params = {
        .params_version = CSS_STYLESHEET_PARAMS_VERSION_1, .level = CSS_LEVEL_3,
        .charset = "UTF-8", .url = url, .inline_style = inline_style, .resolve = resolve_url
    };
    if (css_stylesheet_create(&params, &owned->sheet) != CSS_OK) { free(owned); return NULL; }
    owned->next = doc->sheets;
    doc->sheets = owned;
    css_error error = css_stylesheet_append_data(owned->sheet, (const uint8_t *)data, len);
    if (error != CSS_OK && error != CSS_NEEDDATA) return NULL;
    error = css_stylesheet_data_done(owned->sheet);
    if (error != CSS_OK && error != CSS_IMPORTS_PENDING) return NULL;
    lwc_string *import_url;
    while (css_stylesheet_next_pending_import(owned->sheet, &import_url) == CSS_OK) {
        char *path = strndup(lwc_string_data(import_url), lwc_string_length(import_url));
        lwc_string_unref(import_url);
        if (!path) return NULL;
        size_t size = 0;
        char *source = depth < 16 ? read_local(path, &size) : NULL;
        css_stylesheet *child = parse_sheet(doc, source ? source : "", size, path, false, depth + 1);
        free(source);
        free(path);
        if (!child || css_stylesheet_register_import(owned->sheet, child) != CSS_OK) return NULL;
    }
    return owned->sheet;
}

static lxb_dom_node_t *next_node(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    if (n->first_child) return n->first_child;
    while (n != root && !n->next) n = n->parent;
    return n == root ? NULL : n->next;
}

static bool prepare_node(CssDocument *doc, lxb_dom_node_t *n)
{
    NodeStyle *data = calloc(1, sizeof(*data));
    if (!data) return false;
    n->user = data;
    data->node = n;
    data->next = doc->nodes;
    doc->nodes = data;
    size_t len;
    const unsigned char *s = attr(n, "class", &len);
    if (!s) return true;
    for (size_t i = 0; i < len;) {
        while (i < len && whitespace(s[i])) i++;
        size_t start = i;
        while (i < len && !whitespace(s[i])) i++;
        if (i == start) break;
        if (data->count == UINT32_MAX) return false;
        lwc_string **p = realloc(data->classes, ((size_t)data->count + 1) * sizeof(*p));
        if (!p) return false;
        data->classes = p;
        if (lwc_intern_string((const char *)s + start, i - start,
                               &data->classes[data->count]) != lwc_error_ok) return false;
        data->count++;
    }
    return true;
}

static bool add_document_sheet(CssDocument *doc, lxb_dom_node_t *n, const char *base)
{
    const unsigned char *source = NULL;
    size_t len = 0;
    unsigned char *allocated = NULL;
    char *path = NULL;
    size_t type_len;
    const unsigned char *type = attr(n, "type", &type_len);
    if (type && type_len &&
        (type_len != 8 || strncasecmp((const char *)type, "text/css", 8))) return true;
    if (n->local_name == LXB_TAG_STYLE) {
        allocated = lxb_dom_node_text_content(n, &len);
        source = allocated;
    } else if (n->local_name == LXB_TAG_LINK) {
        size_t nrel;
        const unsigned char *rel = attr(n, "rel", &nrel);
        lwc_string *stylesheet;
        if (lwc_intern_string("stylesheet", 10, &stylesheet) != lwc_error_ok) return false;
        bool is_sheet = false;
        for (size_t i = 0; rel && i < nrel;) {
            while (i < nrel && whitespace(rel[i])) i++;
            size_t start = i;
            while (i < nrel && !whitespace(rel[i])) i++;
            if (string_equal(rel + start, i - start, stylesheet, true)) is_sheet = true;
        }
        lwc_string_unref(stylesheet);
        if (!is_sheet || has_attr(n, "disabled")) return true;
        size_t href_len;
        const unsigned char *href = attr(n, "href", &href_len);
        if (!href) return true;
        lwc_string *relative, *absolute;
        if (lwc_intern_string((const char *)href, href_len, &relative) != lwc_error_ok) return false;
        css_error error = resolve_url(NULL, base, relative, &absolute);
        lwc_string_unref(relative);
        if (error != CSS_OK) return false;
        path = strndup(lwc_string_data(absolute), lwc_string_length(absolute));
        lwc_string_unref(absolute);
        if (!path) return false;
        source = (unsigned char *)read_local(path, &len);
    } else return true;
    css_stylesheet *sheet = parse_sheet(doc, source ? (const char *)source : "", len,
                                        path ? path : base, false, 0);
    if (allocated) lxb_dom_document_destroy_text(n->owner_document, allocated);
    else free((void *)source);
    free(path);
    size_t media_len;
    const unsigned char *media = attr(n, "media", &media_len);
    char *query = media ? strndup((const char *)media, media_len) : NULL;
    if (media && !query) return false;
    bool ok = sheet && css_select_ctx_append_sheet(doc->selection, sheet,
                                                    CSS_ORIGIN_AUTHOR, query) == CSS_OK;
    free(query);
    return ok;
}

CssDocument *css_document_create(lxb_dom_node_t *root, const char *source,
                                 size_t columns, bool color)
{
    static const char ua[] =
        "strong,b,th,h1,h2,h3,h4,h5,h6{font-weight:bold}"
        "em,i,cite,var{font-style:italic}"
        "u,ins,a{ text-decoration:underline }del,s,strike{text-decoration:line-through}"
        "h1,h2,h3,h4,h5,h6{color:blue}a{color:cyan}"
        "code,pre,kbd,samp{color:#ffff00}mark{color:black;background-color:yellow}"
        "pre,code,kbd,samp{white-space:pre}"
        "ul{list-style-type:disc}ol{list-style-type:decimal}"
        "[hidden],head,script,style,template,title{display:none}"
        "p,div,section,article,header,footer,main,aside,nav,address,figure,figcaption,"
        "details,summary,dl,dt,dd,h1,h2,h3,h4,h5,h6,blockquote,pre,ul,ol{display:block}"
        "li{display:list-item}table{display:table}tr{display:table-row}"
        "td,th{display:table-cell}";
    CssDocument *doc = calloc(1, sizeof(*doc));
    if (!doc) return NULL;
    size_t width = columns ? columns : 80;
    if (width > 10000) width = 10000;
    doc->units.viewport_width = INTTOFIX((int)width * 8);
    doc->units.viewport_height = INTTOFIX(384);
    doc->units.font_size_default = INTTOFIX(16);
    doc->units.device_dpi = INTTOFIX(96);
    doc->media = (css_media){
        .type = CSS_MEDIA_SCREEN, .width = doc->units.viewport_width,
        .height = doc->units.viewport_height, .grid = INTTOFIX(1),
        .aspect_ratio = FDIV(doc->units.viewport_width, doc->units.viewport_height),
        .orientation = doc->units.viewport_width >= doc->units.viewport_height ?
                       CSS_MEDIA_ORIENTATION_LANDSCAPE : CSS_MEDIA_ORIENTATION_PORTRAIT,
        .color = color ? INTTOFIX(8) : 0, .monochrome = color ? 0 : INTTOFIX(1),
        .scripting = CSS_MEDIA_SCRIPTING_NONE
    };
    if (css_select_ctx_create(&doc->selection) != CSS_OK) goto fail;
    css_stylesheet *sheet = parse_sheet(doc, ua, sizeof(ua) - 1, "m2t:defaults", false, 0);
    if (!sheet || css_select_ctx_append_sheet(doc->selection, sheet, CSS_ORIGIN_UA, NULL) != CSS_OK)
        goto fail;
    const char *base = source && strcmp(source, "-") ? source : "./stdin";
    for (lxb_dom_node_t *n = root; n; n = next_node(n, root))
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (!prepare_node(doc, n) || !add_document_sheet(doc, n, base)) goto fail;
        }
    /* Select in document order so parents and sibling caches are available. */
    for (lxb_dom_node_t *n = root; n; n = next_node(n, root)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        NodeStyle *data = n->user;
        size_t len;
        const unsigned char *inline_css = attr(n, "style", &len);
        css_stylesheet *inline_sheet = inline_css ?
            parse_sheet(doc, (const char *)inline_css, len, base, true, 0) : NULL;
        if (inline_css && !inline_sheet) goto fail;
        if (css_select_style(doc->selection, n, &doc->units, &doc->media, inline_sheet,
                              &handler, doc, &data->results) != CSS_OK) goto fail;
        const css_computed_style *parent = css_node_style(element_parent(n), CSS_PSEUDO_ELEMENT_NONE);
        css_computed_style **computed = &data->results->styles[CSS_PSEUDO_ELEMENT_NONE];
        if (parent) {
            css_computed_style *composed;
            if (css_computed_style_compose(parent, *computed, &doc->units, &composed) != CSS_OK) goto fail;
            css_computed_style_destroy(*computed);
            *computed = composed;
        } else doc->units.root_style = *computed;
        for (unsigned p = CSS_PSEUDO_ELEMENT_FIRST_LINE; p < CSS_PSEUDO_ELEMENT_COUNT; p++) {
            css_computed_style **pseudo = &data->results->styles[p];
            if (!*pseudo) continue;
            css_computed_style *composed;
            if (css_computed_style_compose(*computed, *pseudo, &doc->units, &composed) != CSS_OK) goto fail;
            css_computed_style_destroy(*pseudo);
            *pseudo = composed;
        }
    }
    return doc;
fail:
    css_document_destroy(doc);
    return NULL;
}

const css_computed_style *css_node_style(lxb_dom_node_t *node, css_pseudo_element pseudo)
{
    NodeStyle *data = node ? node->user : NULL;
    return data && data->results ? data->results->styles[pseudo] : NULL;
}

bool css_node_omitted(lxb_dom_node_t *node)
{
    const css_computed_style *style = css_node_style(node, CSS_PSEUDO_ELEMENT_NONE);
    return style && css_computed_display_static(style) == CSS_DISPLAY_NONE;
}

void css_document_destroy(CssDocument *doc)
{
    if (!doc) return;
    for (NodeStyle *data = doc->nodes; data;) {
        NodeStyle *next = data->next;
        if (data->cache)
            css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, doc,
                                         data->node, NULL, data->cache);
        if (data->results) css_select_results_destroy(data->results);
        for (uint32_t i = 0; i < data->count; i++) lwc_string_unref(data->classes[i]);
        free(data->classes);
        data->node->user = NULL;
        free(data);
        data = next;
    }
    if (doc->selection) css_select_ctx_destroy(doc->selection);
    for (Sheet *owned = doc->sheets; owned;) {
        Sheet *next = owned->next;
        css_stylesheet_destroy(owned->sheet);
        free(owned);
        owned = next;
    }
    free(doc);
}

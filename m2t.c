#define _XOPEN_SOURCE 700
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

#include <md4c-html.h>
#include <lexbor/html/html.h>
#include <lexbor/dom/interfaces/character_data.h>

typedef struct {
    char *data;
    size_t len, cap;
    bool failed;
} Buffer;

static bool reserve(void **data, size_t *cap, size_t need, size_t unit)
{
    if (need <= *cap) return true;
    size_t n = *cap ? *cap : 64;
    while (n < need) {
        if (n > SIZE_MAX / 2) { n = need; break; }
        n *= 2;
    }
    if (n > SIZE_MAX / unit) return false;
    void *p = realloc(*data, n * unit);
    if (!p) return false;
    *data = p;
    *cap = n;
    return true;
}

static void append(const MD_CHAR *s, MD_SIZE n, void *userdata)
{
    Buffer *b = userdata;
    if (b->failed) return;
    if (n > SIZE_MAX - b->len - 1 ||
        !reserve((void **)&b->data, &b->cap, b->len + n + 1, 1)) {
        b->failed = true;
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

enum { BOLD = 1, ITALIC = 2, UNDERLINE = 4, STRIKE = 8, CODE = 16,
       LINK = 32, HEADING = 64, MARK = 128 };

typedef struct {
    unsigned style;
    size_t indent, quotes;
    bool pre, verbatim;
} Context;

typedef struct {
    Context ctx;
    FILE *out;
    bool ansi, pending_space, has_output, cell_mode, hard_wrap, failed;
    unsigned active_style;
    size_t col, width, newlines;
} Renderer;

static void style(Renderer *r, unsigned bits)
{
    if (!r->ansi || r->active_style == bits) return;
    fputs("\033[0m", r->out);
    if (bits & BOLD) fputs("\033[1m", r->out);
    if (bits & ITALIC) fputs("\033[3m", r->out);
    if (bits & UNDERLINE) fputs("\033[4m", r->out);
    if (bits & STRIKE) fputs("\033[9m", r->out);
    if (bits & CODE) fputs("\033[33m", r->out);
    if (bits & LINK) fputs("\033[36m", r->out);
    if (bits & HEADING) fputs("\033[34m", r->out);
    if (bits & MARK) fputs("\033[30;43m", r->out);
    r->active_style = bits;
}

static void newline(Renderer *r)
{
    style(r, 0);
    fputc('\n', r->out);
    r->col = 0;
    r->newlines++;
    r->pending_space = false;
}

static void boundary(Renderer *r, size_t lines)
{
    r->pending_space = false;
    if (r->has_output) while (r->newlines < lines) newline(r);
}

static void prefix(Renderer *r)
{
    if (r->col) return;
    style(r, 0);
    for (size_t i = 0; i < r->ctx.quotes; i++) fputs("│ ", r->out);
    for (size_t i = 0; i < r->ctx.indent; i++) fputc(' ', r->out);
    r->col = r->ctx.quotes * 2 + r->ctx.indent;
}

/* Decode UTF-8 independently of the process locale; never pass terminal
 * control bytes (including encoded C1 controls) through from the document. */
static size_t decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    unsigned c = s[0];
    size_t count;
    uint32_t value;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xc2 && c <= 0xdf) { count = 2; value = c & 31; }
    else if (c >= 0xe0 && c <= 0xef) { count = 3; value = c & 15; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 4; value = c & 7; }
    else { *cp = 0xfffd; return 1; }
    if (count > n) { *cp = 0xfffd; return 1; }
    for (size_t i = 1; i < count; i++) {
        if ((s[i] & 0xc0) != 0x80) { *cp = 0xfffd; return 1; }
        value = (value << 6) | (s[i] & 63);
    }
    if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
        *cp = 0xfffd;
        return 1;
    }
    *cp = value;
    return count;
}

static size_t char_width(uint32_t cp)
{
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : (size_t)w;
}

static bool space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void glyph(Renderer *r, const unsigned char *s, size_t n, uint32_t cp)
{
    if (r->hard_wrap && r->width &&
        r->col > r->ctx.indent + r->ctx.quotes * 2 &&
        r->col + char_width(cp) > r->width) newline(r);
    prefix(r);
    style(r, r->ctx.style);
    if (cp < 32 || (cp >= 0x7f && cp <= 0x9f) || cp == 0xfffd)
        fputs("�", r->out);
    else
        fwrite(s, 1, n, r->out);
    r->col += char_width(cp);
    r->newlines = 0;
    r->has_output = true;
}

static void text(Renderer *r, const unsigned char *s, size_t n)
{
    if (r->ctx.verbatim && !r->ctx.pre && r->pending_space) {
        if (r->col) glyph(r, (const unsigned char *)" ", 1, ' ');
        r->pending_space = false;
    }
    for (size_t i = 0; i < n;) {
        if (r->ctx.pre || r->ctx.verbatim) {
            if (s[i] == '\n') { newline(r); i++; continue; }
            if (s[i] == '\t') {
                prefix(r);
                size_t count = 4 - r->col % 4;
                while (count--) glyph(r, (const unsigned char *)" ", 1, ' ');
                i++;
                continue;
            }
        } else {
            if (space(s[i])) { r->pending_space = true; i++; continue; }
            /* Wrap at whitespace, retaining long words and URLs intact. */
            size_t word_width = 0;
            for (size_t j = i; j < n && !space(s[j]);) {
                uint32_t cp;
                size_t k = decode(s + j, n - j, &cp);
                word_width += char_width(cp);
                j += k;
            }
            if (r->pending_space && r->col) {
                if (r->width && r->col + 1 + word_width > r->width) newline(r);
                else glyph(r, (const unsigned char *)" ", 1, ' ');
            }
            r->pending_space = false;
            /* Consume a whole word here, avoiding quadratic rescans. */
            while (i < n && !space(s[i])) {
                uint32_t cp;
                size_t k = decode(s + i, n - i, &cp);
                glyph(r, s + i, k, cp);
                i += k;
            }
            continue;
        }
        uint32_t cp;
        size_t k = decode(s + i, n - i, &cp);
        glyph(r, s + i, k, cp);
        i += k;
    }
}

static void literal(Renderer *r, const char *s)
{
    text(r, (const unsigned char *)s, strlen(s));
}

static const unsigned char *attribute(lxb_dom_node_t *node, const char *name,
                                     size_t *len)
{
    return lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                        (const lxb_char_t *)name, strlen(name), len);
}

static bool has_attribute(lxb_dom_node_t *node, const char *name)
{
    return lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                                         (const lxb_char_t *)name, strlen(name));
}

static long number_attr(lxb_dom_node_t *node, const char *name, long fallback)
{
    size_t n;
    const unsigned char *s = attribute(node, name, &n);
    char buf[32], *end;
    if (!s || n == 0 || n >= sizeof(buf)) return fallback;
    memcpy(buf, s, n);
    buf[n] = '\0';
    errno = 0;
    long v = strtol(buf, &end, 10);
    return errno || end == buf || *end ? fallback : v;
}

static bool block(uintptr_t tag)
{
    switch (tag) {
    case LXB_TAG_P: case LXB_TAG_DIV: case LXB_TAG_SECTION:
    case LXB_TAG_ARTICLE: case LXB_TAG_HEADER: case LXB_TAG_FOOTER:
    case LXB_TAG_MAIN: case LXB_TAG_ASIDE: case LXB_TAG_NAV:
    case LXB_TAG_ADDRESS: case LXB_TAG_FIGURE: case LXB_TAG_FIGCAPTION:
    case LXB_TAG_DETAILS: case LXB_TAG_SUMMARY: case LXB_TAG_DL:
    case LXB_TAG_DT: case LXB_TAG_DD: case LXB_TAG_TABLE:
        return true;
    default: return false;
    }
}

static bool render(Renderer *r, lxb_dom_node_t *root);

typedef struct {
    lxb_dom_node_t *node;
    char *data;
    size_t len, pos;
    unsigned align; /* 0: left, 1: center, 2: right */
} Cell;

typedef struct {
    Cell *cells;
    size_t count, cap;
} Row;

/* Cell buffers contain only sanitized UTF-8 and SGR emitted by this renderer. */
static size_t display_width(const char *s, size_t n)
{
    size_t width = 0, longest = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '\033') {
            while (i < n && s[i++] != 'm') {}
        } else if (s[i] == '\n') {
            if (width > longest) longest = width;
            width = 0;
            i++;
        } else {
            uint32_t cp;
            i += decode((const unsigned char *)s + i, n - i, &cp);
            width += char_width(cp);
        }
    }
    return width > longest ? width : longest;
}

static bool capture_cell(Renderer *r, Cell *cell, size_t width)
{
    free(cell->data);
    cell->data = NULL;
    cell->len = cell->pos = 0;
    FILE *stream = open_memstream(&cell->data, &cell->len);
    if (!stream) return false;
    Renderer inner = {
        .ctx.style = r->ctx.style, .out = stream, .ansi = r->ansi,
        .width = width, .hard_wrap = width != 0, .cell_mode = true
    };
    bool ok = render(&inner, cell->node);
    if (fclose(stream)) ok = false;
    while (cell->len && cell->data[cell->len - 1] == '\n') cell->len--;
    return ok;
}

static bool collect_row(Renderer *r, Row *row, lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *c = node->first_child; c; c = c->next) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT ||
            (c->local_name != LXB_TAG_TD && c->local_name != LXB_TAG_TH) ||
            has_attribute(c, "hidden")) continue;
        if (!reserve((void **)&row->cells, &row->cap, row->count + 1,
                     sizeof(*row->cells))) return false;
        Cell *cell = &row->cells[row->count++];
        *cell = (Cell){.node = c};
        size_t n;
        const unsigned char *align = attribute(c, "align", &n);
        if (align && n == 6 && !strncasecmp((const char *)align, "center", n))
            cell->align = 1;
        if (align && n == 5 && !strncasecmp((const char *)align, "right", n))
            cell->align = 2;
        if (!capture_cell(r, cell, 0)) return false;
    }
    return true;
}

static void table_spaces(Renderer *r, size_t count)
{
    while (count--) glyph(r, (const unsigned char *)" ", 1, ' ');
}

static void table_border(Renderer *r, const size_t *widths, size_t columns,
                         const char *left, const char *middle, const char *right)
{
    literal(r, left);
    for (size_t col = 0; col < columns; col++) {
        for (size_t i = 0; i < widths[col] + 2; i++) literal(r, "─");
        literal(r, col + 1 == columns ? right : middle);
    }
    newline(r);
}

static bool render_table(Renderer *r, lxb_dom_node_t *table)
{
    Row *rows = NULL;
    size_t count = 0, cap = 0, columns = 0;
    size_t *widths = NULL;
    bool ok = false;
    /* Only inspect this table's rows, not rows of tables inside cells. */
    for (lxb_dom_node_t *group = table->first_child; group; group = group->next) {
        if (group->type != LXB_DOM_NODE_TYPE_ELEMENT || has_attribute(group, "hidden"))
            continue;
        if (group->local_name == LXB_TAG_CAPTION) {
            if (!render(r, group)) goto done;
            boundary(r, 1);
            continue;
        }
        bool section = group->local_name == LXB_TAG_THEAD ||
                       group->local_name == LXB_TAG_TBODY ||
                       group->local_name == LXB_TAG_TFOOT;
        lxb_dom_node_t *node = section ? group->first_child : group;
        while (node) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                node->local_name == LXB_TAG_TR && !has_attribute(node, "hidden")) {
                if (!reserve((void **)&rows, &cap, count + 1, sizeof(*rows))) goto done;
                Row *row = &rows[count++];
                *row = (Row){0};
                if (!collect_row(r, row, node)) goto done;
                if (row->count > columns) columns = row->count;
            }
            node = section ? node->next : NULL;
        }
    }
    if (!columns) { ok = true; goto done; }
    widths = calloc(columns, sizeof(*widths));
    if (!widths) goto done;
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < rows[i].count; j++) {
            Cell *cell = &rows[i].cells[j];
            size_t w = display_width(cell->data, cell->len);
            if (w > widths[j]) widths[j] = w;
        }
    size_t total = 0;
    for (size_t j = 0; j < columns; j++) {
        if (widths[j] < 2) widths[j] = 2;
        total += widths[j];
    }
    size_t overhead = r->ctx.indent + r->ctx.quotes * 2 + columns * 3 + 1;
    if (r->width) {
        size_t budget = r->width > overhead ? r->width - overhead : 0;
        if (budget < columns * 2) budget = columns * 2;
        while (total > budget) {
            size_t widest = 0;
            for (size_t j = 1; j < columns; j++)
                if (widths[j] > widths[widest]) widest = j;
            size_t reduce = (total - budget + columns - 1) / columns;
            if (reduce > widths[widest] - 2) reduce = widths[widest] - 2;
            widths[widest] -= reduce;
            total -= reduce;
        }
        /* Render again at the selected widths so wrapping retains styles,
         * explicit line breaks, code spacing, and complete link destinations. */
        for (size_t i = 0; i < count; i++)
            for (size_t j = 0; j < rows[i].count; j++)
                if (!capture_cell(r, &rows[i].cells[j], widths[j])) goto done;
        /* Wide glyphs or indented block content may need more than the minimum. */
        for (size_t i = 0; i < count; i++)
            for (size_t j = 0; j < rows[i].count; j++) {
                Cell *cell = &rows[i].cells[j];
                size_t w = display_width(cell->data, cell->len);
                if (w > widths[j]) widths[j] = w;
            }
    }
    unsigned saved_style = r->ctx.style;
    r->ctx.style = 0;
    style(r, 0);
    boundary(r, 1);
    table_border(r, widths, columns, "┌", "┬", "┐");
    for (size_t i = 0; i < count; i++) {
        Row *row = &rows[i];
        bool more;
        do {
            more = false;
            literal(r, "│");
            for (size_t j = 0; j < columns; j++) {
                Cell *cell = j < row->count ? &row->cells[j] : NULL;
                size_t start = cell ? cell->pos : 0, end = start;
                if (cell) while (end < cell->len && cell->data[end] != '\n') end++;
                size_t w = cell ? display_width(cell->data + start, end - start) : 0;
                size_t pad = widths[j] - w;
                size_t before = cell && cell->align == 2 ? pad :
                                cell && cell->align == 1 ? pad / 2 : 0;
                table_spaces(r, before + 1);
                if (cell) {
                    fwrite(cell->data + start, 1, end - start, r->out);
                    r->col += w;
                    cell->pos = end < cell->len ? end + 1 : end;
                    if (cell->pos < cell->len) more = true;
                }
                table_spaces(r, pad - before + 1);
                literal(r, "│");
            }
            newline(r);
        } while (more);
        table_border(r, widths, columns, i + 1 == count ? "└" : "├",
                     i + 1 == count ? "┴" : "┼", i + 1 == count ? "┘" : "┤");
    }
    r->ctx.style = saved_style;
    boundary(r, 2);
    ok = true;
done:
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < rows[i].count; j++) free(rows[i].cells[j].data);
        free(rows[i].cells);
    }
    free(rows);
    free(widths);
    return ok;
}

typedef struct {
    lxb_dom_node_t *node, *child;
    Context saved;
    long ordinal, step;
} Frame;

/* Returns false for nodes whose children must not be displayed. */
static bool enter(Renderer *r, Frame *stack, size_t depth)
{
    Frame *f = &stack[depth - 1];
    lxb_dom_node_t *node = f->node;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_character_data_t *d = lxb_dom_interface_character_data(node);
        text(r, d->data.data, d->data.length);
        return false;
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    uintptr_t tag = node->local_name;
    if (tag == LXB_TAG_HEAD || tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE ||
        tag == LXB_TAG_TEMPLATE || tag == LXB_TAG_TITLE) return false;
    size_t len;
    if (has_attribute(node, "hidden")) return false;
    /* A loose list's first paragraph follows the marker on the same line. */
    bool first_list_paragraph = tag == LXB_TAG_P && node->parent &&
        node->parent->local_name == LXB_TAG_LI;
    if (first_list_paragraph) {
        for (lxb_dom_node_t *p = node->prev; p; p = p->prev)
            if (p->type == LXB_DOM_NODE_TYPE_ELEMENT ||
                p->type == LXB_DOM_NODE_TYPE_TEXT) {
                /* Formatting whitespace before the first paragraph is fine. */
                if (p->type == LXB_DOM_NODE_TYPE_TEXT) {
                    lxb_dom_character_data_t *d = lxb_dom_interface_character_data(p);
                    size_t i = 0;
                    while (i < d->data.length && space(d->data.data[i])) i++;
                    if (i == d->data.length) continue;
                }
                first_list_paragraph = false;
                break;
            }
    }
    if (block(tag) && !first_list_paragraph) boundary(r, 2);
    switch (tag) {
    case LXB_TAG_TABLE:
        if (!r->cell_mode) {
            if (!render_table(r, node)) r->failed = true;
            return false;
        }
        break;
    case LXB_TAG_H1: case LXB_TAG_H2: case LXB_TAG_H3:
    case LXB_TAG_H4: case LXB_TAG_H5: case LXB_TAG_H6:
        boundary(r, 2);
        r->ctx.style |= BOLD | HEADING;
        break;
    case LXB_TAG_STRONG: case LXB_TAG_B: r->ctx.style |= BOLD; break;
    case LXB_TAG_EM: case LXB_TAG_I: case LXB_TAG_CITE:
    case LXB_TAG_VAR: r->ctx.style |= ITALIC; break;
    case LXB_TAG_U: case LXB_TAG_INS: r->ctx.style |= UNDERLINE; break;
    case LXB_TAG_DEL: case LXB_TAG_S: case LXB_TAG_STRIKE:
        r->ctx.style |= STRIKE; break;
    case LXB_TAG_MARK: r->ctx.style |= MARK; break;
    case LXB_TAG_CODE: case LXB_TAG_KBD: case LXB_TAG_SAMP:
        r->ctx.style |= CODE;
        r->ctx.verbatim = true;
        break;
    case LXB_TAG_PRE:
        boundary(r, 2);
        r->ctx.pre = true;
        r->ctx.style |= CODE;
        r->ctx.indent += 4;
        break;
    case LXB_TAG_BLOCKQUOTE:
        boundary(r, 1);
        r->ctx.quotes++;
        break;
    case LXB_TAG_UL: case LXB_TAG_OL:
        boundary(r, 1);
        f->step = 1;
        if (tag == LXB_TAG_OL && has_attribute(node, "reversed")) {
            f->step = -1;
            f->ordinal = 0;
            for (lxb_dom_node_t *c = node->first_child; c; c = c->next)
                if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && c->local_name == LXB_TAG_LI)
                    f->ordinal++;
        } else f->ordinal = 1;
        f->ordinal = number_attr(node, "start", f->ordinal);
        break;
    case LXB_TAG_LI: {
        boundary(r, 1);
        char marker[64] = "• ";
        for (size_t i = depth - 1; i > 0; i--) {
            Frame *parent = &stack[i - 1];
            if (parent->node->local_name == LXB_TAG_OL) {
                parent->ordinal = number_attr(node, "value", parent->ordinal);
                snprintf(marker, sizeof(marker), "%ld. ", parent->ordinal);
                if ((parent->step > 0 && parent->ordinal < LONG_MAX) ||
                    (parent->step < 0 && parent->ordinal > LONG_MIN))
                    parent->ordinal += parent->step;
                break;
            }
            if (parent->node->local_name == LXB_TAG_UL) break;
        }
        prefix(r);
        size_t marker_start = r->col;
        literal(r, marker);
        r->pending_space = false;
        /* literal() defers trailing whitespace; emit the marker's space. */
        glyph(r, (const unsigned char *)" ", 1, ' ');
        r->ctx.indent += r->col - marker_start;
        break;
    }
    case LXB_TAG_DD: r->ctx.indent += 2; break;
    case LXB_TAG_BR: newline(r); break;
    case LXB_TAG_HR:
        boundary(r, 2);
        literal(r, "────────────────────");
        boundary(r, 2);
        break;
    case LXB_TAG_A: r->ctx.style |= LINK | UNDERLINE; break;
    case LXB_TAG_INPUT: {
        const unsigned char *type = attribute(node, "type", &len);
        if (type && len == 8 && !strncasecmp((const char *)type, "checkbox", len))
            literal(r, has_attribute(node, "checked") ? "[x] " : "[ ] ");
        return false;
    }
    case LXB_TAG_IMG: {
        literal(r, "[image: ");
        const unsigned char *alt = attribute(node, "alt", &len);
        if (alt) text(r, alt, len);
        literal(r, "]");
        return false;
    }
    case LXB_TAG_TR: boundary(r, 1); break;
    case LXB_TAG_TD: case LXB_TAG_TH:
        for (lxb_dom_node_t *p = r->cell_mode && depth == 1 ? NULL : node->prev;
             p; p = p->prev) {
            if (p->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                (p->local_name == LXB_TAG_TD || p->local_name == LXB_TAG_TH)) {
                literal(r, " | ");
                break;
            }
        }
        if (tag == LXB_TAG_TH) r->ctx.style |= BOLD;
        break;
    default: break;
    }
    return true;
}

static void leave(Renderer *r, Frame *f)
{
    lxb_dom_node_t *node = f->node;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        uintptr_t tag = node->local_name;
        if (tag == LXB_TAG_A) {
            size_t n;
            const unsigned char *href = attribute(node, "href", &n);
            if (href && n) {
                r->ctx.style = f->saved.style;
                literal(r, " (");
                text(r, href, n);
                literal(r, ")");
            }
        }
        if (block(tag) || tag == LXB_TAG_PRE ||
            (tag >= LXB_TAG_H1 && tag <= LXB_TAG_H6)) boundary(r, 2);
        if (tag == LXB_TAG_LI || tag == LXB_TAG_UL || tag == LXB_TAG_OL ||
            tag == LXB_TAG_BLOCKQUOTE || tag == LXB_TAG_TR) boundary(r, 1);
    }
    r->ctx = f->saved;
}

static bool render(Renderer *r, lxb_dom_node_t *root)
{
    Frame *stack = NULL;
    size_t cap = 0, depth = 0;
    lxb_dom_node_t *next = root;
    while (next || depth) {
        if (r->failed) { free(stack); return false; }
        if (next) {
            if (!reserve((void **)&stack, &cap, depth + 1, sizeof(*stack))) {
                free(stack);
                return false;
            }
            stack[depth++] = (Frame){.node = next, .saved = r->ctx};
            /* Skipped elements must not run leave() side effects. */
            if (!enter(r, stack, depth)) {
                r->ctx = stack[depth - 1].saved;
                depth--;
                next = NULL;
            } else {
                stack[depth - 1].child = next->first_child;
                next = NULL;
            }
        } else {
            Frame *f = &stack[depth - 1];
            if (f->child) {
                next = f->child;
                f->child = next->next;
            } else {
                leave(r, f);
                depth--;
            }
        }
    }
    free(stack);
    style(r, 0);
    boundary(r, 1);
    return true;
}

static void usage(FILE *out)
{
    fputs("Usage: m2t [-g|--github] [--no-ansi] [--width N] [FILE|-]\n"
          "Render CommonMark and embedded HTML5 to an ANSI terminal.\n"
          "Use -g, --github, or invoke as g2t for GitHub Flavored Markdown.\n"
          "Read stdin when FILE is omitted or '-'. Width 0 disables wrapping.\n", out);
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
    Renderer renderer = {.out = stdout, .ansi = true, .width = 80};
    const char *program = strrchr(argv[0], '/');
    program = program ? program + 1 : argv[0];
    bool github = !strcmp(program, "g2t");
    const char *path = NULL;
    bool options = true;
    for (int i = 1; i < argc; i++) {
        if (options && !strcmp(argv[i], "--")) { options = false; continue; }
        if (options && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            usage(stdout);
            return 0;
        }
        if (options && !strcmp(argv[i], "--no-ansi")) { renderer.ansi = false; continue; }
        if (options && (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--github"))) {
            github = true;
            continue;
        }
        if (options && !strcmp(argv[i], "--width")) {
            char *end;
            if (++i == argc || !*argv[i] || *argv[i] == '-') goto bad_args;
            errno = 0;
            unsigned long n = strtoul(argv[i], &end, 10);
            if (errno || *end || n > 1000000) goto bad_args;
            renderer.width = (size_t)n;
            continue;
        }
        if (options && argv[i][0] == '-' && argv[i][1]) goto bad_args;
        if (path) goto bad_args;
        path = argv[i];
    }

    FILE *in = stdin;
    if (path && strcmp(path, "-")) {
        in = fopen(path, "rb");
        if (!in) { fprintf(stderr, "m2t: %s: %s\n", path, strerror(errno)); return 1; }
    }
    Buffer input = {0}, html = {0};
    char chunk[16384];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), in)) != 0) {
        append(chunk, (MD_SIZE)n, &input);
        if (input.failed || input.len > UINT_MAX) break;
    }
    bool read_failed = ferror(in);
    if (in != stdin && fclose(in)) read_failed = true;
    const char *error = NULL;
    lxb_html_document_t *doc = NULL;
    if (read_failed) { error = "failed to read input"; goto done; }
    if (input.failed || input.len > UINT_MAX) { error = "input too large or out of memory"; goto done; }
    /* Markdown owns the syntax. Only its raw HTML is passed through;
     * generated HTML also keeps code/escaped text protected from Lexbor. */
    unsigned dialect = github ? MD_DIALECT_GITHUB : MD_DIALECT_COMMONMARK;
    if (md_html(input.data ? input.data : "", (MD_SIZE)input.len, append, &html,
                dialect, MD_HTML_FLAG_SKIP_UTF8_BOM) != 0 || html.failed) {
        error = "Markdown parsing failed or out of memory";
        goto done;
    }
    doc = lxb_html_document_create();
    if (!doc || lxb_html_document_parse(doc,
            (const lxb_char_t *)(html.data ? html.data : ""), html.len) != LXB_STATUS_OK) {
        error = "HTML parsing failed or out of memory";
        goto done;
    }
    if (!render(&renderer, lxb_dom_interface_node(doc))) error = "out of memory";
    if (fflush(stdout) || ferror(stdout)) error = "failed to write output";
done:
    if (doc) lxb_html_document_destroy(doc);
    free(input.data);
    free(html.data);
    if (error) { fprintf(stderr, "m2t: %s\n", error); return 1; }
    return 0;
bad_args:
    usage(stderr);
    return 2;
}

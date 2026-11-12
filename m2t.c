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
#include <wctype.h>

#include <md4c-html.h>
#include <lexbor/html/html.h>
#include <lexbor/dom/interfaces/character_data.h>
#include "css.h"
#include "images.h"

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

enum { BOLD = 1, ITALIC = 2, UNDERLINE = 4, STRIKE = 8,
       DIM = 16, OVERLINE = 32, BLINK = 64 };

typedef struct {
    unsigned style;
    css_color foreground, background;
    const css_computed_style *computed;
    const css_computed_style *first_base, *first_line, *first_letter;
    size_t first_line_number, first_letter_number;
    size_t indent, quotes;
    bool pre, verbatim, nowrap, pre_line, hidden;
    uint8_t transform;
} Context;

typedef struct {
    Context ctx;
    FILE *out;
    Images *images;
    bool ansi, pending_space, has_output, cell_mode, hard_wrap, failed;
    unsigned active_style;
    css_color active_foreground, active_background;
    size_t col, width, newlines, quote_depth, line_number, letter_number;
    bool word_start;
} Renderer;

static void paint(Renderer *r, const Context *ctx)
{
    unsigned bits = ctx->style;
    if (!r->ansi || (r->active_style == bits && r->active_foreground == ctx->foreground &&
                    r->active_background == ctx->background)) return;
    fputs("\033[0m", r->out);
    if (bits & BOLD) fputs("\033[1m", r->out);
    if (bits & DIM) fputs("\033[2m", r->out);
    if (bits & ITALIC) fputs("\033[3m", r->out);
    if (bits & UNDERLINE) fputs("\033[4m", r->out);
    if (bits & STRIKE) fputs("\033[9m", r->out);
    if (bits & OVERLINE) fputs("\033[53m", r->out);
    if (bits & BLINK) fputs("\033[5m", r->out);
    if (ctx->foreground >> 24)
        fprintf(r->out, "\033[38;2;%u;%u;%um", (ctx->foreground >> 16) & 255,
                (ctx->foreground >> 8) & 255, ctx->foreground & 255);
    if (ctx->background >> 24)
        fprintf(r->out, "\033[48;2;%u;%u;%um", (ctx->background >> 16) & 255,
                (ctx->background >> 8) & 255, ctx->background & 255);
    r->active_style = bits;
    r->active_foreground = ctx->foreground;
    r->active_background = ctx->background;
}

static void style(Renderer *r, unsigned bits)
{
    Context ctx = {.style = bits};
    paint(r, &ctx);
}

static unsigned font_bits(const css_computed_style *s)
{
    unsigned bits = 0;
    unsigned weight = css_computed_font_weight(s);
    if (weight == CSS_FONT_WEIGHT_BOLD || weight == CSS_FONT_WEIGHT_BOLDER ||
        weight >= CSS_FONT_WEIGHT_600) bits |= BOLD;
    if (weight >= CSS_FONT_WEIGHT_100 && weight <= CSS_FONT_WEIGHT_300) bits |= DIM;
    if (css_computed_font_style(s) != CSS_FONT_STYLE_NORMAL) bits |= ITALIC;
    return bits;
}

static unsigned decoration_bits(const css_computed_style *s)
{
    unsigned bits = 0, d = css_computed_text_decoration(s);
    if (d & CSS_TEXT_DECORATION_UNDERLINE) bits |= UNDERLINE;
    if (d & CSS_TEXT_DECORATION_LINE_THROUGH) bits |= STRIKE;
    if (d & CSS_TEXT_DECORATION_OVERLINE) bits |= OVERLINE;
    if (d & CSS_TEXT_DECORATION_BLINK) bits |= BLINK;
    return bits;
}

static void apply_css(Renderer *r, const css_computed_style *s)
{
    if (!s) return;
    r->ctx.computed = s;
    unsigned decorations = r->ctx.style & (UNDERLINE | STRIKE | OVERLINE | BLINK);
    r->ctx.style = decorations | font_bits(s) | decoration_bits(s);
    css_computed_color(s, &r->ctx.foreground);
    css_color bg;
    css_computed_background_color(s, &bg);
    if (bg >> 24) r->ctx.background = bg;
    unsigned ws = css_computed_white_space(s);
    r->ctx.pre = ws == CSS_WHITE_SPACE_PRE;
    r->ctx.verbatim = ws == CSS_WHITE_SPACE_PRE_WRAP;
    r->ctx.pre_line = ws == CSS_WHITE_SPACE_PRE_LINE;
    r->ctx.nowrap = ws == CSS_WHITE_SPACE_NOWRAP || ws == CSS_WHITE_SPACE_PRE;
    r->ctx.hidden = css_computed_visibility(s) != CSS_VISIBILITY_VISIBLE;
    r->ctx.transform = css_computed_text_transform(s);
}

static void pseudo_paint(Context *ctx, const css_computed_style *base,
                          const css_computed_style *pseudo)
{
    if (!base || !pseudo) return;
    css_color normal, special;
    css_computed_color(base, &normal);
    css_computed_color(pseudo, &special);
    if (normal != special) ctx->foreground = special;
    css_computed_background_color(base, &normal);
    css_computed_background_color(pseudo, &special);
    if (normal != special && special >> 24) ctx->background = special;
    if (css_computed_font_weight(base) != css_computed_font_weight(pseudo))
        ctx->style = (ctx->style & ~(BOLD | DIM)) | (font_bits(pseudo) & (BOLD | DIM));
    if (css_computed_font_style(base) != css_computed_font_style(pseudo))
        ctx->style = (ctx->style & ~ITALIC) | (font_bits(pseudo) & ITALIC);
    ctx->style |= decoration_bits(pseudo);
    if (css_computed_text_transform(base) != css_computed_text_transform(pseudo))
        ctx->transform = css_computed_text_transform(pseudo);
}

static void newline(Renderer *r)
{
    style(r, 0);
    fputc('\n', r->out);
    r->col = 0;
    r->newlines++;
    r->line_number++;
    r->pending_space = false;
    r->word_start = true;
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
    if ((r->hard_wrap || (r->ctx.verbatim && !r->ctx.nowrap)) && r->width &&
        r->col > r->ctx.indent + r->ctx.quotes * 2 &&
        r->col + char_width(cp) > r->width) newline(r);
    prefix(r);
    Context painted = r->ctx;
    if (r->line_number == r->ctx.first_line_number)
        pseudo_paint(&painted, r->ctx.first_base, r->ctx.first_line);
    if (!iswspace((wint_t)cp) && r->letter_number == r->ctx.first_letter_number)
        pseudo_paint(&painted, r->ctx.first_base, r->ctx.first_letter);
    paint(r, &painted);
    if (r->ctx.hidden) {
        for (size_t i = 0; i < char_width(cp); i++) fputc(' ', r->out);
    } else if (cp < 32 || (cp >= 0x7f && cp <= 0x9f) || cp == 0xfffd)
        fputs("�", r->out);
    else {
        uint32_t transformed = cp;
        if (painted.transform == CSS_TEXT_TRANSFORM_UPPERCASE ||
            (painted.transform == CSS_TEXT_TRANSFORM_CAPITALIZE && r->word_start))
            transformed = (uint32_t)towupper((wint_t)cp);
        if (painted.transform == CSS_TEXT_TRANSFORM_LOWERCASE)
            transformed = (uint32_t)towlower((wint_t)cp);
        if (transformed == cp) fwrite(s, 1, n, r->out);
        else {
            char bytes[MB_LEN_MAX];
            mbstate_t state = {0};
            size_t len = wcrtomb(bytes, (wchar_t)transformed, &state);
            if (len == (size_t)-1) fwrite(s, 1, n, r->out);
            else { fwrite(bytes, 1, len, r->out); cp = transformed; }
        }
    }
    r->word_start = iswspace((wint_t)cp) != 0;
    if (!r->word_start) r->letter_number++;
    r->col += char_width(cp);
    r->newlines = 0;
    r->has_output = true;
}

static void text(Renderer *r, const unsigned char *s, size_t n)
{
    if ((r->ctx.verbatim || r->ctx.pre) && r->pending_space) {
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
            if (r->ctx.pre_line && s[i] == '\n') { newline(r); i++; continue; }
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
                if (!r->ctx.nowrap && r->width && r->col + 1 + word_width > r->width) newline(r);
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

static void generated(Renderer *r, lxb_dom_node_t *node, css_pseudo_element pseudo)
{
    const css_computed_style *s = css_node_style(node, pseudo);
    if (!s || css_computed_display_static(s) == CSS_DISPLAY_NONE) return;
    const css_computed_content_item *items;
    if (css_computed_content(s, &items) != CSS_CONTENT_SET || !items) return;
    Context saved = r->ctx;
    apply_css(r, s);
    for (const css_computed_content_item *item = items;
         item->type != CSS_COMPUTED_CONTENT_NONE; item++) {
        if (item->type == CSS_COMPUTED_CONTENT_STRING) {
            text(r, (const unsigned char *)lwc_string_data(item->data.string),
                 lwc_string_length(item->data.string));
        } else if (item->type == CSS_COMPUTED_CONTENT_ATTR) {
            size_t len;
            const unsigned char *value = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *)lwc_string_data(item->data.attr),
                lwc_string_length(item->data.attr), &len);
            if (value) text(r, value, len);
        } else if (item->type == CSS_COMPUTED_CONTENT_OPEN_QUOTE ||
                   item->type == CSS_COMPUTED_CONTENT_CLOSE_QUOTE) {
            lwc_string **quotes;
            css_computed_quotes(s, &quotes);
            size_t pairs = 0;
            if (quotes) while (quotes[pairs * 2] && quotes[pairs * 2 + 1]) pairs++;
            bool closing = item->type == CSS_COMPUTED_CONTENT_CLOSE_QUOTE;
            if (closing && r->quote_depth) r->quote_depth--;
            size_t index = r->quote_depth < pairs ? r->quote_depth : pairs ? pairs - 1 : 0;
            if (pairs) {
                lwc_string *quote = quotes[index * 2 + closing];
                text(r, (const unsigned char *)lwc_string_data(quote), lwc_string_length(quote));
            }
            if (!closing) r->quote_depth++;
        } else if (item->type == CSS_COMPUTED_CONTENT_NO_OPEN_QUOTE) r->quote_depth++;
        else if (item->type == CSS_COMPUTED_CONTENT_NO_CLOSE_QUOTE && r->quote_depth)
            r->quote_depth--;
    }
    r->ctx = saved;
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

static unsigned image_dimension(lxb_dom_node_t *node, const char *name, bool width)
{
    long attribute_size = number_attr(node, name, 0);
    unsigned size = attribute_size > 0 && (unsigned long)attribute_size <= UINT_MAX ?
                    (unsigned)attribute_size : 0;
    const css_computed_style *s = css_node_style(node, CSS_PSEUDO_ELEMENT_NONE);
    css_fixed length;
    css_unit unit;
    if (s) {
        unsigned status = width ? css_computed_width(s, &length, &unit) :
                                  css_computed_height(s, &length, &unit);
        if (status == CSS_WIDTH_SET && unit == CSS_UNIT_PX && length > 0)
            size = FIXTOINT(length) > 0 ? (unsigned)FIXTOINT(length) : 1;
    }
    return size;
}

static void image_placeholder(Renderer *r, lxb_dom_node_t *node, unsigned columns)
{
    size_t len;
    const unsigned char *label = attribute(node, "alt", &len);
    size_t start = 0;
    while (label && start < len && space(label[start])) start++;
    if (!label || start == len) {
        label = (const unsigned char *)"[image]";
        len = 7;
        start = 0;
    }
    size_t width = 0;
    bool gap = false;
    for (size_t i = start; i < len;) {
        if (space(label[i])) { gap = width != 0; i++; continue; }
        uint32_t cp;
        size_t n = decode(label + i, len - i, &cp);
        size_t w = char_width(cp);
        if (gap) {
            if (width + 1 >= columns) { literal(r, "…"); break; }
            glyph(r, (const unsigned char *)" ", 1, ' ');
            width++;
            gap = false;
        }
        if (width + w > columns || (width + w == columns && i + n < len)) {
            if (width < columns) literal(r, "…");
            break;
        }
        glyph(r, label + i, n, cp);
        width += w;
        i += n;
    }
}

static bool render_image(Renderer *r, lxb_dom_node_t *node)
{
    if (!r->images || !r->ansi || r->cell_mode || r->ctx.hidden || r->out != stdout) return false;
    size_t len;
    const unsigned char *source = attribute(node, "src", &len);
    if (!source || !len) return false;
    char *path = strndup((const char *)source, len);
    if (!path) return false;
    ImageSlot slot;
    size_t inset = r->ctx.indent + r->ctx.quotes * 2;
    bool queued = image_queue(r->images, path, r->width, inset,
        image_dimension(node, "width", true), image_dimension(node, "height", false),
        r->ctx.background, &slot);
    free(path);
    if (!queued) return false;
    boundary(r, 1);
    style(r, 0);
    /* Reserve the final area without waiting for image metadata. A transparent
     * KGP anchor will track it through subsequent scrolling and scrollback. */
    for (unsigned i = 1; i < slot.rows; i++) fputs("\r\n", r->out);
    if (slot.rows > 1) fprintf(r->out, "\033[%uA", slot.rows - 1);
    fputc('\r', r->out);
    r->col = 0;
    for (unsigned i = 0; i < slot.rows; i++) {
        prefix(r);
        if (!i) image_placeholder(r, node, slot.columns);
        if (i + 1 < slot.rows) { newline(r); fputc('\r', r->out); }
    }
    if (slot.rows > 1) fprintf(r->out, "\033[%uA", slot.rows - 1);
    fprintf(r->out, "\033[%zuG", inset + 1);
    if (!image_anchor(r->images, r->out, &slot)) r->failed = true;
    if (slot.rows > 1) fprintf(r->out, "\033[%uB", slot.rows - 1);
    fputc('\r', r->out);
    r->col = r->newlines = 0;
    r->has_output = true;
    newline(r);
    return true;
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
    css_color background;
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
        .ctx = r->ctx, .out = stream, .ansi = r->ansi,
        .width = width, .hard_wrap = width != 0, .cell_mode = true, .word_start = true
    };
    inner.ctx.indent = inner.ctx.quotes = 0;
    inner.ctx.first_base = inner.ctx.first_line = inner.ctx.first_letter = NULL;
    for (lxb_dom_node_t *n = cell->node; n; n = n->parent) {
        const css_computed_style *s = css_node_style(n, CSS_PSEUDO_ELEMENT_NONE);
        css_color background = 0;
        if (s) css_computed_background_color(s, &background);
        if (background >> 24) { inner.ctx.background = background; break; }
    }
    cell->background = inner.ctx.background;
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
            css_node_omitted(c)) continue;
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
        const css_computed_style *s = css_node_style(c, CSS_PSEUDO_ELEMENT_NONE);
        if (s) {
            unsigned alignment = css_computed_text_align(s);
            if (alignment == CSS_TEXT_ALIGN_CENTER || alignment == CSS_TEXT_ALIGN_LIBCSS_CENTER)
                cell->align = 1;
            else if (alignment == CSS_TEXT_ALIGN_RIGHT || alignment == CSS_TEXT_ALIGN_LIBCSS_RIGHT)
                cell->align = 2;
            else if (alignment == CSS_TEXT_ALIGN_LEFT || alignment == CSS_TEXT_ALIGN_LIBCSS_LEFT)
                cell->align = 0;
        }
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
        if (group->type != LXB_DOM_NODE_TYPE_ELEMENT ||
            css_node_omitted(group))
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
                node->local_name == LXB_TAG_TR &&
                !css_node_omitted(node)) {
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
                css_color table_background = r->ctx.background;
                if (cell) r->ctx.background = cell->background;
                table_spaces(r, before + 1);
                if (cell) {
                    style(r, 0);
                    fwrite(cell->data + start, 1, end - start, r->out);
                    r->col += w;
                    cell->pos = end < cell->len ? end + 1 : end;
                    if (cell->pos < cell->len) more = true;
                }
                table_spaces(r, pad - before + 1);
                r->ctx.background = table_background;
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
    if (css_node_omitted(node)) return false;
    apply_css(r, css_node_style(node, CSS_PSEUDO_ELEMENT_NONE));
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
            generated(r, node, CSS_PSEUDO_ELEMENT_BEFORE);
            if (!render_table(r, node)) r->failed = true;
            generated(r, node, CSS_PSEUDO_ELEMENT_AFTER);
            return false;
        }
        break;
    case LXB_TAG_H1: case LXB_TAG_H2: case LXB_TAG_H3:
    case LXB_TAG_H4: case LXB_TAG_H5: case LXB_TAG_H6:
        boundary(r, 2);
        break;
    case LXB_TAG_PRE:
        boundary(r, 2);
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
            if (parent->node->local_name == LXB_TAG_OL || parent->node->local_name == LXB_TAG_UL) {
                parent->ordinal = number_attr(node, "value", parent->ordinal);
                const css_computed_style *s = r->ctx.computed;
                if (s && css_computed_list_style_type(s) == CSS_LIST_STYLE_TYPE_NONE) {
                    marker[0] = '\0';
                } else if (s && parent->ordinal >= INT_MIN && parent->ordinal <= INT_MAX) {
                    size_t len;
                    if (css_computed_format_list_style(s, (int)parent->ordinal, marker,
                                                        sizeof(marker) - 2, &len) == CSS_OK &&
                        len < sizeof(marker) - 2) {
                        marker[len] = '\0';
                        if (len && marker[len - 1] != ' ') strcat(marker, " ");
                    }
                } else snprintf(marker, sizeof(marker), "%ld. ", parent->ordinal);
                if ((parent->step > 0 && parent->ordinal < LONG_MAX) ||
                    (parent->step < 0 && parent->ordinal > LONG_MIN))
                    parent->ordinal += parent->step;
                break;
            }
        }
        prefix(r);
        size_t marker_start = r->col;
        literal(r, marker);
        r->pending_space = false;
        /* literal() defers trailing whitespace; emit the marker's space. */
        if (*marker) glyph(r, (const unsigned char *)" ", 1, ' ');
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
    case LXB_TAG_INPUT: {
        const unsigned char *type = attribute(node, "type", &len);
        if (type && len == 8 && !strncasecmp((const char *)type, "checkbox", len))
            literal(r, has_attribute(node, "checked") ? "[x] " : "[ ] ");
        return false;
    }
    case LXB_TAG_IMG: {
        if (render_image(r, node)) return false;
        const unsigned char *alt = attribute(node, "alt", &len);
        size_t start = 0;
        while (alt && start < len && space(alt[start])) start++;
        if (alt && start < len) {
            literal(r, "[image: ");
            text(r, alt, len);
            literal(r, "]");
        } else literal(r, "[image]");
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
        break;
    default: break;
    }
    const css_computed_style *s = r->ctx.computed;
    if (s && (css_computed_display_static(s) == CSS_DISPLAY_BLOCK ||
              css_computed_display_static(s) == CSS_DISPLAY_LIST_ITEM ||
              css_computed_display_static(s) == CSS_DISPLAY_TABLE_CELL)) {
        r->ctx.first_base = s;
        r->ctx.first_line = css_node_style(node, CSS_PSEUDO_ELEMENT_FIRST_LINE);
        r->ctx.first_letter = css_node_style(node, CSS_PSEUDO_ELEMENT_FIRST_LETTER);
        r->ctx.first_line_number = r->line_number;
        r->ctx.first_letter_number = r->letter_number;
    }
    generated(r, node, CSS_PSEUDO_ELEMENT_BEFORE);
    return true;
}

static void leave(Renderer *r, Frame *f)
{
    lxb_dom_node_t *node = f->node;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        uintptr_t tag = node->local_name;
        generated(r, node, CSS_PSEUDO_ELEMENT_AFTER);
        if (tag == LXB_TAG_A) {
            size_t n;
            const unsigned char *href = attribute(node, "href", &n);
            if (href && n) {
                r->ctx = f->saved;
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
    fputs("Usage: m2t [-g|--github] [--no-ansi] [--width N] [--images MODE] [FILE|-]\n"
          "Render CommonMark and embedded HTML5 to an ANSI terminal.\n"
          "Use -g, --github, or invoke as g2t for GitHub Flavored Markdown.\n"
          "Images: auto (detect Kitty graphics), always, or never. Default: auto.\n"
          "--image-timeout N sets the image load deadline in seconds (default: 10).\n"
          "Read stdin when FILE is omitted or '-'. Width 0 disables wrapping.\n", out);
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
    Renderer renderer = {.out = stdout, .ansi = true, .width = 80, .word_start = true};
    const char *program = strrchr(argv[0], '/');
    program = program ? program + 1 : argv[0];
    bool github = !strcmp(program, "g2t");
    const char *path = NULL;
    ImageMode image_mode = IMAGES_AUTO;
    unsigned image_timeout = 10;
    bool options = true;
    for (int i = 1; i < argc; i++) {
        if (options && !strcmp(argv[i], "--")) { options = false; continue; }
        if (options && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            usage(stdout);
            return 0;
        }
        if (options && !strcmp(argv[i], "--no-ansi")) { renderer.ansi = false; continue; }
        if (options && !strcmp(argv[i], "--no-images")) { image_mode = IMAGES_NEVER; continue; }
        if (options && (!strcmp(argv[i], "--image-timeout") ||
                        !strncmp(argv[i], "--image-timeout=", 16))) {
            const char *value;
            if (!strcmp(argv[i], "--image-timeout")) {
                if (++i == argc) goto bad_args;
                value = argv[i];
            } else value = argv[i] + 16;
            char *end;
            if (!*value || *value == '-') goto bad_args;
            errno = 0;
            unsigned long n = strtoul(value, &end, 10);
            if (errno || *end || !n || n > 3600) goto bad_args;
            image_timeout = (unsigned)n;
            continue;
        }
        if (options && (!strcmp(argv[i], "--images") || !strncmp(argv[i], "--images=", 9))) {
            const char *mode;
            if (!strcmp(argv[i], "--images")) {
                if (++i == argc) goto bad_args;
                mode = argv[i];
            } else mode = argv[i] + 9;
            if (!strcmp(mode, "auto")) image_mode = IMAGES_AUTO;
            else if (!strcmp(mode, "always")) image_mode = IMAGES_ALWAYS;
            else if (!strcmp(mode, "never")) image_mode = IMAGES_NEVER;
            else goto bad_args;
            continue;
        }
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
    CssDocument *css = NULL;
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
    css = css_document_create(lxb_dom_interface_node(doc), path, renderer.width, renderer.ansi);
    if (!css) { error = "CSS parsing or selection failed"; goto done; }
    renderer.images = images_create(path, renderer.ansi ? image_mode : IMAGES_NEVER, image_timeout);
    if (!renderer.images) { error = "out of memory"; goto done; }
    if (!render(&renderer, lxb_dom_interface_node(doc))) error = "out of memory";
    if (fflush(stdout) || ferror(stdout)) error = "failed to write output";
    if (!error && !images_finish(renderer.images, stdout)) error = "failed to write image output";
done:
    images_destroy(renderer.images);
    css_document_destroy(css);
    if (doc) lxb_html_document_destroy(doc);
    free(input.data);
    free(html.data);
    if (error) { fprintf(stderr, "m2t: %s\n", error); return 1; }
    return 0;
bad_args:
    usage(stderr);
    return 2;
}

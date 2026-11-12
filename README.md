# m2t

Render Markdown and embedded HTML5 in the terminal using ANSI styles.

`m2t` uses CommonMark by default. Run it as `g2t`, or pass `-g` / `--github`,
for GitHub Flavored Markdown. Both support headings, lists, quotes, code,
links, and HTML tables. GFM adds Markdown tables, task lists, strikethrough,
and bare URL autolinks.

## Build and install

Requires a C11 compiler, Make, pkg-config, and development packages for:

- [MD4C](https://github.com/mity/md4c), including its HTML renderer
- [Lexbor](https://lexbor.com/modules/html/)
- [libcss](https://www.netsurf-browser.org/projects/libcss/), with LibParserUtils
  and LibWapcaplet
- stb headers (`stb_image.h`), libwebp, and libcurl for images

These libraries are required for either Markdown mode.

```sh
make
make install
```

The build prefers static library archives when available, falling back to shared
libraries otherwise. The C runtime uses the compiler's default linking mode.
To use normal shared-library selection, run `make clean && make PREFER_STATIC=0`.

Installation places `m2t` and the `g2t` symlink in `/usr/local/bin`. Use elevated
privileges if needed, or install into your own directory:

```sh
make install PREFIX="$HOME/.local"
```

Remove them with `make uninstall`, using the same `PREFIX` you installed with.
Both targets also accept `bindir` and `DESTDIR` overrides for custom paths and
package staging. Run `make test` for the test suite (requires Python 3).

## Usage

```sh
m2t document.md
g2t README.md
cat document.md | m2t
m2t document.md | less -R
m2t --no-ansi document.md > document.txt
```

With no file argument, or with `-`, input comes from stdin.

| Option | Effect |
| --- | --- |
| `-g`, `--github` | Use GitHub Flavored Markdown |
| `--width N` | Wrap at N columns (default: 80); `0` disables wrapping |
| `--no-ansi` | Output plain text |
| `--images MODE` | Image output: `auto` (default), `always`, or `never` |
| `--no-images` | Keep image alt text instead of graphics |
| `--image-timeout N` | Image load deadline in seconds (default: 10) |
| `-h`, `--help` | Show usage |
| `--` | End options, allowing filenames that start with `-` |

ANSI styles stay enabled when output is redirected. Input and output are UTF-8;
use a UTF-8 locale for correct column widths. Code blocks preserve whitespace,
and long words outside tables can exceed the wrapping width.

Try the included examples directly from the build directory:

```sh
./m2t examples/commonmark-html5.md
./g2t examples/gfm-html5.md
./m2t examples/css-html5.md
```

## Markdown and HTML

MD4C parses both modes, using its GitHub dialect for GFM. The resulting HTML
passes through Lexbor before rendering. Markdown gets first interpretation, so
autolinks work and HTML inside code stays literal. Following CommonMark rules,
Markdown inside a raw HTML block stays literal until that block ends. Both modes
pass raw HTML to Lexbor; MD4C does not apply GFM's disallowed-tag filter.
MD4C accepts both single and double tildes for strikethrough in GFM mode.

Tables have borders, aligned columns, and wrapped multiline cells. Links show
their destinations, images fall back to their alt text, and task checkboxes appear as
`[x]` or `[ ]`.

## Images

On terminals supporting the [Kitty Graphics Protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/),
image loading runs in the background while the document renders. Each image
initially shows its alt text, or `[image]` if no alt text is provided. Once the
text is complete, images fill their reserved areas as they become ready, even
if those areas have scrolled into scrollback. Failed or timed-out loads keep
the text placeholder.

Placeholders use the available width and eight rows by default, capped to the
terminal height. An HTML/CSS pixel height hint adjusts the reserved height.
Long alt labels are clipped to one line. Loaded images preserve their aspect
ratio and are centered within the reserved area; later text does not move.

Detection checks KGP relative placements, which keep deferred images anchored
to their original locations. Terminals without that feature, and redirected
output, retain alt text. `--images always` skips the support check, and
`--no-images` disables graphics. `--no-ansi` always disables graphics too.

PNG, JPEG, GIF (first frame), WebP, and other stb raster formats are supported.
Sources can be local files, HTTP(S) URLs, or image data URLs. Local paths resolve
relative to the input document, or the current directory for stdin. Numeric HTML
`width` / `height` and CSS pixel dimensions provide size hints. Images in tables,
missing files, unsupported formats such as SVG, and failed downloads use alt text.

Image downloads happen only when graphics output is enabled. Four workers load
images concurrently. Each source is limited to 64 MiB and decoded images to
16 megapixels. The load deadline includes queue time; change it with
`--image-timeout N` (1–3600 seconds). Transparent images use a CSS or queried
terminal background behind them, falling back to black when neither is known.
In tmux, graphics commands use passthrough, which must be enabled in tmux's
configuration.

## CSS

libcss handles selectors, specificity, inheritance, `!important`, shorthands,
and media queries. Styles come from `style` attributes, `<style>` blocks, and
local `<link rel="stylesheet">` files, including relative `@import`s. File paths
resolve relative to the document or stylesheet; stdin uses the current directory.
Network stylesheets are not fetched. Media queries use a screen viewport with
8 CSS pixels per column and a height of 384 pixels.

The terminal renderer applies:

- Foreground and background colours as 24-bit RGB ANSI sequences.
- Bold, faint, italic/oblique, underline, overline, strikethrough, and blink.
- Whitespace modes, text transforms, `display: none`, and `visibility`.
- List marker styles, table cell alignment, and generated strings, attributes,
  and quotation marks from `::before` / `::after`.
- Text styling for `::first-line` and `::first-letter` when selected by libcss.

Other properties are parsed and computed but have no terminal rendering effect.
This includes graphical layout, fonts and font sizes, CSS background images, opacity, and alpha
blending. Nonzero-alpha colours use their RGB channels. Interactive pseudo-classes
such as `:hover` and `:visited` do not match. Italics, overlines, and blinking also
depend on terminal support. `--no-ansi` removes paint while retaining text effects.

JavaScript is not executed. Hidden elements, scripts, stylesheet
text, templates, and document metadata are omitted.
Merged table cells render as individual cells; nested tables use a simplified
layout. Tables may exceed the requested width when their minimum layout cannot
fit. Input control characters are filtered before terminal output.

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

Both libraries are required for either Markdown mode.

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
| `-h`, `--help` | Show usage |
| `--` | End options, allowing filenames that start with `-` |

ANSI styles stay enabled when output is redirected. Input and output are UTF-8;
use a UTF-8 locale for correct column widths. Code blocks preserve whitespace,
and long words outside tables can exceed the wrapping width.

Try the included examples directly from the build directory:

```sh
./m2t examples/commonmark-html5.md
./g2t examples/gfm-html5.md
```

## Markdown and HTML

MD4C parses both modes, using its GitHub dialect for GFM. The resulting HTML
passes through Lexbor before rendering. Markdown gets first interpretation, so
autolinks work and HTML inside code stays literal. Following CommonMark rules,
Markdown inside a raw HTML block stays literal until that block ends. Both modes pass raw HTML
to Lexbor; MD4C does not apply GFM's disallowed-tag filter.
MD4C accepts both single and double tildes for strikethrough in GFM mode.

Tables have borders, aligned columns, and wrapped multiline cells. Links show
their destinations, images show their alt text, and task checkboxes appear as
`[x]` or `[ ]`.

CSS, JavaScript, external resources, and image pixels are not rendered. Hidden
elements, scripts, styles, templates, and document metadata are omitted.
Merged table cells render as individual cells; nested tables use a simplified
layout. Tables may exceed the requested width when their minimum layout cannot
fit. Input control characters are filtered before terminal output.

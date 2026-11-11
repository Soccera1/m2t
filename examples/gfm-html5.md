# GitHub Flavored Markdown and HTML5

Render this file with `./g2t examples/gfm-html5.md`,
`./m2t -g examples/gfm-html5.md`, or `./m2t --github examples/gfm-html5.md`.

## CommonMark foundation

GFM retains *emphasis*, **strong text**, **nested *emphasis***,
`inline code`, [links](https://example.com), and quotations:

> A quotation containing **strong text** and <u>inline HTML</u>.

## Strikethrough

~~This task was cancelled.~~ This task is still active.
Use double tildes: `~~deleted text~~`.

## Task lists

- [x] Parse GitHub Flavored Markdown with MD4C
- [x] Render HTML5 through Lexbor
- [ ] Review the output
  - [x] Check nested tasks
  - [ ] Try the plain-text option

## GFM tables

| Feature | Syntax | Status |
| :--- | :---: | ---: |
| Emphasis | `*text*` | Ready |
| Strikethrough | `~~text~~` | Ready |
| Task lists | `- [x] task` | Ready |
| Inline HTML | `<mark>text</mark>` | Ready |

Table cells can contain **strong text**, *emphasis*, ~~deleted text~~,
and links:

| Description | Destination |
| --- | --- |
| **Project home** | [Example](https://example.com) |
| ~~Old location~~ | `archive/` |

## Extended autolinks

GFM recognizes bare URLs and email addresses:

- https://example.com/docs
- www.example.com
- reader@example.com

CommonMark angle-bracket autolinks also work: <https://example.com>.

## Inline HTML5

Use <mark>highlighted text</mark>, <u>underlining</u>, and
<strong>HTML containing *Markdown emphasis*</strong>.
An HTML link works too: <a href="https://example.com/html">HTML destination</a>.
Entities decode as text: &copy; &mdash; &lt;tag&gt; &#x1F642;.

## HTML5 blocks

<article>
<h3>An HTML article</h3>
<p>HTML supplies <strong>bold</strong>, <em>italic</em>, and <del>deleted</del> text.</p>
<p>~~GFM syntax inside this raw HTML block stays literal.~~</p>
<p hidden>This hidden paragraph should not appear.</p>
</article>

After the blank line, ~~GFM strikethrough resumes~~.

<ol start="5">
<li>An HTML ordered list</li>
<li>With an explicit starting number</li>
</ol>

<table>
<tr><th>Input</th><th>Parser</th></tr>
<tr><td>GFM</td><td>MD4C</td></tr>
<tr><td>HTML5</td><td>Lexbor</td></tr>
</table>

## Code and raw HTML

Code protects both syntaxes: `~~not deleted~~` and `<mark>not highlighted</mark>`.

```html
<section>
    <p>~~This is code, not strikethrough.~~</p>
</section>
```

MD4C passes raw HTML through to Lexbor. Script content is omitted by the
terminal renderer; the following script should not appear in the output:

<script>console.log("This script is omitted.");</script>

---

Try `--no-ansi` for plain text, or `--width 100` for wider output.

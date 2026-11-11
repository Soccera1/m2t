# CommonMark and HTML5

Render this file with `./m2t examples/commonmark-html5.md`.

## Text and links

This paragraph has *emphasis*, **strong text**, and **nested *emphasis***.
Inline code preserves spaces: `one  two`. A soft
line break joins the paragraph; two trailing spaces create a hard break:  
This starts on a new line.

Here is an [inline link](https://example.com), a [reference link][example],
and a CommonMark autolink: <https://example.com/docs>.
An image renders its alternative text: ![A small mountain landscape](mountains.png).

[example]: https://example.com/reference "An example reference"

## Lists and quotations

- First item
- Second item with **strong text**
  - Nested item
  - Another nested item

3. An ordered list starting at three
4. The next item

> A quotation with *emphasis*.
>
> > A nested quotation.
>
> - A list inside the quotation

## Code and syntax precedence

Backticks protect HTML: `<strong>This stays literal.</strong>`.
Backslashes do the same: \<em>also literal\</em>.
The autolink above is parsed as Markdown before HTML.

```c
#include <stdio.h>

int main(void)
{
    puts("<b>Code is not HTML</b>");
    return 0;
}
```

    <em>Indented code also stays literal.</em>

---

## Inline HTML5

Mix <strong>HTML strong text and *Markdown emphasis*</strong> in a paragraph.
Try <u>underlining</u>, <del>deleted text</del>, <mark>highlighting</mark>,
and a keyboard shortcut: <kbd>Ctrl+C</kbd>.
Entities decode as text: &copy; &mdash; &lt;tag&gt; &#x1F642;.
Here is an HTML break:<br>Text after the break.

## HTML5 blocks

<section>
<h3>A semantic HTML section</h3>
<p>This paragraph uses <em>HTML emphasis</em> and <strong>HTML strong text</strong>.</p>
<p>**Markdown inside this raw HTML block stays literal.**</p>
<p hidden>This hidden paragraph should not appear.</p>
</section>

Markdown resumes after the blank line: **strong text again**.

<dl>
<dt>CommonMark</dt>
<dd>The default Markdown dialect, parsed by MD4C.</dd>
<dt>HTML5</dt>
<dd>The HTML document is parsed by Lexbor.</dd>
</dl>

<table>
<thead><tr><th>Element</th><th>Terminal rendering</th></tr></thead>
<tbody>
<tr><td>strong</td><td>Bold text</td></tr>
<tr><td>em</td><td>Italic text</td></tr>
<tr><td>code</td><td>Code styling</td></tr>
</tbody>
</table>

<pre>HTML preformatted text
    keeps indentation
and  repeated  spaces.</pre>

## Unicode

Café, Ελληνικά, 日本語, and 🙂 are preserved as UTF-8.

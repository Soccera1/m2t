#!/usr/bin/env python3
"""End-to-end checks of parsing precedence, terminal output, and the CLI."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

BINARY = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else "./m2t")
SGR = re.compile(rb"\x1b\[[0-9;]*m")


def run(source=b"", *args):
    if isinstance(source, str):
        source = source.encode()
    return subprocess.run([BINARY, *args], input=source, capture_output=True,
                          timeout=10)


class RendererTests(unittest.TestCase):
    def plain(self, source, *args):
        p = run(source, "--no-ansi", *args)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stderr, b"")
        self.assertNotIn(b"\x1b", p.stdout)
        return p.stdout.decode().rstrip("\n")

    def test_commonmark_blocks(self):
        self.assertEqual(self.plain("Heading\n=======\n\nA paragraph.\n\n---\n"),
                         "Heading\n\nA paragraph.\n\n────────────────────")

    def test_commonmark_wins_over_html_text(self):
        self.assertEqual(self.plain("**bold** and *emphasis*"), "bold and emphasis")

    def test_autolink_wins_over_html_tag(self):
        out = self.plain("<https://example.com> <user@example.com>")
        self.assertIn("https://example.com", out)
        self.assertIn("user@example.com", out)
        self.assertNotIn("<", out)

    def test_code_and_escapes_protect_html(self):
        self.assertEqual(self.plain(r"`<b>literal</b>` and \<i>escaped\</i>"),
                         "<b>literal</b> and <i>escaped</i>")
        self.assertEqual(self.plain("    <b>indented</b>\n"), "    <b>indented</b>")
        self.assertEqual(self.plain("~~~html\n<b>fenced</b>\n~~~"),
                         "    <b>fenced</b>")

    def test_code_spaces_and_entity_literals(self):
        self.assertEqual(self.plain("before `a  b &amp;` after"),
                         "before a  b &amp; after")

    def test_inline_html_spans_markdown(self):
        p = run("a <strong>bold *and italic*</strong> z")
        self.assertEqual(SGR.sub(b"", p.stdout).strip(), b"a bold and italic z")
        self.assertIn(b"\x1b[1m", p.stdout)
        self.assertIn(b"\x1b[3m", p.stdout)
        # Closing the italic span restores the outer bold style.
        p = run("**outer *inner* outer**")
        self.assertIn(b"inner\x1b[0m\x1b[1m outer", p.stdout)
        self.assertTrue(p.stdout.endswith(b"\x1b[0m\n\n"))

    def test_html_blocks_keep_commonmark_rules(self):
        self.assertEqual(self.plain("<div>\n**literal in raw HTML**\n</div>"),
                         "**literal in raw HTML**")

    def test_html5_entities_and_repair(self):
        self.assertEqual(self.plain("<div><p>one<p>two &copy; &#x1f642;</div>"),
                         "one\n\ntwo © 🙂")
        self.assertEqual(self.plain("&amp;amp; &lt;tag&gt; &NotEqualTilde;"),
                         "&amp; <tag> ≂̸")

    def test_full_html_document(self):
        source = ("<!doctype html>\n<html><head><title>Hidden</title></head>"
                  "<body><h1>Title</h1><p>Body</p></body></html>")
        self.assertEqual(self.plain(source), "Title\n\nBody")

    def test_no_script_style_template_or_hidden(self):
        self.assertEqual(self.plain("<div><script>bad()</script><style>bad{}</style>"
                                    "<template>bad</template><span hidden>bad</span>"
                                    "<p>visible</p></div>"), "visible")

    def test_lists(self):
        self.assertEqual(self.plain("3. first\n   - nested\n4. last"),
                         "3. first\n   • nested\n4. last")
        self.assertEqual(self.plain("- first\n\n  paragraph\n\n- last"),
                         "• first\n\n  paragraph\n\n• last")
        self.assertEqual(self.plain("<ol reversed><li>A<li value=7>B<li>C</ol>"),
                         "3. A\n7. B\n6. C")

    def test_blockquotes(self):
        self.assertEqual(self.plain("> quote\n>\n> > nested\n>\n> - item"),
                         "│ quote\n\n│ │ nested\n\n│ • item")

    def test_html_table(self):
        self.assertEqual(self.plain("<table><tr>\n<th>A</th><th>B</th></tr>\n"
                                    "<tr><td>C</td><td>D</td></tr></table>"),
                         "┌────┬────┐\n│ A  │ B  │\n├────┼────┤\n│ C  │ D  │\n└────┴────┘")

    def test_links_images_and_reference_links(self):
        self.assertEqual(self.plain("[name][ref] ![alt](pic.png)\n\n[ref]: /path"),
                         "name (/path) [image: alt]")

    def test_breaks(self):
        self.assertEqual(self.plain("soft\nbreak  \nhard<br>HTML"),
                         "soft break\nhard\nHTML")

    def test_preformatted(self):
        self.assertEqual(self.plain("```\na  b\n\tindent\n\nlast\n```"),
                         "    a  b\n        indent\n\n    last")

    def test_wrap_and_nested_indentation(self):
        self.assertEqual(self.plain("one two three four", "--width", "9"),
                         "one two\nthree\nfour")
        self.assertEqual(self.plain("- one two three", "--width", "9"),
                         "• one two\n  three")
        self.assertEqual(self.plain("one two three", "--width", "0"), "one two three")

    def test_utf8_and_controls(self):
        self.assertEqual(self.plain("日本語 café 🙂"), "日本語 café 🙂")
        source = b"```\n\x1b[31m\x07\xc2\x9b31m\xff\n```"
        self.assertEqual(self.plain(source), "    �[31m��31m�")
        p = run(source)
        self.assertNotIn(b"\x1b", SGR.sub(b"", p.stdout))

    def test_empty_input_and_bom(self):
        self.assertEqual(self.plain(""), "")
        self.assertEqual(self.plain("\ufeff# Title"), "Title")

    def test_deep_html_without_recursive_walk(self):
        self.assertEqual(self.plain("<span>" * 5000 + "text" + "</span>" * 5000),
                         "text")

    def test_file_and_stdin(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.md"
            path.write_text("# From file\n")
            self.assertEqual(self.plain("", str(path)), "From file")
            self.assertEqual(self.plain("stdin", "-"), "stdin")
            self.assertEqual(self.plain("", "--", str(path)), "From file")

    def test_cli_errors(self):
        for args in [("--width",), ("--width", "-1"), ("--width", "abc"),
                     ("--width", "1000001"), ("--unknown",), ("a", "b")]:
            with self.subTest(args=args):
                p = run("", *args)
                self.assertEqual(p.returncode, 2)
                self.assertIn(b"Usage:", p.stderr)
        self.assertEqual(run("", "/no/such/file").returncode, 1)
        self.assertEqual(run("", "--help").returncode, 0)


class GithubTests(unittest.TestCase):
    def plain(self, source, *args):
        p = run(source, "--github", "--no-ansi", *args)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stderr, b"")
        return p.stdout.decode().rstrip("\n")

    def test_strikethrough_and_default_parser(self):
        self.assertEqual(self.plain("~~deleted~~ ~also deleted~"), "deleted also deleted")
        self.assertEqual(self.plain(r"`~literal~` \~escaped\~"), "~literal~ ~escaped~")
        p = run("~~deleted~~", "-g")
        self.assertIn(b"\x1b[9m", p.stdout)
        self.assertEqual(run("~~deleted~~", "--no-ansi").stdout.strip(), b"~~deleted~~")

    def test_tables(self):
        source = "| A | B |\n| :- | -: |\n| C | D |"
        self.assertEqual(self.plain(source),
                         "┌────┬────┐\n│ A  │  B │\n├────┼────┤\n│ C  │  D │\n└────┴────┘")
        self.assertIn(b":-", run(source, "--no-ansi").stdout)

    def test_task_lists(self):
        source = "- [x] done\n- [ ] todo\n  - [X] nested"
        self.assertEqual(self.plain(source), "• [x] done\n• [ ] todo\n  • [x] nested")
        # Uppercase checked markers are normalized only in GFM mode.
        self.assertIn(b"[X] nested", run(source, "--no-ansi").stdout)

    def test_autolinks(self):
        self.assertEqual(self.plain("www.example.com"),
                         "www.example.com (http://www.example.com)")
        self.assertEqual(run("www.example.com", "--no-ansi").stdout.strip(),
                         b"www.example.com")

    def test_raw_html(self):
        self.assertEqual(self.plain("<script>alert(1)</script>"), "")
        self.assertEqual(self.plain("`<script>literal</script>`"),
                         "<script>literal</script>")
        self.assertEqual(self.plain("<div><em>HTML</em></div>"), "HTML")

    def test_markdown_precedence_and_code(self):
        self.assertEqual(self.plain("**bold** `<b>~~code~~</b>`"),
                         "bold <b>~~code~~</b>")
        self.assertEqual(self.plain("\ufeff# Heading"), "Heading")
        self.assertEqual(self.plain(""), "")

    def test_options_and_argv_zero(self):
        source = b"~~deleted~~\n\n| A | B |\n| - | - |\n| C | D |\n\n- [x] done"
        expected = run(source, "--github", "--no-ansi").stdout
        self.assertEqual(run(source, "-g", "--no-ansi").stdout, expected)
        # Test argv[0] directly, including its path, independently of symlinks.
        for argv_zero in ["g2t", "/some/path/g2t"]:
            p = subprocess.run([argv_zero, "--no-ansi"], executable=BINARY,
                               input=source, capture_output=True, timeout=10)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout, expected)
        p = subprocess.run(["/some/path/not-g2t", "--no-ansi"], executable=BINARY,
                           input=b"~~literal~~", capture_output=True, timeout=10)
        self.assertEqual(p.stdout.strip(), b"~~literal~~")

    def test_file_and_wrapping(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "github.md"
            path.write_text("~~one~~ two three")
            self.assertEqual(self.plain("", "--width", "8", str(path)),
                             "one two\nthree")


class TableLayoutTests(unittest.TestCase):
    def plain(self, source, *args):
        p = run(source, "--no-ansi", *args)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout.decode().strip("\n")

    def test_multiline_cells_caption_and_missing_cells(self):
        source = ("<table><caption>Caption</caption>"
                  "<tr><th>A</th><th>B</th></tr>"
                  "<tr><td>one<br>two</td><td>three</td></tr>"
                  "<tr><td></td></tr></table>")
        self.assertEqual(self.plain(source),
                         "Caption\n┌─────┬───────┐\n│ A   │ B     │\n"
                         "├─────┼───────┤\n│ one │ three │\n│ two │       │\n"
                         "├─────┼───────┤\n│     │       │\n└─────┴───────┘")

    def test_unicode_widths_and_long_word_wrapping(self):
        source = ("<table><tr><td>日本語</td><td>é</td></tr>"
                  "<tr><td>short</td><td>longwordlongword</td></tr></table>")
        self.assertEqual(self.plain(source, "--width", "24"),
                         "┌────────┬─────────────┐\n│ 日本語 │ é           │\n"
                         "├────────┼─────────────┤\n│ short  │ longwordlon │\n"
                         "│        │ gword       │\n└────────┴─────────────┘")

    def test_styles_do_not_change_column_widths(self):
        source = ("<table><tr><th>Heading</th><th>Other</th></tr>"
                  "<tr><td><strong>bold</strong> <em>italic</em></td>"
                  "<td><code>a  b</code> <a href='/url'>link</a></td></tr></table>")
        p = run(source, "--width", "24")
        plain = run(source, "--no-ansi", "--width", "24")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(SGR.sub(b"", p.stdout), plain.stdout)
        for code in [b"\x1b[1m", b"\x1b[3m", b"\x1b[33m", b"\x1b[36m"]:
            self.assertIn(code, p.stdout)
        lines = plain.stdout.decode().strip().splitlines()
        self.assertTrue(all(len(line) <= 24 for line in lines))
        self.assertEqual(len(set(map(len, lines))), 1)
        self.assertIn("(/url)", "".join(line.replace("│", "").strip() for line in lines))

    def test_empty_and_hidden_tables(self):
        self.assertEqual(self.plain("<table></table>"), "")
        self.assertEqual(self.plain("<table hidden><tr><td>hidden</td></tr></table>"), "")
        source = ("<table><tr hidden><td>hidden</td></tr>"
                  "<tr><td>visible</td><td hidden>hidden</td></tr></table>")
        self.assertNotIn("hidden", self.plain(source))
        self.assertIn("visible", self.plain(source))

    def test_quoted_tables_and_nested_tables_retain_text(self):
        out = self.plain("> <table><tr><td>quoted</td></tr></table>")
        self.assertTrue(all(line.startswith("│ ") for line in out.splitlines()))
        out = self.plain("<table><tr><td>outer<table><tr><td>inner</td>"
                         "</tr></table></td></tr></table>")
        self.assertIn("outer", out)
        self.assertIn("inner", out)

    def test_center_and_right_alignment(self):
        source = ("| Left | Center | Right |\n| :--- | :---: | ---: |\n"
                  "| x | x | x |")
        out = self.plain(source, "-g")
        self.assertIn("│ x    │   x    │     x │", out)


class ExampleTests(unittest.TestCase):
    def test_examples_keep_content_layout_and_styles(self):
        root = Path(__file__).resolve().parent.parent
        cases = [
            ("commonmark-html5.md", [], [
                "nested emphasis", "[image: A small mountain landscape]",
                "• Nested item", "3. An ordered list starting at three",
                "│ │ A nested quotation.", '<strong>This stays literal.</strong>',
                'puts("<b>Code is not HTML</b>");',
                "**Markdown inside this raw HTML block stays literal.**",
                "Markdown resumes after the blank line: strong text again.",
                "The default Markdown dialect, parsed by MD4C.",
                "Café, Ελληνικά, 日本語, and 🙂", "HTML preformatted text"
            ]),
            ("gfm-html5.md", ["-g"], [
                "This task was cancelled.", "Use double tildes: ~~deleted text~~.",
                "• [x] Parse GitHub Flavored Markdown with MD4C",
                "• [ ] Review the output", "• [x] Check nested tasks",
                "reader@example.com (mailto:reader@example.com)",
                "~~GFM syntax inside this raw HTML block stays literal.~~",
                "After the blank line, GFM strikethrough resumes.",
                "5. An HTML ordered list",
                "<p>~~This is code, not strikethrough.~~</p>"
            ])
        ]
        for filename, args, snippets in cases:
            with self.subTest(filename=filename):
                path = str(root / "examples" / filename)
                styled = run("", *args, path)
                plain = run("", *args, "--no-ansi", path)
                self.assertEqual(styled.returncode, 0, styled.stderr)
                self.assertEqual(plain.returncode, 0, plain.stderr)
                self.assertEqual(SGR.sub(b"", styled.stdout), plain.stdout)
                normalized = " ".join(plain.stdout.decode().split())
                for snippet in snippets:
                    self.assertIn(snippet, normalized)
                self.assertNotIn("This hidden paragraph should not appear.", normalized)
                self.assertNotIn('console.log("This script is omitted.")', normalized)
                for code in [1, 3, 4, 9, 33, 34, 36]:
                    self.assertIn(f"\x1b[{code}m".encode(), styled.stdout)
                self.assertIn(b"\x1b[30;43m", styled.stdout)
                self.assertIn("┌", plain.stdout.decode())
                self.assertIn("└", plain.stdout.decode())


if __name__ == "__main__":
    unittest.main()

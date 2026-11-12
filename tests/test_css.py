#!/usr/bin/env python3
"""Verify libcss selection and the resulting terminal paint, not just parsing."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

BINARY = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else "./m2t")
SGR = re.compile(r"\x1b\[([0-9;]*)m")


def render(source, *args):
    p = subprocess.run([BINARY, *args], input=source.encode(), capture_output=True,
                       timeout=10)
    if p.returncode or p.stderr:
        raise AssertionError((p.returncode, p.stderr.decode(errors="replace")))
    return p.stdout.decode()


def plain(output):
    return SGR.sub("", output)


def painted(output):
    """Return each displayed character with its emitted foreground/background/SGR."""
    chars = []
    fg = bg = None
    attrs = set()
    end = 0
    for match in SGR.finditer(output):
        chars.extend((c, fg, bg, frozenset(attrs)) for c in output[end:match.start()])
        codes = [int(c) for c in match[1].split(";") if c] or [0]
        i = 0
        while i < len(codes):
            code = codes[i]
            if code == 0:
                fg = bg = None
                attrs.clear()
            elif code in (38, 48) and codes[i + 1] == 2:
                color = tuple(codes[i + 2:i + 5])
                if code == 38:
                    fg = color
                else:
                    bg = color
                i += 4
            else:
                attrs.add(code)
            i += 1
        end = match.end()
    chars.extend((c, fg, bg, frozenset(attrs)) for c in output[end:])
    return chars


def paint_of(output, word):
    chars = painted(output)
    index = "".join(c[0] for c in chars).index(word)
    return chars[index][1:]


class CssTests(unittest.TestCase):
    def test_color_formats(self):
        for value, expected in [("red", (255, 0, 0)), ("#123456", (18, 52, 86)),
                                ("#abc", (170, 187, 204)),
                                ("rgb(10, 20, 30)", (10, 20, 30)),
                                ("rgb(100%, 0%, 0%)", (255, 0, 0)),
                                ("hsl(120, 100%, 50%)", (0, 255, 0))]:
            with self.subTest(value=value):
                out = render(f'<span style="color:{value}">text</span>')
                self.assertEqual(paint_of(out, "text")[0], expected)

    def test_foreground_background_inheritance_and_restore(self):
        out = render('<span style="color:red;background:#123456">red'
                     '<b style="color:blue">blue</b>red-again</span> normal')
        self.assertEqual(paint_of(out, "red")[:2], ((255, 0, 0), (18, 52, 86)))
        self.assertEqual(paint_of(out, "blue")[:2], ((0, 0, 255), (18, 52, 86)))
        self.assertEqual(paint_of(out, "red-again")[0], (255, 0, 0))
        self.assertEqual(paint_of(out, "normal")[:2], (None, None))

    def test_background_shorthand_and_currentcolor(self):
        out = render('<span style="color:#123456;background:currentColor">text</span>')
        self.assertEqual(paint_of(out, "text")[:2], ((18, 52, 86), (18, 52, 86)))

    def test_cascade_specificity_important_and_late_sheets(self):
        out = render('<style>p{color:blue}.note{color:green}#id{color:red}</style>\n\n'
                     '<p class="note" id="id" style="color:purple">inline</p>\n\n'
                     '<style>#id{color:lime!important}</style>')
        self.assertEqual(paint_of(out, "inline")[0], (0, 255, 0))
        out = render('<style>p{color:blue!important}</style>\n\n'
                     '<p style="color:red!important">inline</p>')
        self.assertEqual(paint_of(out, "inline")[0], (255, 0, 0))

    def test_combinators_and_structural_pseudo_classes(self):
        out = render('<style>div > p:nth-child(2){color:red}'
                     'p + p{font-style:italic}p ~ p:last-child{font-weight:bold}'
                     'div p:not(:first-child){text-decoration:underline}</style>\n\n'
                     '<div><p>first</p><p>second</p><p>third</p></div>')
        self.assertEqual(paint_of(out, "second")[0], (255, 0, 0))
        self.assertIn(3, paint_of(out, "second")[2])
        self.assertIn(4, paint_of(out, "second")[2])
        self.assertIn(1, paint_of(out, "third")[2])
        self.assertNotIn(4, paint_of(out, "first")[2])

    def test_attribute_selectors(self):
        for selector, value in [('[data-x]', ''), ('[data-x="yes"]', 'yes'),
                                ('[data-x~="yes"]', 'no yes maybe'),
                                ('[data-x|="yes"]', 'yes-en'),
                                ('[data-x^="yes"]', 'yes-more'),
                                ('[data-x$="yes"]', 'more-yes'),
                                ('[data-x*="yes"]', 'more-yes-more')]:
            with self.subTest(selector=selector):
                out = render(f'<style>{selector}{{color:red}}</style>\n\n'
                             f'<span data-x="{value}">match</span>')
                self.assertEqual(paint_of(out, "match")[0], (255, 0, 0))

    def test_namespace_selectors(self):
        out = render('<style>@namespace h "http://www.w3.org/1999/xhtml";'
                     'h|span{color:red}</style>\n\n<span>match</span>')
        self.assertEqual(paint_of(out, "match")[0], (255, 0, 0))

    def test_class_and_id_case_sensitivity(self):
        out = render('<style>.Note{color:red}#Target{font-style:italic}</style>\n\n'
                     '<span class="Note extra" id="Target">match</span>'
                     '<span class="note" id="target">miss</span>')
        self.assertEqual(paint_of(out, "match")[0], (255, 0, 0))
        self.assertIn(3, paint_of(out, "match")[2])
        self.assertEqual(paint_of(out, "miss")[0], None)
        self.assertNotIn(3, paint_of(out, "miss")[2])

    def test_lang_and_link_pseudo_classes(self):
        out = render('<style>:lang(en){color:red}a:link{font-weight:bold}'
                     'a:visited,a:hover,a:active,a:focus{color:lime}</style>\n\n'
                     '<div lang="en-GB"><span>english</span></div>'
                     '<a href="/path">link</a>')
        self.assertEqual(paint_of(out, "english")[0], (255, 0, 0))
        self.assertIn(1, paint_of(out, "link")[2])
        self.assertNotEqual(paint_of(out, "link")[0], (0, 255, 0))

    def test_form_pseudo_classes(self):
        out = render('<style>input:checked + span{color:red}'
                     'input:disabled + span{font-style:italic}</style>\n\n'
                     '<div><input type="checkbox" checked disabled><span>match</span></div>')
        self.assertEqual(paint_of(out, "match")[0], (255, 0, 0))
        self.assertIn(3, paint_of(out, "match")[2])

    def test_all_ansi_text_decorations_and_font_shorthand(self):
        out = render('<span style="font:bold italic 16px monospace;'
                     'text-decoration:underline overline line-through blink">text</span>')
        self.assertTrue({1, 3, 4, 9, 53, 5} <= paint_of(out, "text")[2])
        out = render('<span style="font-weight:200">light</span>'
                     '<strong style="font-weight:normal">normal</strong>'
                     '<a href="/" style="text-decoration:none">link</a>')
        self.assertIn(2, paint_of(out, "light")[2])
        self.assertNotIn(1, paint_of(out, "normal")[2])
        self.assertNotIn(4, paint_of(out, "link")[2])

    def test_display_none_and_visibility(self):
        out = render('<div><span style="display:none">absent</span>'
                     '<span style="visibility:hidden">abc'
                     '<span style="visibility:visible">visible</span>def</span></div>', '--no-ansi')
        self.assertEqual(out.strip('\n'), '   visible   ')
        out = render('<span hidden style="display:inline">shown</span>', '--no-ansi')
        self.assertEqual(out.strip(), 'shown')

    def test_whitespace(self):
        out = render('<span style="white-space:pre">a  b\nc</span>', '--no-ansi')
        self.assertEqual(out.strip('\n'), 'a  b\nc')
        out = render('<span style="white-space:pre-line">a  b\nc</span>', '--no-ansi')
        self.assertEqual(out.strip('\n'), 'a b\nc')
        out = render('<span style="white-space:nowrap">one two three</span>', '--no-ansi', '--width', '5')
        self.assertEqual(out.strip('\n'), 'one two three')
        out = render('<span style="white-space:pre-wrap">abcdefgh</span>', '--no-ansi', '--width', '4')
        self.assertEqual(out.strip('\n'), 'abcd\nefgh')

    def test_text_transform(self):
        out = render('<span style="text-transform:uppercase">Café</span> '
                     '<span style="text-transform:lowercase">HELLO</span> '
                     '<span style="text-transform:capitalize">some words</span>', '--no-ansi')
        self.assertEqual(out.strip(), 'CAFÉ hello Some Words')

    def test_list_markers(self):
        out = render('<ol style="list-style-type:upper-roman"><li>one</li><li>two</li></ol>', '--no-ansi')
        self.assertEqual(out.strip(), 'I. one\nII. two')
        out = render('<ul style="list-style-type:none"><li>one</li><li>two</li></ul>', '--no-ansi')
        self.assertEqual(out.strip(), 'one\ntwo')

    def test_generated_content_attributes_and_quotes(self):
        out = render('<style>p::before{content:"Before: ";color:red}'
                     'p::after{content:attr(data-end)}</style>\n\n'
                     '<p data-end="!">text</p>')
        self.assertEqual(plain(out).strip(), 'Before: text!')
        self.assertEqual(paint_of(out, "Before")[0], (255, 0, 0))
        out = render('<style>q{quotes:"«" "»"}'
                     'q::before{content:open-quote}q::after{content:close-quote}</style>\n\n'
                     '<q>quoted</q>', '--no-ansi')
        self.assertEqual(out.strip(), '«quoted»')

    def test_first_line_style_stops_at_wrap(self):
        out = render('<style>p::first-line{color:red}</style>\n\n<p>one two three</p>', '--width', '8')
        self.assertEqual(paint_of(out, "one")[0], (255, 0, 0))
        self.assertEqual(paint_of(out, "three")[0], None)

    def test_media_queries(self):
        source = ('<style>@media screen and (min-width:600px){p{color:red}}'
                  '@media print{p{color:blue}}</style>\n\n<p>text</p>')
        self.assertEqual(paint_of(render(source), "text")[0], (255, 0, 0))
        self.assertEqual(paint_of(render(source, '--width', '40'), "text")[0], None)

    def test_local_stylesheet_and_relative_import(self):
        with tempfile.TemporaryDirectory(prefix='m2t css ') as directory:
            root = Path(directory)
            (root / 'styles').mkdir()
            (root / 'styles' / 'child.css').write_text('p{color:red}')
            (root / 'styles' / 'main.css').write_text('@import "child.css";p{font-style:italic}')
            path = root / 'input.md'
            path.write_text('<link rel="StyleSheet" href="styles/main.css">\n\n<p>text</p>')
            out = render('', str(path))
            self.assertEqual(paint_of(out, "text")[0], (255, 0, 0))
            self.assertIn(3, paint_of(out, "text")[2])

    def test_import_cycle_and_missing_import(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cycle.css').write_text('@import "cycle.css";@import "missing.css";p{color:red}')
            path = root / 'input.md'
            path.write_text('<link rel="stylesheet" href="cycle.css">\n\n<p>text</p>')
            self.assertEqual(paint_of(render('', str(path)), "text")[0], (255, 0, 0))

    def test_non_css_stylesheets_are_ignored(self):
        out = render('<style type="text/plain">p{color:red}</style>\n\n<p>text</p>')
        self.assertEqual(paint_of(out, "text")[0], None)

    def test_tables_keep_colors_background_and_alignment(self):
        source = ('<style>tr{color:red;background:blue}td{text-align:right}'
                  'td span{color:lime}</style>\n\n'
                  '<table><tr><td><b>longer</b></td></tr>'
                  '<tr><td><span>short</span></td></tr></table>')
        out = render(source)
        self.assertEqual(paint_of(out, "longer")[:2], ((255, 0, 0), (0, 0, 255)))
        self.assertEqual(paint_of(out, "short")[:2], ((0, 255, 0), (0, 0, 255)))
        self.assertIn('│  short │', plain(out))
        self.assertEqual(plain(out), render(source, '--no-ansi'))
        # Cell backgrounds paint alignment padding as well as the text itself.
        self.assertEqual(paint_of(out, '  short')[1], (0, 0, 255))

    def test_css_example(self):
        path = Path(__file__).resolve().parent.parent / 'examples' / 'css-html5.md'
        out = render('', str(path))
        self.assertEqual(paint_of(out, 'Named CSS colour')[0], (255, 99, 71))
        self.assertEqual(paint_of(out, 'a gold child')[:2], ((255, 215, 0), (36, 52, 71)))
        self.assertIn('THIS TEXT BECOMES UPPERCASE.', plain(out))
        self.assertIn('Note: The label is generated by CSS.', plain(out))
        self.assertIn('III. Third item', plain(out))
        self.assertNotIn('This paragraph should not appear.', plain(out))
        self.assertEqual(plain(out), render('', '--no-ansi', str(path)))

    def test_malformed_and_browser_only_css_keep_text(self):
        out = render('<style>p{bogus:bad;position:absolute;left:10px;'
                     'font-family:serif;float:right;color:red;bad}</style>\n\n<p>text</p>')
        self.assertEqual(plain(out).strip(), 'text')
        self.assertEqual(paint_of(out, "text")[0], (255, 0, 0))

    def test_code_protects_css_and_no_raw_controls_are_emitted(self):
        out = render('```html\n<style>p{color:red}</style>\n```\n\nparagraph')
        self.assertEqual(paint_of(out, "paragraph")[0], None)
        out = render('<style>span::before{content:"\\1b [31m"}</style>\n\n<span>text</span>')
        self.assertNotIn('\x1b', plain(out))
        self.assertIn('text', plain(out))


if __name__ == '__main__':
    unittest.main()

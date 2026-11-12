#!/usr/bin/env python3
"""Check KGP payloads and emulate terminal replies through a controlling PTY."""
import base64
import contextlib
import fcntl
import http.server
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest
import zlib

BINARY = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else './m2t')
PACKET = re.compile(rb'\x1b_G([^;]*);([A-Za-z0-9+/=]*)\x1b\\')
QUERY = re.compile(rb'\x1b_Gi=(\d+),[^;]*a=q[^;]*;AAAA')
RELATIVE_QUERY = re.compile(rb'\x1b_Ga=p,i=(\d+),p=1,P=\d+,Q=2,')
ACTUAL = re.compile(rb'\x1b_Ga=T,[^;]*z=2[^;]*;')


def png(width=2, height=1, color=(255, 0, 0, 255), pixels=None):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    if pixels is None:
        pixels = bytes(color) * width * height
    pixels = b''.join(b'\0' + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def data_image(data=None, mime='png'):
    return 'data:image/' + mime + ';base64,' + base64.b64encode(data if data is not None else png()).decode()


def environment(tmux=False):
    env = os.environ.copy()
    env.pop('TMUX', None)
    if tmux:
        env['TMUX'] = 'test-session'
    return env


def run(source, *args, tmux=False):
    p = subprocess.run([BINARY, *args], input=source.encode(), capture_output=True,
                       timeout=15, env=environment(tmux))
    if p.returncode or p.stderr:
        raise AssertionError((p.returncode, p.stderr.decode(errors='replace')))
    return p.stdout


def unwrap_tmux(data):
    result = bytearray()
    i = 0
    while i < len(data):
        if not data.startswith(b'\x1bPtmux;', i):
            result.append(data[i])
            i += 1
            continue
        i += 7
        while i < len(data):
            if data.startswith(b'\x1b\x1b', i):
                result.append(27)
                i += 2
            elif data.startswith(b'\x1b\\', i):
                i += 2
                break
            else:
                result.append(data[i])
                i += 1
    return bytes(result)


def transmitted(data, tmux=False):
    if tmux:
        data = unwrap_tmux(data)
    packets = []
    actual_image = False
    for control, payload in PACKET.findall(data):
        fields = dict(field.split(b'=', 1) for field in control.split(b','))
        if b'a' in fields:
            actual_image = fields[b'a'] == b'T' and fields.get(b'z') == b'2'
        if actual_image:
            packets.append((fields, payload))
    return packets


def terminal(source, *args, support=True, tmux=False, interrupt=False,
             geometry=(24, 80, 640, 384), relative_support=True, background=(0, 0, 0)):
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', *geometry))
    saved = termios.tcgetattr(slave)

    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(1, termios.TIOCSCTTY, 0)

    p = subprocess.Popen([BINARY, *args], stdin=subprocess.PIPE, stdout=slave,
                         stderr=subprocess.PIPE, env=environment(tmux),
                         preexec_fn=controlling_terminal)
    output = bytearray()
    replied = False
    deadline = time.monotonic() + 10
    try:
        p.stdin.write(source.encode())
        p.stdin.close()
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], .05)
            if ready:
                output.extend(os.read(master, 65536))
                decoded = unwrap_tmux(bytes(output)) if tmux else output
                found = QUERY.search(decoded)
                if found and not replied:
                    replied = True
                    if interrupt:
                        os.kill(p.pid, signal.SIGTERM)
                    elif support is not None:
                        reply = (b'\x1b_Gi=' + found[1] + b';OK\x1b\\') if support else b''
                        relative = RELATIVE_QUERY.search(decoded)
                        if support and relative:
                            status = b'ENOPARENT:missing parent' if relative_support else b'OK'
                            reply += b'\x1b_Gi=' + relative[1] + b',p=1;' + status + b'\x1b\\'
                        color = '/'.join(f'{channel * 257:04x}' for channel in background)
                        reply += f'\x1b]11;rgb:{color}\x1b\\'.encode()
                        rows, columns, pixels_x, pixels_y = geometry
                        reply += f'\x1b[6;{pixels_y // rows};{pixels_x // columns}t'.encode()
                        os.write(master, reply + b'\x1b[?1;2c')
            elif p.poll() is not None:
                break
        p.wait(timeout=2)
        errors = p.stderr.read()
        if errors:
            raise AssertionError(errors.decode(errors='replace'))
        if not interrupt and p.returncode:
            raise AssertionError(p.returncode)
        if termios.tcgetattr(slave) != saved:
            raise AssertionError('terminal attributes were not restored')
        return bytes(output), p.returncode
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
        p.stderr.close()
        os.close(master)
        os.close(slave)


@contextlib.contextmanager
def image_server(callback):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            callback(self)

        def log_message(self, *args):
            pass

    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield f'http://127.0.0.1:{server.server_port}'
        finally:
            server.shutdown()
            thread.join()


def serve_png(request):
    try:
        request.send_response(200)
        request.send_header('Content-Type', 'image/png')
        request.end_headers()
        request.wfile.write(png())
    except (BrokenPipeError, ConnectionResetError):
        pass  # Expected when a timeout test has already closed the connection.


def start_stream(source, *args):
    process = subprocess.Popen([BINARY, '--images=always', *args], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment())
    process.stdin.write(source.encode())
    process.stdin.close()
    process.stdin = None
    return process


def wait_output(process, output, predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate(bytes(output)):
            return
        ready, _, _ = select.select([process.stdout], [], [], .05)
        if ready:
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                break
            output.extend(chunk)
    raise AssertionError('expected output did not arrive before the image completed')


def finish_stream(process, output):
    remaining, errors = process.communicate(timeout=5)
    output.extend(remaining)
    if process.returncode or errors:
        raise AssertionError((process.returncode, errors.decode(errors='replace')))


def stop_stream(process):
    if process.poll() is None:
        process.kill()
        process.communicate()


class ImageTests(unittest.TestCase):
    def test_raw_rgba_payload_and_alpha(self):
        out = run(f'![alt]({data_image(png(color=(10, 20, 30, 128)))}) after', '--images=always')
        packets = transmitted(out)
        self.assertEqual(len(packets), 1)
        fields, payload = packets[0]
        self.assertEqual(fields[b'a'], b'T')
        self.assertEqual(fields[b'f'], b'32')
        self.assertEqual(fields[b's'], b'2')
        self.assertEqual(fields[b'v'], b'1')
        self.assertEqual(fields[b'C'], b'1')
        self.assertEqual(fields[b'q'], b'2')
        self.assertEqual(base64.b64decode(payload), bytes([10, 20, 30, 128]) * 2)
        self.assertIn(b'after', out)
        self.assertNotIn(b'[image:', out)

    def test_chunk_size_and_continuations(self):
        out = run(f'![alt]({data_image(png(100, 100))})', '--images', 'always')
        packets = transmitted(out)
        self.assertGreater(len(packets), 1)
        for fields, payload in packets[:-1]:
            self.assertEqual(fields[b'm'], b'1')
            self.assertEqual(len(payload), 4096)
        self.assertEqual(packets[-1][0][b'm'], b'0')
        self.assertTrue(all(len(payload) <= 4096 for _, payload in packets))
        pixels = base64.b64decode(b''.join(payload for _, payload in packets))
        self.assertEqual(pixels, bytes([255, 0, 0, 255]) * 10000)

    def test_modes_and_plain_output_fallback(self):
        source = f'![alt]({data_image()})'
        for args in [(), ('--images=auto',), ('--images=never',), ('--no-images',),
                     ('--images=always', '--no-ansi')]:
            with self.subTest(args=args):
                out = run(source, *args)
                self.assertNotIn(b'\x1b_G', out)
                self.assertIn(b'[image: alt]', out)

    def test_missing_invalid_and_unsupported_images_fall_back(self):
        for src in ['/no/such/file.png', data_image(b'not an image'),
                    'data:image/png;base64,not-valid!!', 'data:image/png;base64,AA=A',
                    'data:image/svg+xml,%3Csvg%3E%3C/svg%3E', 'ftp://example.com/image.png']:
            with self.subTest(src=src):
                out = run(f'![fallback]({src})', '--images=always')
                self.assertFalse(transmitted(out))
                self.assertIn(b'fallback', out)

    def test_local_relative_paths_and_percent_encoding(self):
        with tempfile.TemporaryDirectory(prefix='m2t images ') as directory:
            root = Path(directory)
            (root / 'a picture.png').write_bytes(png())
            doc = root / 'input.md'
            doc.write_text('![local](a%20picture.png)')
            self.assertTrue(transmitted(run('', '--images=always', str(doc))))
            self.assertTrue(transmitted(run(f'![local]({(root / "a picture.png").as_uri()})', '--images=always')))

    def test_size_hints_and_css_pixels(self):
        src = data_image(png(20, 10))
        for attributes, width, height in [('width="10"', 10, 5),
                                          ('width="10" style="width:6px"', 6, 3),
                                          ('height="4"', 8, 4)]:
            with self.subTest(attributes=attributes):
                fields = transmitted(run(f'<img src="{src}" alt="alt" {attributes}>', '--images=always'))[0][0]
                self.assertEqual((int(fields[b's']), int(fields[b'v'])), (width, height))

    def test_jpeg_gif_and_webp_decoding(self):
        fixtures = Path(__file__).resolve().parent / 'fixtures'
        for name in ['red.jpg', 'red.gif', 'red.webp']:
            with self.subTest(name=name):
                out = run(f'![alt]({data_image((fixtures / name).read_bytes())})', '--images=always')
                fields, payload = transmitted(out)[0]
                self.assertEqual((fields[b's'], fields[b'v']), (b'2', b'1'))
                pixels = base64.b64decode(payload)
                self.assertEqual(len(pixels), 8)
                self.assertGreaterEqual(pixels[0], 250)
                self.assertLessEqual(pixels[1], 5)
                self.assertLessEqual(pixels[2], 5)
                self.assertEqual(pixels[3], 255)

    def test_resizing_uses_linear_light_and_premultiplied_alpha(self):
        for second, expected in [(bytes([0, 0, 255, 255]), bytes([188, 0, 188, 255])),
                                 (bytes([0, 0, 255, 0]), bytes([255, 0, 0, 128]))]:
            with self.subTest(second=second):
                source = png(pixels=bytes([255, 0, 0, 255]) + second)
                out = run(f'<img src="{data_image(source)}" width="1">', '--images=always')
                fields, payload = transmitted(out)[0]
                self.assertEqual((fields[b's'], fields[b'v']), (b'1', b'1'))
                self.assertEqual(base64.b64decode(payload), expected)

    def test_tables_and_hidden_images_keep_text_fallback(self):
        src = data_image()
        out = run(f'<table><tr><td><img src="{src}" alt="table"></td></tr></table>', '--images=always')
        self.assertFalse(transmitted(out))
        self.assertIn(b'[image: table]', out)
        out = run(f'<img src="{src}" style="display:none">', '--images=always')
        self.assertEqual(out, b'')
        out = run(f'<img src="{src}" alt="hidden" style="visibility:hidden">', '--images=always')
        self.assertFalse(transmitted(out))
        self.assertNotIn(b'hidden', out)

    def test_tmux_passthrough(self):
        out = run(f'![alt]({data_image()})', '--images=always', tmux=True)
        self.assertIn(b'\x1bPtmux;', out)
        self.assertTrue(transmitted(out, tmux=True))

    def test_supported_terminal_detection_and_state_restore(self):
        out, _ = terminal(f'![alt]({data_image()}) after')
        self.assertTrue(QUERY.search(out))
        self.assertTrue(transmitted(out))
        self.assertIn(b'after', out)

    def test_unsupported_terminal_and_timeout(self):
        for support in [False, None]:
            with self.subTest(support=support):
                out, _ = terminal(f'![alt]({data_image()})', support=support)
                self.assertTrue(QUERY.search(out))
                self.assertFalse(transmitted(out))
                self.assertIn(b'[image: alt]', out)

    def test_basic_kgp_without_relative_placements_keeps_alt(self):
        out, _ = terminal(f'![alt]({data_image()})', relative_support=False)
        self.assertFalse(transmitted(out))
        self.assertIn(b'[image: alt]', out)

    def test_terminal_background_covers_placeholder_without_changing_alpha(self):
        color = (240, 245, 250)
        out, _ = terminal(f'![alt]({data_image(png(color=(10, 20, 30, 128)))})', background=color)
        masks = []
        for control, payload in PACKET.findall(out):
            fields = dict(field.split(b'=', 1) for field in control.split(b','))
            if fields.get(b'z') == b'1':
                masks.append((fields, base64.b64decode(payload)))
        self.assertEqual(len(masks), 1)
        fields, pixels = masks[0]
        self.assertEqual((int(fields[b's']), int(fields[b'v'])), (5, 1))
        self.assertEqual(pixels, bytes((*color, 255)) * 5)
        self.assertEqual(base64.b64decode(transmitted(out)[0][1]), bytes([10, 20, 30, 128]) * 2)

    def test_generic_placeholder_without_alt(self):
        for attributes in ['', 'alt=""', 'alt="   "']:
            with self.subTest(attributes=attributes):
                source = f'<img src="{data_image()}" {attributes}>'
                out = run(source, '--images=always')
                self.assertIn(b'[image]', out[:ACTUAL.search(out).start()])
                self.assertEqual(run(source, '--no-images').strip(), b'[image]')

    def test_rest_is_rendered_before_a_delayed_image(self):
        release = threading.Event()
        started = threading.Event()

        def delayed(request):
            started.set()
            release.wait(5)
            serve_png(request)

        with image_server(delayed) as url:
            process = start_stream(f'![Descriptive alt]({url}/slow)\n\nREST_RENDERED_MARKER')
            output = bytearray()
            try:
                wait_output(process, output, lambda data: b'REST_RENDERED_MARKER' in data)
                self.assertIn(b'Descriptive alt', output)
                self.assertFalse(ACTUAL.search(output))
                self.assertIsNone(process.poll())
                self.assertTrue(started.wait(1))
                release.set()
                finish_stream(process, output)
                self.assertLess(output.index(b'REST_RENDERED_MARKER'), ACTUAL.search(output).start())
                # Deferred completion uses KGP-relative placement, never cursor
                # rewinds which would overwrite later text or the shell prompt.
                tail = output[output.index(b'REST_RENDERED_MARKER'):]
                self.assertFalse(re.search(rb'\x1b\[[0-9]+[ABGHf]', tail))
                self.assertIn(b'P', transmitted(output)[0][0])
            finally:
                release.set()
                stop_stream(process)

    def test_fast_image_is_published_while_an_earlier_image_is_pending(self):
        release = threading.Event()

        def varied(request):
            if request.path == '/slow':
                release.wait(5)
            serve_png(request)

        with image_server(varied) as url:
            process = start_stream(f'![Slow alt]({url}/slow)\n\n![Fast alt]({url}/fast)\n\nREST_RENDERED_MARKER')
            output = bytearray()
            try:
                wait_output(process, output, lambda data: bool(transmitted(data)))
                self.assertIn(b'REST_RENDERED_MARKER', output)
                self.assertIsNone(process.poll())
                anchors = []
                for control, _ in PACKET.findall(output):
                    fields = dict(field.split(b'=', 1) for field in control.split(b','))
                    if fields.get(b'z') == b'-1':
                        anchors.append(fields[b'i'])
                self.assertEqual(len(anchors), 2)
                self.assertEqual(transmitted(output)[0][0][b'P'], anchors[1])
                release.set()
                finish_stream(process, output)
                placed = [fields for fields, _ in transmitted(output) if fields.get(b'a') == b'T']
                self.assertEqual([fields[b'P'] for fields in placed], [anchors[1], anchors[0]])
            finally:
                release.set()
                stop_stream(process)

    def test_load_timeout_keeps_alt_and_does_not_delay_text(self):
        release = threading.Event()

        def delayed(request):
            release.wait(5)
            serve_png(request)

        with image_server(delayed) as url:
            begin = time.monotonic()
            process = start_stream(f'![Timeout alt]({url}/slow)\n\nREST_RENDERED_MARKER', '--image-timeout', '1')
            output = bytearray()
            try:
                wait_output(process, output, lambda data: b'REST_RENDERED_MARKER' in data)
                self.assertIn(b'Timeout alt', output)
                finish_stream(process, output)
                self.assertFalse(transmitted(output))
                self.assertLess(time.monotonic() - begin, 4)
            finally:
                release.set()
                stop_stream(process)

    def test_queue_larger_than_the_worker_pool_and_alt_clipping(self):
        source = '\n\n'.join(f'![alt {n}]({data_image()})' for n in range(12))
        out = run(source + '\n\nREST_RENDERED_MARKER', '--images=always')
        images = [fields for fields, _ in transmitted(out) if fields.get(b'a') == b'T']
        self.assertEqual(len(images), 12)
        self.assertEqual(len({fields[b'P'] for fields in images}), 12)
        self.assertLess(out.index(b'REST_RENDERED_MARKER'), ACTUAL.search(out).start())
        out = run(f'![{"x" * 100}]({data_image()})\n\nREST', '--images=always', '--width', '10')
        self.assertIn(('x' * 9 + '…').encode(), out[:ACTUAL.search(out).start()])

    def test_signal_during_probe_restores_terminal(self):
        out, code = terminal(f'![alt]({data_image()})', interrupt=True)
        self.assertTrue(QUERY.search(out))
        self.assertEqual(code, -signal.SIGTERM)

    def test_tmux_probe_and_detection(self):
        out, _ = terminal(f'![alt]({data_image()})', tmux=True)
        self.assertTrue(transmitted(out, tmux=True))

    def test_terminal_geometry_and_quote_margin(self):
        out, _ = terminal(f'> ![alt]({data_image(png(1000, 400))})\n\nAfter',
                          '--width', '80', geometry=(12, 40, 400, 240))
        fields = transmitted(out)[0][0]
        self.assertLessEqual(int(fields[b'c']), 38)
        self.assertLessEqual(int(fields[b'r']), 10)
        self.assertLessEqual(int(fields[b's']), 380)
        self.assertIn('│ '.encode(), out)
        self.assertIn(b'After', out)

    def test_http_fetch_redirect_and_fallback(self):
        seen = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.path)
                if self.path == '/redirect':
                    self.send_response(302)
                    self.send_header('Location', '/image.png')
                    self.end_headers()
                elif self.path == '/image.png':
                    self.send_response(200)
                    self.send_header('Content-Type', 'image/png')
                    self.end_headers()
                    self.wfile.write(png())
                else:
                    self.send_response(404)
                    self.end_headers()

            def log_message(self, *args):
                pass

        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                url = f'http://127.0.0.1:{server.server_port}'
                self.assertTrue(transmitted(run(f'![remote]({url}/redirect)', '--images=always')))
                self.assertEqual(seen, ['/redirect', '/image.png'])
                out = run(f'![missing]({url}/missing)', '--images=always')
                self.assertIn(b'missing', out)
                seen.clear()
                run(f'![remote]({url}/image.png)')
                run(f'![remote]({url}/image.png)', '--images=always', '--no-ansi')
                self.assertEqual(seen, [])
            finally:
                server.shutdown()
                thread.join()

    def test_oversized_dimensions_are_rejected(self):
        header = struct.pack('>IIBBBBB', 32768, 32768, 8, 6, 0, 0, 0)
        kind = b'IHDR'
        data = b'\x89PNG\r\n\x1a\n' + struct.pack('>I', len(header)) + kind + header
        data += struct.pack('>I', zlib.crc32(kind + header) & 0xffffffff)
        out = run(f'![large]({data_image(data)})', '--images=always')
        self.assertFalse(transmitted(out))
        self.assertIn(b'large', out)

    def test_bad_options(self):
        for args in [('--images',), ('--images=',), ('--images=invalid',),
                     ('--image-timeout',), ('--image-timeout=0',), ('--image-timeout=-1',),
                     ('--image-timeout=invalid',), ('--image-timeout=3601',)]:
            p = subprocess.run([BINARY, *args], input=b'', capture_output=True)
            self.assertEqual(p.returncode, 2)


if __name__ == '__main__':
    unittest.main()

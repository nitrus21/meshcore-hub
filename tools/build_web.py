#!/usr/bin/env python3
"""
Builds the web app for LittleFS.

  web/  ->  data/www/<file>.gz   (light minification + gzip, no CDN, no npm needed)
           index.html embeds the CSS, the scripts and the translations: the browser loads the
           whole app with ONE request (each parallel connection costs RAM on the ESP32).
        ->  dist/webapp.pack     (OTA update of the web app that keeps the history)
  also generates web/icon-192.png and web/icon-512.png when missing.

Usage: python tools/build_web.py      (also run automatically by `pio run -t buildfs`)
"""
import gzip
import io
import json
import math
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'web')
OUT = os.path.join(ROOT, 'data', 'www')
DIST = os.path.join(ROOT, 'dist')
SKIP = {'LICENSE'}


def minify_css(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'\s+', ' ', s)
    s = re.sub(r'\s*([{};,>])\s*', r'\1', s)
    return s.replace(';}', '}').strip()


def minify_js(s):
    # Safe, conservative: drop whole-line comments, indentation and blank lines.
    # Template literals stay valid (only their leading indentation changes).
    out = []
    for line in s.splitlines():
        st = line.strip()
        if not st or (st.startswith('//') and not st.startswith('//#')):
            continue
        if st.startswith('//# sourceMappingURL'):
            continue
        out.append(st)
    return '\n'.join(out) + '\n'


def minify_json(s):
    return json.dumps(json.loads(s), ensure_ascii=False, separators=(',', ':'))


def minify_html(s):
    return re.sub(r'>\s+<', '><', re.sub(r'<!--.*?-->', '', s, flags=re.S)).strip()


# ------------------------------------------------------------------ PNG icons (pure Python)
def make_icon(size):
    """Rasterises the logo (rounded square, mesh lines, nodes) with 3x3 supersampling."""
    acc = (0x2f, 0x6f, 0xed)
    k = size / 48.0
    lines = [((24, 24), (13, 14)), ((24, 24), (36, 16)), ((24, 24), (21, 37)), ((13, 14), (36, 16)), ((21, 37), (36, 16))]
    nodes = [((24, 24), 5), ((13, 14), 3.4), ((36, 16), 3.4), ((21, 37), 3.4)]

    def seg_d(px, py, a, b):
        ax, ay = a; bx, by = b
        dx, dy = bx - ax, by - ay
        t = max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - ax - t * dx, py - ay - t * dy)

    def sample(x, y):
        # rounded square r=13 (maskable icons get the full square)
        r = 13
        qx, qy = max(abs(x - 24) - (24 - r), 0), max(abs(y - 24) - (24 - r), 0)
        if math.hypot(qx, qy) > r:
            return None
        for c, rad in nodes:
            if math.hypot(x - c[0], y - c[1]) <= rad:
                return (255, 255, 255)
        for a, b in lines:
            if seg_d(x, y, a, b) <= 1.2:
                return (255, 255, 255)
        return acc

    rows = []
    ss = 3
    for j in range(size):
        row = bytearray([0])
        for i in range(size):
            rr = gg = bb = aa = 0
            for sj in range(ss):
                for si in range(ss):
                    c = sample((i + (si + .5) / ss) / k, (j + (sj + .5) / ss) / k)
                    if c:
                        rr += c[0]; gg += c[1]; bb += c[2]; aa += 255
            n = ss * ss
            a = aa // n
            row += bytes([rr // max(1, aa // 255), gg // max(1, aa // 255), bb // max(1, aa // 255), a]) if aa else b'\0\0\0\0'
        rows.append(bytes(row))
    raw = b''.join(rows)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


BUNDLED_JS = ['icons.js', 'vendor/qrcode.js', 'app.js', 'pages.js', 'wizard.js']
BUNDLED_CSS = ['app.css']


def bundle(processed):
    """Inlines CSS, scripts and translations into index.html and drops the separate files."""
    html = processed['index.html'].decode('utf-8')
    for css in BUNDLED_CSS:
        tag = re.search(r'<link[^>]*href="%s"[^>]*>' % re.escape(css), html)
        if not tag:
            sys.exit(f'build_web: {css} not referenced in index.html')
        html = html.replace(tag.group(0), '<style>' + processed.pop(css).decode('utf-8') + '</style>')
    langs = {}
    for rel in list(processed):
        m = re.fullmatch(r'i18n/([a-z]{2})\.json', rel)
        if m:
            langs[m.group(1)] = json.loads(processed[rel].decode('utf-8'))  # files kept for other tools
    i18n = '<script>window.I18N=' + json.dumps(langs, ensure_ascii=False, separators=(',', ':')) + '</script>'
    first = True
    for js in BUNDLED_JS:
        tag = re.search(r'<script src="%s"></script>' % re.escape(js), html)
        if not tag:
            sys.exit(f'build_web: {js} not referenced in index.html')
        code = processed.pop(js).decode('utf-8').replace('</', '<\/')  # never close the <script> early
        html = html.replace(tag.group(0), (i18n.replace('</', '<\/').replace('<\/script>', '</script>') if first else '') + '<script>' + code + '</script>')
        first = False
    processed['index.html'] = html.encode('utf-8')


def main():
    for sz in (192, 512):
        p = os.path.join(SRC, f'icon-{sz}.png')
        if not os.path.exists(p):
            with open(p, 'wb') as f:
                f.write(make_icon(sz))
            print(f'  generated {os.path.relpath(p, ROOT)}')

    # clean output
    if os.path.isdir(OUT):
        for dp, dns, fns in os.walk(OUT, topdown=False):
            for fn in fns:
                os.remove(os.path.join(dp, fn))
            for dn in dns:
                os.rmdir(os.path.join(dp, dn))
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(DIST, exist_ok=True)

    processed = {}  # rel -> minified bytes
    total_in = 0
    for dp, _, fns in os.walk(SRC):
        for fn in sorted(fns):
            if fn in SKIP or fn.endswith('.map'):
                continue
            src = os.path.join(dp, fn)
            rel = os.path.relpath(src, SRC).replace(os.sep, '/')
            data = open(src, 'rb').read()
            total_in += len(data)
            ext = fn.rsplit('.', 1)[-1].lower()
            try:
                if ext == 'css':
                    data = minify_css(data.decode('utf-8')).encode('utf-8')
                elif ext == 'js':
                    data = minify_js(data.decode('utf-8')).encode('utf-8')
                elif ext == 'json':
                    data = minify_json(data.decode('utf-8')).encode('utf-8')
                elif ext == 'html':
                    data = minify_html(data.decode('utf-8')).encode('utf-8')
            except Exception as e:  # never ship a broken file silently
                sys.exit(f'build_web: cannot process {rel}: {e}')
            processed[rel] = data

    bundle(processed)

    files = []
    total_out = 0
    for rel, data in sorted(processed.items()):
        buf = io.BytesIO()
        with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as gz:
            gz.write(data)
        gzd = buf.getvalue()
        dst = os.path.join(OUT, rel + '.gz')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(gzd)
        files.append((rel + '.gz', gzd))
        total_out += len(gzd)

    # webapp.pack: "MCHPACK1\n" then for each file "path\nsize\n" + bytes (see src/web/Web.cpp)
    with open(os.path.join(DIST, 'webapp.pack'), 'wb') as f:
        f.write(b'MCHPACK1\n')
        for rel, d in files:
            f.write(rel.encode() + b'\n' + str(len(d)).encode() + b'\n' + d)
    print(f'  web app: {len(files)} files, {total_in // 1024} kB -> {total_out // 1024} kB gzip  (data/www, dist/webapp.pack)')


if __name__ == '__main__':
    main()

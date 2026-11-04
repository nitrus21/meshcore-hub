#!/usr/bin/env python3
"""Package the Web Flasher images (flasher/firmware/) and manifests from PlatformIO builds.

    python tools/make_release.py              # package what is already built
    python tools/make_release.py --build      # build firmware + LittleFS first

For each board the images are kept apart (bootloader, partition table, boot_app0, app, LittleFS)
and flasher/manifest-<board>.json lists them at their own offsets. They are not merged into one
image at 0: the gaps would be filled with 0xFF and wipe the nvs partition (settings) on every
install. LittleFS (web app) is part of the install, so a browser install always resets the history.
ESP-IDF refuses project paths with spaces when it rebuilds its libraries (custom_sdkconfig of
t-display-s3). A directory junction is not enough (PlatformIO resolves it to the real path):
build a copy in a folder without spaces, e.g. robocopy <project> C:\\mchub /MIR /XD .pio .git
"""
import argparse
import csv
import glob
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'flasher', 'firmware')
# env -> (esp-web-tools chip family, bootloader offset, partition table, manifest, manifest name)
BOARDS = {
    't-display-s3': ('ESP32-S3', 0x0, 'partitions/16mb_ota_fs.csv', 'manifest-t-display-s3.json',
                     'MeshCore Hub (LilyGO T-Display-S3)'),
}


def pio(*args):
    env = dict(os.environ)
    env.pop('MSYSTEM', None)  # ESP-IDF tools refuse to run under MSYS / Git Bash
    subprocess.check_call(['pio', *args], cwd=ROOT, env=env)


def partition_offset(table, name):
    with open(os.path.join(ROOT, table), newline='') as f:
        for row in csv.reader(l for l in f if not l.lstrip().startswith('#')):
            if len(row) >= 5 and row[0].strip() == name:
                return int(row[3].strip(), 0)
    raise SystemExit(f'no {name} partition in {table}')


def boot_app0():
    # Same file in Arduino 2 and 3 (framework-arduinoespressif32 and framework-arduinoespressif32@x.y.z)
    pkgs = os.path.expanduser('~/.platformio/packages')
    hits = glob.glob(os.path.join(pkgs, 'framework-arduinoespressif32*', 'tools', 'partitions', 'boot_app0.bin'))
    if not hits:
        raise SystemExit('boot_app0.bin not found in ' + pkgs)
    return hits[0]


def hub_version():
    with open(os.path.join(ROOT, 'platformio.ini'), encoding='utf-8') as f:
        m = re.search(r'-DHUB_VERSION=\\"([^"\\]+)\\"', f.read())
    return m.group(1) if m else '0.0.0'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', action='store_true', help='build firmware and LittleFS before packaging')
    ap.add_argument('envs', nargs='*', default=list(BOARDS))
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    for env in a.envs:
        family, boot_off, table, manifest, name = BOARDS[env]
        if a.build:
            pio('run', '-e', env)
            pio('run', '-e', env, '-t', 'buildfs')
        b = os.path.join(ROOT, '.pio', 'build', env)
        images = [(boot_off, 'bootloader.bin'), (0x8000, 'partitions.bin'),
                  (partition_offset(table, 'otadata'), boot_app0()),
                  (partition_offset(table, 'app0'), 'firmware.bin'),
                  (partition_offset(table, 'littlefs'), 'littlefs.bin')]
        parts = []
        for off, src in images:
            path = src if os.path.isabs(src) else os.path.join(b, src)
            if not os.path.exists(path):
                raise SystemExit(f'{path} missing: build {env} first (--build)')
            dst = f'{env}-{os.path.basename(src)}'
            shutil.copyfile(path, os.path.join(OUT, dst))
            parts.append({'path': 'firmware/' + dst, 'offset': off})
        doc = {'name': name, 'version': hub_version(), 'new_install_prompt_erase': True,
               'builds': [{'chipFamily': family, 'parts': parts}]}
        with open(os.path.join(ROOT, 'flasher', manifest), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=2)
            f.write('\n')
        print(f'{env}: ' + ', '.join(f'{p["path"]} @ {hex(p["offset"])}' for p in parts))


if __name__ == '__main__':
    sys.exit(main())

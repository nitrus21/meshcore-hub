# PlatformIO hook: rebuild the web app (data/www) before building / uploading LittleFS.
import os
import subprocess
import sys

Import("env")  # noqa: F821  (provided by PlatformIO)

ROOT = env.subst("$PROJECT_DIR")  # noqa: F821


def build_web(*_args, **_kwargs):
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "build_web.py")])


env.AddPreAction("$BUILD_DIR/littlefs.bin", build_web)  # noqa: F821
env.AddPreAction("buildfs", build_web)  # noqa: F821
env.AddPreAction("uploadfs", build_web)  # noqa: F821

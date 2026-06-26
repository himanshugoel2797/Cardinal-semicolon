#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# Subset DejaVu Sans to the UI Latin range -> lisp/data/DejaVuSans-subset.ttf, the
# default TrueType UI font rendered at runtime by libs/ttf (stb_truetype). DejaVu is
# distributed under a free, embeddable license (Bitstream Vera + DejaVu changes);
# only a Latin subset is vendored to keep the initrd small (~26 KiB vs ~760 KiB).
#
#   pip install fonttools brotli && python3 scripts/gen-ttf.py
import subprocess, sys
SRC = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
OUT = "lisp/data/DejaVuSans-subset.ttf"
UNICODES = "U+0020-007E,U+00A0-00FF,U+2013-2014,U+2018-2019,U+201C-201D,U+2022,U+2026,U+20AC"
subprocess.run([sys.executable, "-m", "fontTools.subset", SRC,
                f"--output-file={OUT}", f"--unicodes={UNICODES}",
                "--no-hinting", "--desubroutinize"], check=True)
print(f"wrote {OUT}")

#!/usr/bin/env python3
"""Host test of the MAX31760 fan protocol core (official register sequence, curve, page lines)."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(dir=here.parents[6]) as temp:
    exe = Path(temp) / 'fan'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', str(here / 'fan.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

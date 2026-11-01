#!/usr/bin/env python3
"""Host tests of the actual bounded FocalTech and GENI I2C cores."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[6]
project = Path(os.environ.get('IDRAGON_PROJECT', root))
geni = project / 'firmware/UniXNU-enc3l/drivers/QcomGeniSpi'
with tempfile.TemporaryDirectory(dir=root) as temp:
    exe = Path(temp) / 'touch'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', '-I', str(geni), str(here / 'touch.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

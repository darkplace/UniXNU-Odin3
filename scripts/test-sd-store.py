#!/usr/bin/env python3
"""Compile the actual store functions, not a rewritten model of their policy."""
from pathlib import Path
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
driver = device / "drivers/OdinSd"
source = (driver / "OdinSd.cpp").read_text()
store = source[source.index("// ABI names retained for NativeRamBlock"):source.index("IOService *NativeUfs::probe")]
with tempfile.TemporaryDirectory(dir=device) as tmp:
    folder = Path(tmp)
    (folder / "StoreImplementation.h").write_text(store)
    command = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
               "-I", str(driver), "-I", str(device / "modules/Pagani/drivers/NativeRamBlock"),
               "-I", str(folder), str(driver / "test/store.cpp"), "-o", str(folder / "store")]
    subprocess.run(command, check=True)
    subprocess.run([str(folder / "store")], check=True)

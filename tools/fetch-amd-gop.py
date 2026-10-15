#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fetch the original AMD ARM64 GOP 1.68 and verify its pinned hash."""
import hashlib
import json
import os
from pathlib import Path
import sys
import urllib.request

FILES = {
    "Arm64Gop_1_68.efi": "22021e0a5a47cff0d48dd86161eb540a786604898b81da41c8b06983f762372f",
}
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
source = os.environ.get("AMD_GOP_DIR")
manifest = {}
for name, digest in FILES.items():
    path = output / name
    url = "https://download.amd.com/drivers/" + name
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
        data = path.read_bytes()
    elif source:
        data = (Path(source) / name).read_bytes()
    else:
        request = urllib.request.Request(url, headers={"User-Agent": "ruyifirmware"})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read(1024 * 1024)
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"Unexpected AMD GOP contents: {name}")
    path.write_bytes(data)
    manifest[name] = {"url": url, "sha256": digest, "bytes": len(data)}
(output / "amd-gop.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(output)

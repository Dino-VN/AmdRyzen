#!/usr/bin/env python3
"""Embed only the already-reviewed, hash-locked lossless CPC normalization.
Never derive addresses, evaluate AML, or silently update accepted hashes.
"""
import hashlib
import pathlib
import sys

EXPECTED = "d5fdb6087f9f0665835fa41ad2964608e7a1cb7befa738e4bb73477f8f0edcc9"


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: embed_profile.py static.cpc output.inc")
    source = pathlib.Path(sys.argv[1])
    data = source.read_bytes()
    if len(data) > 131072 or hashlib.sha256(data).hexdigest() != EXPECTED:
        raise SystemExit("refusing unreviewed normalized CPC profile")
    text = data.decode("ascii")
    lines = ["// Generated from the hash-locked lossless CPC fixture; do not edit.\n",
             "static const char kStaticCpc[] =\n"]
    for line in text.splitlines(keepends=True):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        lines.append('    "' + escaped + '"\n')
    lines.append(";\n")
    output = pathlib.Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(lines), encoding="ascii")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Reproduce this one derived fixture from explicit, hash-locked saved inputs.

Run with python3 -I. This is NOT an AML interpreter or a general ACPI normalizer.
No source paths embedded in JSON are opened. No captured scripts are executed.
"""
import sys

if not sys.flags.isolated:
    raise SystemExit("Use python3 -I with explicit data paths")

import argparse
import hashlib
import json
import pathlib
import plistlib
import struct

SOURCE_HASHES = {
    "plist": "d4377b37980199c35a5cfb08d6a5d85c42202effa749d2fe3b181596de74fdb5",
    "manifest": "809579e49e397561ef4bc7a7c67a556239dbefbfffb91b53e657fb13ed087fb1",
    "cpc": "d0da48d4af4d91fc37fc1cbb1da13f48c289053929402c20b8cfa7b2c2787cff",
}
TABLE_HASHES = {
    "PCCT": "5a31529d042b238e4a4dc3d8f7ccbe9cd1e2dbb3845f78caea31b244afcf50a0",
    "APIC": "d4879c22b99e84feaa67cd54ca2ea1bfaf16e951e42ecb5654936fb973ef4c23",
    "SSDT-2": "bdfb454245935efe3ef7174a9efa5324c742764fcf40a8632221fd27fcf189f3",
    "SSDT-3": "f22c5c2615bcd436bdd5601ee16d3e6269187018220ddf1e21b26efd73fe6013",
}
FIELDS = (
    "HighestPerformance", "NominalPerformance", "LowestNonlinearPerformance",
    "LowestPerformance", "GuaranteedPerformance", "DesiredPerformance",
    "MinimumPerformance", "MaximumPerformance", "PerformanceReductionTolerance",
    "TimeWindow", "CounterWraparoundTime", "ReferenceCounter", "DeliveredCounter",
    "PerformanceLimited", "CPPCEnable", "AutonomousSelectionEnable",
    "AutonomousActivityWindow", "EnergyPerformancePreference", "ReferencePerformance",
    "LowestFrequency", "NominalFrequency",
)
PCC_OFFSETS = {
    "HighestPerformance": 0, "NominalPerformance": 4,
    "LowestNonlinearPerformance": 8, "LowestPerformance": 12,
    "DesiredPerformance": 0x14, "MinimumPerformance": 0x18,
    "MaximumPerformance": 0x1c, "PerformanceLimited": 0x40,
    "CPPCEnable": 0x44, "AutonomousSelectionEnable": 0x48,
    "EnergyPerformancePreference": 0x50,
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def read_source(path, expected):
    # Generous bound for these 37--122 KB captures; no embedded paths followed.
    with path.open("rb") as stream:
        data = stream.read(1048577)
    require(len(data) <= 1048576, "source too large")
    require(digest(data) == expected, "source hash differs from verified capture")
    return data


def sanitized_header(raw):
    data = bytearray(raw)
    data[10:16] = b"LAB   "
    data[16:24] = b"DERIVED "
    struct.pack_into("<I", data, 24, 1)
    data[28:32] = b"LAB "
    struct.pack_into("<I", data, 32, 1)
    return data


def finish_table(data):
    data[9] = 0
    data[9] = (-sum(data)) & 255
    require(sum(data) % 256 == 0, "derived checksum")
    return bytes(data)


def hex_table(data, name):
    lines = [
        "# DERIVED/SANITIZED " + name + "; NOT an original firmware table.",
        "# Physical addresses and OEM metadata replaced; ACPI checksum recomputed.",
        "# Checksum detects corruption only; it is not authentication/signing.",
    ]
    lines.extend(" ".join(f"{byte:02x}" for byte in data[i:i + 16])
                 for i in range(0, len(data), 16))
    return ("\n".join(lines) + "\n").encode("ascii")


def generate(args):
    source_bytes = {
        "plist": read_source(args.tables_plist, SOURCE_HASHES["plist"]),
        "manifest": read_source(args.manifest_json, SOURCE_HASHES["manifest"]),
        "cpc": read_source(args.cpc_json, SOURCE_HASHES["cpc"]),
    }
    tables = plistlib.loads(source_bytes["plist"])
    manifest = json.loads(source_bytes["manifest"], object_pairs_hook=unique_object)
    cpc = json.loads(source_bytes["cpc"], object_pairs_hook=unique_object)
    provenance = {}
    for key, expected_hash in TABLE_HASHES.items():
        data = tables[key]
        require(type(data) is bytes and len(data) >= 36, "table buffer type/length")
        require(digest(data) == expected_hash, "raw table hash")
        require(len(data) == struct.unpack_from("<I", data, 4)[0], "raw table length")
        require(sum(data) % 256 == 0, "raw ACPI checksum")
        rows = [row for row in manifest["tables"] if row["property_key"] == key]
        require(len(rows) == 1 and rows[0]["sha256"] == expected_hash, "manifest table hash")
        require(rows[0]["header_length"] == len(data) and rows[0]["checksum_mod256"] == 0,
                "manifest length/checksum")
        provenance[key] = {"sha256": expected_hash, "bytes": len(data), "checksum_mod256": 0}
    require(cpc["sha256"] == TABLE_HASHES["SSDT-3"], "CPC source linkage")
    require(len(cpc["cpus"]) == 16, "CPU count")
    lines = [
        "# DERIVED static packages from the hash-locked saved post-boot capture.",
        "# No AML evaluation. No current capability/control register values.",
        "# Namespace -> ProcessorID pairs independently checked in SSDT-2.",
        "CPPC_STATIC_V1", "CPUS 16",
    ]
    for index, cpu in enumerate(cpc["cpus"]):
        name = f"C{index:03X}"
        path = "\\_PR_." + name
        require(cpu["scope"] == path, "canonical CPU path")
        # Exact byte pattern in a hash-locked, previously decoded static table;
        # not a parser or evaluator. No inferred namespace-to-ID join at import.
        processor = b"\x5b\x83\x0b" + name.encode("ascii") + bytes([index]) + b"\x10\x08\0\0\x06"
        require(tables["SSDT-2"].count(processor) == 1, "static Processor identity")
        require(cpu["cpc_num_entries"] == 23 and cpu["cpc_revision"] == 3, "CPC version/count")
        require(cpu["psd"] == {"num_entries": 5, "revision": 0, "domain": index // 2,
                               "coord_type": 254, "num_processors": 2}, "PSD domain")
        require(set(cpu["fields"]) == set(FIELDS), "CPC field set")
        start = int(cpu["cpc_name_offset"], 0)
        end = int(cpc["cpus"][index + 1]["scope_offset"], 0) if index < 15 else len(tables["SSDT-3"])
        block = tables["SSDT-3"][start:end]
        require(block[:5] == b"\x08_CPC", "static Name anchor")
        cursor = 0
        lines += ["", f"CPU {path} LAPIC {index}", f"PSD 5 0 {index // 2} 254 2",
                  "CPC 23", "I 23 # NumEntries", "I 3 # Revision"]
        for field_name in FIELDS:
            field = cpu["fields"][field_name]
            if field["kind"] == "integer":
                value = field["value"]
                require(type(value) is int and 0 <= value <= (1 << 64) - 1, "integer range")
                require((field_name, value) in (("LowestFrequency", 550), ("NominalFrequency", 3401)),
                        "static frequency anchors")
                lines.append(f"I {value} # {field_name}")
                continue
            require(field["kind"] == "register", "dynamic/unsupported field")
            raw = bytes.fromhex(field["resource_hex"])
            require(len(raw) == 17 and raw[:3] == b"\x82\x0c\0" and raw[15:] == b"\x79\0",
                    "Generic Register resource framing")
            space, width, offset, access = raw[3:7]
            address = int.from_bytes(raw[7:15], "little")
            require((space, width, offset, access, address) ==
                    (field["space_id"], field["bit_width"], field["bit_offset"],
                     field["access_size_raw"], int(field["address"], 0)), "GAS byte/JSON mismatch")
            found = block.find(raw, cursor)
            require(found >= 0, "resource absent/out of order in saved static Name block")
            cursor = found + len(raw)
            if field_name in PCC_OFFSETS:
                require((space, width, offset, access, address) ==
                        (10, 32, 0, 0, 0x100 * index + PCC_OFFSETS[field_name]), "PCC capture layout")
            elif field_name in ("ReferenceCounter", "DeliveredCounter"):
                require((space, width, offset, access, address) ==
                        (127, 64, 0, 4, 0xe7 if field_name == "ReferenceCounter" else 0xe8), "FFH counters")
            else:
                require((space, width, offset, access, address) == (0, 0, 0, 0, 0), "absent option")
            lines.append(f"B {raw.hex()} # {field_name}")
        lines.append("ENDCPU")
    lines.append("END")

    raw_pcct = tables["PCCT"]
    require(raw_pcct[48:50] == b"\0\x3e" and len(raw_pcct) == 110, "generic PCCT")
    require(struct.unpack_from("<QQ", raw_pcct, 56) == (0xbce90000, 0x10000), "PCCT memory evidence")
    require(raw_pcct[72:76] == b"\0\x40\0\x04", "doorbell GAS")
    require(struct.unpack_from("<Q", raw_pcct, 76)[0] == 0xfd010540, "doorbell address evidence")
    require(struct.unpack_from("<QQIIH", raw_pcct, 84) ==
            (0xffffffff00000000, 1, 4000, 60000, 0), "PCCT masks/timing")
    pcct = sanitized_header(raw_pcct)
    struct.pack_into("<Q", pcct, 56, 0x100000) # inert synthetic address labels
    struct.pack_into("<Q", pcct, 76, 0x200000)
    pcct = finish_table(pcct)

    madt = sanitized_header(tables["APIC"])
    struct.pack_into("<I", madt, 36, 0x300000)
    at, io_index, enabled = 44, 0, []
    while at < len(madt):
        kind, size = madt[at:at + 2]
        require(size >= 2 and at + size <= len(madt), "MADT length")
        if kind == 0:
            uid, apic = madt[at + 2:at + 4]
            flags = struct.unpack_from("<I", madt, at + 4)[0]
            if flags & 1:
                require(uid == apic, "saved UID/APIC evidence")
                enabled.append(uid)
        if kind == 1:
            struct.pack_into("<I", madt, at + 4, 0x400000 + io_index * 0x100000)
            io_index += 1
        at += size
    require(enabled == list(range(0, 16, 2)) + list(range(1, 16, 2)), "MADT even/odd order")
    madt = finish_table(madt)
    map_lines = [
        "# SYNTHETIC permutation for tests, NOT this host's measured XNU numbering.",
        "# No live XNU CPU mapping is claimed or discovered by this lab.",
        "XNU_MAP_V1", "CPUS 16",
    ]
    map_lines += [f"MAP {apic} {(apic * 5 + 3) % 16}" for apic in reversed(range(16))]
    map_lines.append("END")
    files = {
        "pcct.hex": hex_table(pcct, "PCCT"),
        "madt.hex": hex_table(madt, "MADT"),
        "static.cpc": ("\n".join(lines) + "\n").encode("ascii"),
        "xnu.map": ("\n".join(map_lines) + "\n").encode("ascii"),
    }
    metadata = {
        "kind": "derived/sanitized offline regression fixture; NOT original firmware",
        "source_provenance": "Saved AppleACPIPlatformExpert ACPI Tables property; OS-exposed post-boot-patch tables",
        "source_file_sha256": SOURCE_HASHES,
        "source_tables": provenance,
        "normalization": "Static verified revision-3/23-entry Name packages only; no AML interpreter/evaluation",
        "identity_source": "SSDT-2 static Processor declarations, joined by namespace path; not MADT list position",
        "xnu_mapping": "SYNTHETIC permutation, not measured; all physical/live values remain unverified",
        "sanitization": {
            "both_tables": "OEM/creator metadata replaced with LAB / DERIVED; byte-9 checksum recomputed",
            "pcct": "base and doorbell replaced with 0x100000 and 0x200000 inert labels",
            "madt": "local APIC and I/O APIC addresses replaced; all CPU records/order/disabled placeholders retained",
            "cpc": "lossless integer/resource bytes, redundant JSON annotations omitted",
        },
        "checksum_meaning": "ACPI byte sum modulo 256 is corruption detection, NOT a digital signature or proof of authenticity. Derived checksums/hashes do not identify original tables.",
        "derived_raw_tables": {
            "PCCT": {"sha256": digest(pcct), "bytes": len(pcct), "checksum_mod256": sum(pcct) % 256},
            "MADT": {"sha256": digest(madt), "bytes": len(madt), "checksum_mod256": sum(madt) % 256},
        },
        "fixture_file_sha256": {name: digest(data) for name, data in files.items()},
    }
    files["provenance.json"] = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("ascii")
    if args.check:
        for name, data in files.items():
            require((args.output_dir / name).read_bytes() == data, f"fixture differs: {name}")
        print(f"PASS: {len(files)} fixture files reproduced byte-for-byte; original and derived checksums distinguished")
    else:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        require(not any(args.output_dir.iterdir()), "output directory must be empty; refusing overwrite")
        for name, data in files.items():
            (args.output_dir / name).write_bytes(data)
        print(f"Wrote {len(files)} derived fixture files ({sum(map(len, files.values()))} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tables-plist", type=pathlib.Path, required=True)
    parser.add_argument("--manifest-json", type=pathlib.Path, required=True)
    parser.add_argument("--cpc-json", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--check", action="store_true", help="compare without writing")
    args = parser.parse_args()
    try:
        generate(args)
    except (ValueError, KeyError, TypeError, OSError, struct.error) as error:
        parser.exit(1, f"Fixture rejected: {error}\n")


if __name__ == "__main__":
    main()

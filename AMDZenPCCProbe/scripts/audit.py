#!/usr/bin/env python3
"""Offline structural checks; NOT a proof that kernel lifecycle/MMIO is safe."""
import pathlib
import plistlib
import re
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
bundle = root / "build/AMDZenPCCProbe.kext"
binary = bundle / "Contents/MacOS/AMDZenPCCProbe"
info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
personality = info["IOKitPersonalities"]["AMDZenPCCProbe"]
assert personality["EnablePccReadProbe"] is False
assert info["LSMinimumSystemVersion"] == "12.0"
assert info["OSBundleLibraries"]["com.dinovn.AMDZenPM"] == "1.0.0"
imports = subprocess.check_output(["nm", "-u", str(binary)], text=True)
# Reject imports OWNED by mapping classes, not mentions of those types in an
# inherited IOUserClient method's parameter list. clientMemoryForType's base
# vtable slot is emitted even though this client exposes no memory mapping API.
for klass in ("IOMemoryDescriptor", "IOMemoryMap"):
    owner = rf"(?m)^\s*_+Z(?:NK?|T[VIS]){len(klass)}{klass}"
    assert not re.search(owner, imports), "forbidden mapping-class import: " + klass
for forbidden in ("evaluateObject", "readAddressSpace", "writeAddressSpace", "pmKextRegister",
                  "IOTimerEventSource", "thread_call", "ml_io_map", "pmap_enter", "AMDZenPM_copySample"):
    assert forbidden not in imports, "forbidden import: " + forbidden
assert "AMDZenPM_copyInfo" in imports
assembly = subprocess.check_output(["otool", "-tvV", str(binary)], text=True)
assert not re.search(r"\b(?:rdmsr|wrmsr|rdmsr_safe|wrmsr_safe|inb|inw|inl|outb|outw|outl)\b", assembly)
source = (root / "kernel/AMDZenPCCProbe.cpp").read_text()
assert "control_, client, nullptr, reply)" in source, "production transport boundary changed"
for path in (root / "kernel").glob("*.cpp"):
    text = path.read_text()
    assert not re.search(r"reinterpret_cast\s*<\s*volatile|withPhysicalAddress\s*\(|ioremap\s*\(", text)
makefile = (root / "Makefile").read_text()
assert not re.search(r"(?m)^(?:install|load|reload|deploy)\s*:", makefile)
print("Offline audit: closed config gate, null physical transport, no mapping/MSR/port/timer/PM-dispatch imports.")
print("Audit does not validate real ownership acquisition, cache attributes, device barriers, sleep fences or hardware.")

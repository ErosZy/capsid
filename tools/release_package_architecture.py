"""Independent release CPU checks; an archive name is not binary architecture evidence."""

import os
from pathlib import Path
import platform
import re
import struct
import subprocess

ELF_MACHINES = {"x86_64": 62, "arm64": 183}
MACHO_CPU_TYPES = {"x86_64": 0x01000007, "arm64": 0x0100000C}


def canonical_arch(machine):
    value = machine.lower()
    if value in {"x86_64", "amd64", "x64"}:
        return "x86_64"
    if value in {"aarch64", "arm64"}:
        return "arm64"
    raise ValueError(f"unsupported release architecture: {machine}")


def native_release_platform(system=None, machine=None):
    system = (system or platform.system()).lower()
    arch = canonical_arch(machine or platform.machine())
    if system == "linux":
        return f"linux-musl-{arch}"
    if system == "darwin":
        return f"darwin-{arch}"
    if system == "windows" and arch == "x86_64":
        return "windows-x86_64"
    raise ValueError(f"unsupported release host: {system}/{arch}")


def validate_elf_header(data, expected_arch):
    if expected_arch not in ELF_MACHINES:
        raise ValueError("unsupported expected ELF architecture")
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("release binary must be a complete little-endian ELF64 header")
    kind, machine, version = struct.unpack_from("<HHI", data, 16)
    if kind not in {2, 3} or version != 1 or struct.unpack_from("<H", data, 52)[0] != 64:
        raise ValueError("release binary must be an ELF64 executable or PIE")
    if machine != ELF_MACHINES[expected_arch]:
        raise ValueError(f"ELF machine {machine} does not match {expected_arch}")
    return machine


def verify_macos_binary(data, expected_arch):
    if expected_arch not in MACHO_CPU_TYPES:
        raise ValueError("unsupported expected Mach-O architecture")
    if len(data) < 32 or data[:4] != b"\xcf\xfa\xed\xfe":
        raise ValueError("release binary must be a complete thin little-endian Mach-O64 header")
    cpu_type, _, file_type = struct.unpack_from("<III", data, 4)
    if file_type != 2:
        raise ValueError("release Mach-O binary must be an executable")
    if cpu_type != MACHO_CPU_TYPES[expected_arch]:
        raise ValueError(f"Mach-O CPU type {cpu_type} does not match {expected_arch}")
    return {"architecture": expected_arch, "machOCpuType": cpu_type, "thin": True}


def validate_static_elf_output(output):
    if "Program Headers:" not in output or not re.search(r"^\s*LOAD\s", output, re.M):
        raise ValueError("missing ELF program-header evidence")
    if re.search(r"\bINTERP\b|\(NEEDED\)", output):
        raise ValueError("musl release binary requires a dynamic loader or shared library")


def verify_linux_binary(path, data, expected_arch):
    machine = validate_elf_header(data, expected_arch)
    result = subprocess.run(
        ["readelf", "--wide", "--program-headers", "--dynamic", str(Path(path))],
        capture_output=True,
        text=True,
        check=True,
        timeout=20,
        env={**os.environ, "LC_ALL": "C"},
    )
    validate_static_elf_output(result.stdout)
    return {"architecture": expected_arch, "elfMachine": machine, "fullyStatic": True}

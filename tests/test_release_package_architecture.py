"""CPU identity controls, independent of the release matrix's archive labels."""

import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "release_package_architecture", ROOT / "tools/release_package_architecture.py"
)
ARCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCH)


def elf(machine):
    data = bytearray(64)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHI", data, 16, 2, machine, 1)
    struct.pack_into("<H", data, 52, 64)
    return bytes(data)


def macho(cpu_type):
    return struct.pack("<IIIIIIII", 0xFEEDFACF, cpu_type, 0, 2, 0, 0, 0, 0)


class ReleaseArchitectureTests(unittest.TestCase):
    def test_native_runner_architecture_aliases(self):
        for machine in ["x86_64", "AMD64", "amd64", "x64"]:
            self.assertEqual(ARCH.native_release_platform("Linux", machine), "linux-musl-x86_64")
        for machine in ["aarch64", "ARM64", "arm64"]:
            self.assertEqual(ARCH.native_release_platform("Linux", machine), "linux-musl-arm64")
        self.assertEqual(ARCH.native_release_platform("Darwin", "arm64"), "darwin-arm64")
        self.assertEqual(ARCH.native_release_platform("Darwin", "x86_64"), "darwin-x86_64")
        self.assertEqual(ARCH.native_release_platform("Windows", "AMD64"), "windows-x86_64")
        for machine in ["i686", "x86", "armv7l", "riscv64"]:
            with self.subTest(machine=machine), self.assertRaises(ValueError):
                ARCH.native_release_platform("Linux", machine)

    def test_actual_header_machine_numbers_not_archive_names(self):
        self.assertEqual(ARCH.validate_elf_header(elf(62), "x86_64"), 62)
        self.assertEqual(ARCH.validate_elf_header(elf(183), "arm64"), 183)
        for machine, target in [(62, "arm64"), (183, "x86_64"), (40, "arm64"), (3, "x86_64")]:
            with (
                self.subTest(machine=machine, target=target),
                self.assertRaisesRegex(ValueError, "machine"),
            ):
                ARCH.validate_elf_header(elf(machine), target)

    def test_truncated_wrong_endian_width_and_object_headers_fail(self):
        good = elf(183)
        malformed = [good[:20], b"not ELF" + good[7:]]
        for offset, value in [(4, 1), (5, 2), (6, 0), (16, 1), (52, 0)]:
            data = bytearray(good)
            data[offset] = value
            malformed.append(bytes(data))
        for data in malformed:
            with self.subTest(header=data[:24]), self.assertRaises(ValueError):
                ARCH.validate_elf_header(data, "arm64")

    def test_macho_cpu_types_are_checked_for_both_native_archives(self):
        for cpu, arch in [(0x01000007, "x86_64"), (0x0100000C, "arm64")]:
            result = ARCH.verify_macos_binary(macho(cpu), arch)
            self.assertEqual(result, {"architecture": arch, "machOCpuType": cpu, "thin": True})
        for cpu, arch in [(0x01000007, "arm64"), (0x0100000C, "x86_64"), (7, "x86_64")]:
            with self.subTest(cpu=cpu, arch=arch), self.assertRaisesRegex(ValueError, "CPU type"):
                ARCH.verify_macos_binary(macho(cpu), arch)

    def test_macho_rejects_fat_truncated_wrong_endian_and_nonexecutables(self):
        good = macho(0x01000007)
        malformed = [good[:16], b"\xca\xfe\xba\xbe" + good[4:], good[::-1], elf(62)]
        for offset, value in [(0, 0xCE), (12, 6)]:
            bad = bytearray(good)
            bad[offset] = value
            malformed.append(bytes(bad))
        for data in malformed:
            with self.subTest(header=data[:16]), self.assertRaises(ValueError):
                ARCH.verify_macos_binary(data, "x86_64")

    def test_static_policy_rejects_dynamic_loader_and_needed_libraries(self):
        static = "Program Headers:\n  LOAD 0x000000 0x400000\nThere is no dynamic section in this file.\n"
        ARCH.validate_static_elf_output(static)
        for output in [
            "",
            "No ELF evidence",
            static + "  INTERP 0x200\n",
            static + " 0x1 (NEEDED) Shared library: [libc.so.6]\n",
        ]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                ARCH.validate_static_elf_output(output)


if __name__ == "__main__":
    unittest.main()

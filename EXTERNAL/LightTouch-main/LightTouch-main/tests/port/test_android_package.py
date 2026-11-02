"""Regression checks for the packaged ELF contract, without proprietary inputs."""
import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location(
    "package_check", Path(__file__).resolve().parents[2] / "scripts/check-android-package.py"
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def elf(machine=183, alignment=16384, flags=5, address=0):
    result = bytearray(128)
    result[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", result, 16,
                     3, machine, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", result, 64, 1, flags, 0, address, address, 128, 128, alignment)
    return result


class NativePackageContract(unittest.TestCase):
    def test_accepts_both_target_machines(self):
        for abi, machine in CHECK.MACHINES.items():
            self.assertEqual(CHECK.inspect_elf(elf(machine), abi)[0]["alignment"], 16384)

    def test_rejects_4k_dependency_even_when_other_libraries_are_aligned(self):
        with self.assertRaisesRegex(ValueError, "alignment"):
            CHECK.inspect_elf(elf(alignment=4096), "arm64-v8a")

    def test_rejects_abi_mismatch(self):
        with self.assertRaisesRegex(ValueError, "machine"):
            CHECK.inspect_elf(elf(machine=62), "arm64-v8a")

    def test_rejects_incongruent_segment_offsets(self):
        with self.assertRaisesRegex(ValueError, "incongruent"):
            CHECK.inspect_elf(elf(address=4096), "arm64-v8a")

    def test_rejects_writable_executable_segments(self):
        with self.assertRaisesRegex(ValueError, "writable executable"):
            CHECK.inspect_elf(elf(flags=7), "arm64-v8a")

    def test_rejects_truncated_segment(self):
        with self.assertRaisesRegex(ValueError, "truncated"):
            CHECK.inspect_elf(elf()[:-1], "arm64-v8a")


if __name__ == "__main__":
    unittest.main()

# -*- coding: utf-8 -*-
"""Собирает tests/native_test.cpp компилятором компьютера и сверяет кодек прошивки с эталоном на Python."""
import math
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "hub"))
import wt_proto as P  # noqa: E402

CXX = shutil.which("clang++") or shutil.which("g++")


def signal(frame):
    out = []
    for i in range(320):
        n = frame * 320 + i
        v = 9000 * math.sin(2 * math.pi * 440 * n / 16000.0) + 3000 * math.sin(2 * math.pi * 1300 * n / 16000.0) \
            + ((n * 7919) % 601) - 300
        # lrint: банковское округление к чётному, как в C
        out.append(int(round(v)))
    return out


@unittest.skipIf(CXX is None, "нет компилятора C++")
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        exe = cls.tmp / "native_test"
        fw = ROOT / "firmware" / "walkie"
        subprocess.run([CXX, "-std=c++17", "-O1", "-Wall", "-Wextra", "-I", str(fw),
                        str(ROOT / "tests" / "native_test.cpp"), str(fw / "adpcm.cpp"), str(fw / "jitter.cpp"),
                        str(fw / "battery_est.cpp"), str(fw / "howl.cpp"),
                        "-o", str(exe)], check=True)
        cls.run_ = subprocess.run([str(exe)], capture_output=True, text=True)

    def test_cpp_checks_pass(self):
        self.assertEqual(self.run_.returncode, 0, self.run_.stderr + self.run_.stdout[-500:])

    def test_adpcm_matches_python_reference(self):
        lines = [l[4:] for l in self.run_.stdout.splitlines() if l.startswith("ENC ")]
        self.assertEqual(len(lines), 6)
        enc = P.AdpcmEncoder()
        for f, hexline in enumerate(lines):
            self.assertEqual(enc.encode(signal(f)).hex(), hexline, "кадр %d" % f)


if __name__ == "__main__":
    unittest.main()

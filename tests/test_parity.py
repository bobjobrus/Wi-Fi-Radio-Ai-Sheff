# -*- coding: utf-8 -*-
"""Константы протокола в прошивке (protocol.h) и в мосте (wt_proto.py) должны совпадать."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "hub"))
import wt_proto as P  # noqa: E402

H = (ROOT / "firmware" / "walkie" / "protocol.h").read_text(encoding="utf-8")


def c_value(name):
    m = re.search(r"\b%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)" % re.escape(name), H)
    if not m:
        raise AssertionError("нет %s в protocol.h" % name)
    return int(m.group(1), 0)


class ParityTests(unittest.TestCase):
    def test_types_and_reasons(self):
        for name in ["T_HELLO", "T_HELLO_ACK", "T_TALK_REQ", "T_TALK_GRANT", "T_TALK_DENY", "T_TALK_END",
                     "T_TALK_START", "T_AUDIO", "T_PEER_HELLO", "T_BAD_KEY",
                     "DENY_BUSY", "DENY_REVOKED", "DENY_TIMEOUT", "DENY_UNKNOWN",
                     "SAMPLE_RATE", "FRAME_SAMPLES", "CODEC_ADPCM16", "FLAG_MUTED"]:
            self.assertEqual(c_value(name), getattr(P, name), name)

    def test_sizes(self):
        self.assertEqual(c_value("VERSION"), P.VERSION)
        self.assertEqual(c_value("DEFAULT_PORT"), P.DEFAULT_PORT)
        self.assertEqual(c_value("HDR_LEN"), P.HDR.size)
        self.assertEqual(c_value("MAC_LEN"), P.MAC_LEN)
        self.assertEqual(c_value("NAME_MAX_BYTES"), P.NAME_MAX)
        # составные размеры считаем по выражениям из protocol.h
        self.assertEqual(3 + P.FRAME_SAMPLES // 2, P.AUDIO_ENC_LEN)
        self.assertEqual(P.HELLO_FIX.size, 2 + 1 + 1 + 4 + 2)
        self.assertEqual(P.ACK_FIX.size, 4 + 1 + 1 + 1 + 1 + 2 + 4 + 32 + 2)
        self.assertEqual(P.AUDIO_FIX.size, 4 + 2 + 1 + 1)
        self.assertIn("HELLO_FIX = 2 + 1 + 1 + 4 + 2", H)
        self.assertIn("ACK_FIX = 4 + 1 + 1 + 1 + 1 + 2 + 4 + 32 + 2", H)
        self.assertIn("AUDIO_FIX = 4 + 2 + 1 + 1", H)


if __name__ == "__main__":
    unittest.main()

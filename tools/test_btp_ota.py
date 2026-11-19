#!/usr/bin/env python3
"""Tests for btp_ota.py's authentication: the same golden vector as
test/host/test_ota_auth.cpp, so the tool and the recovery cannot drift apart.

Run with: python tools/test_btp_ota.py
"""

import hashlib
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import btp_ota  # noqa: E402

KEY = bytes(range(16))
NONCE = bytes(range(0xA0, 0xB0))
IMAGE = b"firmware image bytes" * 100


class AuthTest(unittest.TestCase):
    def test_update_matches_the_firmware_vector(self):
        digest = hashlib.sha256(IMAGE).digest()
        self.assertEqual(digest.hex(), "3f5a0b3dfe39f2714a5b2c004faa882dd9a768296bb1bca2d47435894341d11f")
        self.assertEqual(btp_ota.auth_mac(KEY, NONCE, b"U", digest),
                         "6ef143d06ec211459855a0c6588c7ef795799e78d6d002265e82dbf38c89fc8e")

    def test_reboot_matches_the_firmware_vector(self):
        self.assertEqual(btp_ota.auth_mac(KEY, NONCE, b"B", bytes(32)),
                         "800ea9f1afc17066dbd1d5d53cafe0353f0221b71887ed2968575926adeedf21")

    def test_bad_inputs_are_refused(self):
        with self.assertRaises(ValueError):
            btp_ota.auth_mac(KEY, NONCE[:8], b"U", bytes(32))
        with self.assertRaises(ValueError):
            btp_ota.auth_mac(KEY, NONCE, b"X", bytes(32))


if __name__ == "__main__":
    unittest.main()

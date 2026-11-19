#!/usr/bin/env python3
"""Tests for btp_ota.py's authentication: the same golden vector as
test/host/test_ota_auth.cpp, so the tool and the recovery cannot drift apart.

Run with: python tools/test_btp_ota.py
"""

import hashlib
import hmac
import json
import os
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import btp_ota  # noqa: E402
import provision_key  # noqa: E402

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


PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recovery", "main", "page.html")

# Runs the page's crypto section under Node and prints one JSON object.
NODE_HARNESS = r"""
const fs = require("fs");
const html = fs.readFileSync(process.argv[1], "utf8");
const code = html.slice(html.indexOf("// --- crypto"), html.indexOf("// --- end crypto ---"));
const api = new Function(code + "; return {sha256, hmac, pbkdf2, deriveKey, authMac, hex, unhex, utf8};")();
const range = (a, b) => Uint8Array.from({length: b - a}, (_, i) => a + i);
const image = api.utf8("firmware image bytes".repeat(100));
const sha = api.sha256(image);
const big = Uint8Array.from({length: 1234567}, (_, i) => (i * 31 + 7) & 255);
console.log(JSON.stringify({
  sha: api.hex(sha),
  empty: api.hex(api.sha256(new Uint8Array(0))),
  big: api.hex(api.sha256(big)),
  hmac_long_key: api.hex(api.hmac(range(0, 100), api.utf8("msg"))),
  update: api.authMac(range(0, 16), range(0xa0, 0xb0), "U", sha),
  reboot: api.authMac(range(0, 16), range(0xa0, 0xb0), "B", new Uint8Array(32)),
  pbkdf2_short: api.hex(api.pbkdf2(api.utf8("pw"), api.utf8("salt"), 1000, 16)),
  derived: api.hex(api.deriveKey("senha de teste ç")),
}));
"""


@unittest.skipIf(shutil.which("node") is None, "node not installed")
class PageCryptoTest(unittest.TestCase):
    """The recovery's web page signs uploads in JavaScript: same vectors."""

    @classmethod
    def setUpClass(cls):
        out = subprocess.run(["node", "-e", NODE_HARNESS, PAGE], capture_output=True, text=True, check=True)
        cls.r = json.loads(out.stdout)

    def test_sha256(self):
        self.assertEqual(self.r["sha"], hashlib.sha256(IMAGE).hexdigest())
        self.assertEqual(self.r["empty"], hashlib.sha256(b"").hexdigest())
        big = bytes((i * 31 + 7) & 255 for i in range(1234567))
        self.assertEqual(self.r["big"], hashlib.sha256(big).hexdigest())

    def test_hmac(self):
        self.assertEqual(self.r["hmac_long_key"], hmac.new(bytes(range(100)), b"msg", hashlib.sha256).hexdigest())

    def test_auth_matches_the_firmware_vector(self):
        self.assertEqual(self.r["update"], "6ef143d06ec211459855a0c6588c7ef795799e78d6d002265e82dbf38c89fc8e")
        self.assertEqual(self.r["reboot"], "800ea9f1afc17066dbd1d5d53cafe0353f0221b71887ed2968575926adeedf21")

    def test_key_derivation_matches_provision_key(self):
        self.assertEqual(self.r["pbkdf2_short"], hashlib.pbkdf2_hmac("sha256", b"pw", b"salt", 1000, 16).hex())
        self.assertEqual(self.r["derived"], provision_key.derive_key("senha de teste ç").hex())


if __name__ == "__main__":
    unittest.main()

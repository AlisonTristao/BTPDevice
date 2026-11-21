#!/usr/bin/env python3
"""Talk to a TraceKit recovery over Wi-Fi (docs/PLANO.md 4.14, D20-D21).

    python tools/btp_ota.py meu-sensor.local --status
    python tools/btp_ota.py meu-sensor.local firmware.bin
    python tools/btp_ota.py meu-sensor.local --reboot

The device must be in the recovery first (`btp -recovery` in TraceView's
terminal). Requests are authenticated by challenge, with key E derived from
password E exactly as tools/provision_key.py does -- the password never
leaves this machine:

    nonce      = GET /status -> "nonce"
    message    = b"btp-ota-1" + nonce + action + sha256(image)   ('U' update, 'B' reboot)
    X-OTA-Auth = hex(HMAC-SHA256(key_E, message))

When the recovery lists "deflate" in its "encodings", the image goes as a
zlib stream (X-OTA-Encoding: deflate, X-OTA-Size: the image's size), about a
third smaller; the hash and the signature stay over the image itself.
--no-compress sends it as is (to compare the times).

Password E comes from --password-e, the BTP_PASSWORD_E environment variable
or a prompt; or give the key file itself with --key-file bally.key.
"""

import argparse
import getpass
import hashlib
import hmac
import http.client
import json
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import provision_key  # noqa: E402  (the key-derivation contract lives there)

LABEL = b"btp-ota-1"
NONCE_SIZE = 16
KEY_FILE_SIZE = 96
KEY_E_OFFSET = 32


def auth_mac(key_e, nonce, action, image_sha256):
    """HMAC-SHA256(key_E, label || nonce || action || sha256), the device's check."""
    if len(nonce) != NONCE_SIZE or len(image_sha256) != 32 or action not in (b"U", b"B"):
        raise ValueError("bad nonce, hash or action")
    return hmac.new(key_e, LABEL + nonce + action + image_sha256, hashlib.sha256).hexdigest()


def key_from_file(path):
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) != KEY_FILE_SIZE:
        raise SystemExit("%s: %d bytes, a key file has %d" % (path, len(blob), KEY_FILE_SIZE))
    problems = provision_key.check_blob(blob)
    if problems:
        raise SystemExit("%s: %s" % (path, "; ".join(problems)))
    return blob[KEY_E_OFFSET:KEY_E_OFFSET + provision_key.KEY_LENGTH]


def resolve_key(args, status):
    if status.get("auth") != "hmac-sha256":
        return None
    if args.key_file:
        return key_from_file(args.key_file)
    password = args.password_e or os.environ.get("BTP_PASSWORD_E")
    if not password:
        password = getpass.getpass("password E of %s: " % status.get("name", args.host))
    print("deriving key E (PBKDF2, %d iterations)..." % provision_key.KDF_ITERATIONS, file=sys.stderr)
    return provision_key.derive_key(password)


def request(args, method, path, body=None, headers=None, timeout=10):
    connection = http.client.HTTPConnection(args.host, args.port, timeout=timeout)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read().decode("utf-8", "replace")
    finally:
        connection.close()


def get_status(args):
    try:
        code, text = request(args, "GET", "/status", timeout=args.timeout)
    except OSError as error:
        raise SystemExit(
            "%s: %s\nIs the device in the recovery? (`btp -recovery` in TraceView's terminal)" % (args.host, error))
    if code != 200:
        raise SystemExit("%s: /status answered %d: %s" % (args.host, code, text))
    status = json.loads(text)
    if status.get("mode") != "recovery":
        raise SystemExit("%s is not a TraceKit recovery (mode %r)" % (args.host, status.get("mode")))
    return status


def print_status(status):
    app = status.get("app", {})
    print("device    %s (%s)" % (status.get("name"), status.get("chip")))
    print("recovery  %s, reason %s" % (status.get("recovery"), status.get("reason")))
    print("app       %s" % ("%s %s" % (app.get("project"), app.get("version")) if app.get("valid") else "none"))
    print("auth      %s, ready %s, up to %d KB" % (status.get("auth"), status.get("ota_ready"),
                                                   status.get("max_size", 0) // 1024))


def wait_for_recovery(args):
    """The device may still be restarting into the recovery: retry a while."""
    deadline = time.monotonic() + args.wait
    while True:
        try:
            return get_status(args)
        except SystemExit:
            if time.monotonic() >= deadline:
                raise
            time.sleep(2)


def upload(args, status, key_e):
    with open(args.firmware, "rb") as handle:
        image = handle.read()
    if not image or image[0] != 0xE9:
        raise SystemExit("%s is not an ESP application image" % args.firmware)
    if len(image) > status.get("max_size", 0):
        raise SystemExit("%s: %d bytes, the app partition holds %d" % (args.firmware, len(image), status["max_size"]))
    digest = hashlib.sha256(image).digest()
    headers = {"Content-Type": "application/octet-stream", "X-OTA-SHA256": digest.hex()}
    if key_e is not None:
        headers["X-OTA-Auth"] = auth_mac(key_e, bytes.fromhex(status["nonce"]), b"U", digest)
    body = image
    if not args.no_compress and "deflate" in status.get("encodings", []):
        compressed = zlib.compress(image, 9)
        if len(compressed) < len(image):
            body = compressed
            headers["X-OTA-Encoding"] = "deflate"
            headers["X-OTA-Size"] = str(len(image))
    if body is image:
        print("uploading %s (%d KB)..." % (os.path.basename(args.firmware), len(image) // 1024))
    else:
        print("uploading %s (%d KB, %d KB compressed)..."
              % (os.path.basename(args.firmware), len(image) // 1024, len(body) // 1024))
    started = time.monotonic()
    code, text = request(args, "POST", "/update", body=body, headers=headers, timeout=120)
    print("%d %s (%.1f s)" % (code, text, time.monotonic() - started))
    return 0 if code == 200 else 1


def reboot(args, status, key_e):
    headers = {}
    if key_e is not None:
        headers["X-OTA-Auth"] = auth_mac(key_e, bytes.fromhex(status["nonce"]), b"B", bytes(32))
    code, text = request(args, "POST", "/reboot", body=b"", headers=headers)
    print("%d %s" % (code, text))
    return 0 if code == 200 else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", help="<name>.local or an IP address")
    parser.add_argument("firmware", nargs="?", help="application .bin to upload")
    parser.add_argument("--status", action="store_true", help="only show the recovery's status")
    parser.add_argument("--reboot", action="store_true", help="leave the recovery, back to the application")
    parser.add_argument("--password-e", help="password E (else BTP_PASSWORD_E or a prompt)")
    parser.add_argument("--key-file", help="bally.key instead of the password")
    parser.add_argument("--no-compress", action="store_true", help="send the image uncompressed")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds for /status")
    parser.add_argument("--wait", type=float, default=60.0, help="seconds to wait for the recovery to answer")
    args = parser.parse_args(argv)
    if not (args.status or args.reboot or args.firmware):
        parser.error("give a firmware, --status or --reboot")

    status = wait_for_recovery(args)
    print_status(status)
    if args.status:
        return 0
    if status.get("auth") == "unavailable":
        raise SystemExit("the device has no key: provision one (tools/provision_key.py) and boot the application once")
    key_e = resolve_key(args, status)
    # The nonce changes after every attempt: read it right before using it.
    status = get_status(args)
    return reboot(args, status, key_e) if args.reboot else upload(args, status, key_e)


if __name__ == "__main__":
    sys.exit(main())

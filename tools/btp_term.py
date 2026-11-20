#!/usr/bin/env python3
"""Minimal BTP terminal for bench tests without TraceView.

Opens a session (HELLO), types each command into the device's shell
(TERMINAL_IN) and prints what comes back (TERMINAL_OUT):

    python tools/btp_term.py COM8 "btp -status" "btp -mem"                    (Serial, in clear)
    python tools/btp_term.py tracekit-s3-demo.local --password-e ... "btp -status"   (TCP, sealed)
    python tools/btp_term.py COM8 --key-file data/bally.key "btp -recovery"   (Serial, sealed)

A target that looks like a serial port (COM8, /dev/ttyACM0) is opened as
one; anything else is a TCP host (port 44300 unless given as host:port). With
the key E (--password-e or --key-file) every message is sealed with
AES-128-GCM, as a device with a key requires (docs/encryption.md in BTP).

Needs pyserial, and `cryptography` for sealed sessions.
"""

import argparse
import os
import random
import re
import socket
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"BTP\x00"
HEADER_SIZE = 36
TAG_SIZE = 16
FLAG_FRAGMENTED = 0x0001
FLAG_ENCRYPTED = 0x0002  # CIPHER_ID 0 (AES-128-GCM) in bits 2-3
TYPE_TERMINAL, TYPE_CONTROL = 0x04, 0x05
HELLO, HELLO_RESULT = 0x0001, 0x0002
TERMINAL_IN, TERMINAL_OUT = 0x0001, 0x0002
ROLE_DIAGNOSTIC_TOOL = 0x04
TCP_PORT = 44300


def cobs_encode(data):
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(255)
                out += block
                block = bytearray()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data) + 1:
            return None
        out += data[i + 1:i + code]
        i += code
        if code < 255 and i < len(data):
            out.append(0)
    return bytes(out)


class SerialPort:
    def __init__(self, name, baud):
        import serial
        self.port = serial.Serial()
        self.port.port = name
        self.port.baudrate = baud
        self.port.timeout = 0.05
        self.port.write_timeout = 2  # a device not reading the port (the recovery) must not hang us
        self.port.dtr = False  # never strap the chip into download mode
        self.port.rts = False
        self.port.open()
        self.timeout_errors = (serial.SerialTimeoutException,)

    def write(self, data):
        self.port.write(data)

    def read(self):
        return self.port.read(4096)

    def close(self):
        self.port.close()


class TcpPort:
    def __init__(self, target):
        host, _, port = target.partition(":")
        self.sock = socket.create_connection((host, int(port) if port else TCP_PORT), timeout=5)
        self.sock.settimeout(0.05)
        self.timeout_errors = (socket.timeout,)

    def write(self, data):
        self.sock.sendall(data)

    def read(self):
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return b""
        if not data:
            raise ConnectionError("the device closed the connection")
        return data

    def close(self):
        self.sock.close()


class Session:
    def __init__(self, link, key_e, timeout_ms):
        self.link = link
        self.aead = None
        if key_e is not None:
            from cryptography.hazmat.primitives.ciphers.aead import AESGCM
            self.aead = AESGCM(key_e)
        self.timeout_ms = timeout_ms
        self.source_id = random.getrandbits(32) | 1
        self.boot_id = random.getrandbits(32) | 1
        self.sequence = 0
        self.rx = bytearray()
        self.fragments = {}

    @staticmethod
    def header(version, msg_type, flags, size, source, boot, sequence, timestamp, object_id):
        return MAGIC + struct.pack("<BBHHH", version, msg_type, flags, HEADER_SIZE, size) + \
            struct.pack("<IIIQHBB", source, boot, sequence, timestamp, object_id, 0, 1)

    def send(self, msg_type, object_id, payload):
        self.sequence += 1
        timestamp = int(time.monotonic() * 1e6)
        if self.aead is None:
            head = self.header(1, msg_type, 0, len(payload), self.source_id, self.boot_id, self.sequence,
                               timestamp, object_id)
        else:
            # The AAD is the canonical (unfragmented) header itself, payload
            # size counting the tag; the nonce is source || boot || sequence.
            head = self.header(2, msg_type, FLAG_ENCRYPTED, len(payload) + TAG_SIZE, self.source_id,
                               self.boot_id, self.sequence, timestamp, object_id)
            nonce = struct.pack("<III", self.source_id, self.boot_id, self.sequence)
            payload = self.aead.encrypt(nonce, payload, head)
        body = head + payload
        frame = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        self.link.write(b"\x00" + cobs_encode(frame) + b"\x00")

    def open(self, body):
        """The plaintext payload of one logical message, or None (a fragment
        still waiting for the rest, or one that does not authenticate)."""
        flags = struct.unpack("<H", body[6:8])[0]
        size = struct.unpack("<H", body[10:12])[0]
        payload = body[HEADER_SIZE:HEADER_SIZE + size]
        head = body[:HEADER_SIZE]
        if flags & FLAG_FRAGMENTED:
            # Fragments of one message share source, boot and sequence.
            key = body[12:24]
            index, count = body[34], body[35]
            parts = self.fragments.setdefault(key, {})
            parts[index] = payload
            if len(parts) < count:
                return None
            payload = b"".join(parts[i] for i in range(count))
            del self.fragments[key]
            # The canonical logical header: what the sender sealed.
            flags &= ~FLAG_FRAGMENTED
            head = head[:6] + struct.pack("<H", flags) + head[8:10] + struct.pack("<H", len(payload)) + \
                head[12:34] + bytes([0, 1])
        if not flags & FLAG_ENCRYPTED:
            return payload
        if self.aead is None:
            return None
        try:
            return self.aead.decrypt(head[12:24], payload, head)
        except Exception:
            return None

    def frames(self, seconds):
        """Yields (type, object_id, payload) for every valid frame within `seconds`."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.rx += self.link.read()
            while b"\x00" in self.rx:
                chunk, _, rest = bytes(self.rx).partition(b"\x00")
                self.rx = bytearray(rest)
                if not chunk:
                    continue
                frame = cobs_decode(chunk)
                if frame is None or len(frame) < HEADER_SIZE + 4 or frame[:4] != MAGIC:
                    continue
                body, crc = frame[:-4], struct.unpack("<I", frame[-4:])[0]
                if zlib.crc32(body) & 0xFFFFFFFF != crc:
                    continue
                payload = self.open(body)
                if payload is not None:
                    yield body[5], struct.unpack("<H", body[32:34])[0], payload

    def hello(self):
        payload = struct.pack("<BBHIHHII", ROLE_DIAGNOSTIC_TOOL, 2, 0, 4056, 2, 4, 8, self.timeout_ms)
        payload += os.urandom(16) + struct.pack("<I", 0) + bytes([1, 2])
        # The handshake is in clear even on a sealed link; everything after
        # it is sealed.
        aead, self.aead = self.aead, None
        self.send(TYPE_CONTROL, HELLO, payload)
        self.aead = aead
        for msg_type, object_id, result in self.frames(3.0):
            if msg_type == TYPE_CONTROL and object_id == HELLO_RESULT:
                return len(result) > 12 and result[12] == 0  # status SUCCESS
        return False


def key_from_args(args):
    if args.password_e:
        import provision_key
        return provision_key.derive_key(args.password_e)
    if args.key_file:
        import btp_ota
        return btp_ota.key_from_file(args.key_file)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", help="serial port (COM8, /dev/ttyACM0) or TCP host[:port]")
    parser.add_argument("commands", nargs="*", help="shell lines, e.g. \"btp -status\"")
    parser.add_argument("--wait", type=float, default=1.5, help="seconds to collect output after each line")
    parser.add_argument("--baud", type=int, default=921600, help="UART links only (USB ignores it)")
    parser.add_argument("--password-e", help="seal the session with the key E derived from this password")
    parser.add_argument("--key-file", help="seal the session with the key E of this bally.key")
    args = parser.parse_intermixed_args()

    serial_like = re.match(r"^(COM\d+|/dev/)", args.target, re.IGNORECASE)
    link = SerialPort(args.target, args.baud) if serial_like else TcpPort(args.target)
    session = Session(link, key_from_args(args), timeout_ms=10000)
    try:
        opened = session.hello()
    except link.timeout_errors:
        opened = False
    if not opened:
        sys.exit("no HELLO_RESULT: wrong link, or a key mismatch (a device with a key needs --password-e)")
    print("[session open%s]" % (", sealed" if session.aead else ""))
    for line in args.commands:
        print("> " + line)
        session.send(TYPE_TERMINAL, TERMINAL_IN, (line + "\n").encode())
        for msg_type, object_id, payload in session.frames(args.wait):
            if msg_type == TYPE_TERMINAL and object_id == TERMINAL_OUT:
                sys.stdout.write(payload.decode("utf-8", "replace"))
                sys.stdout.flush()
    link.close()


if __name__ == "__main__":
    main()

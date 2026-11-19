#!/usr/bin/env python3
"""Minimal BTP terminal over a serial port, in clear (a device without a key).

Opens a session (HELLO), types each command into the device's shell
(TERMINAL_IN) and prints what comes back (TERMINAL_OUT). For bench tests
without TraceView:

    python tools/btp_term.py COM8 "btp -status" "btp -mem"
    python tools/btp_term.py COM8 --wait 3 "demo -crash"

A sealed session (a device with a key) is TraceView's job: this tool does not
encrypt. Needs pyserial.
"""

import argparse
import os
import random
import struct
import sys
import time
import zlib

import serial

MAGIC = b"BTP\x00"
HEADER_SIZE = 36
TYPE_COMMAND, TYPE_TERMINAL, TYPE_CONTROL = 0x03, 0x04, 0x05
HELLO, HELLO_RESULT = 0x0001, 0x0002
TERMINAL_IN, TERMINAL_OUT = 0x0001, 0x0002
ROLE_DIAGNOSTIC_TOOL = 0x04


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


class Session:
    def __init__(self, port, timeout_ms):
        self.port = port
        self.timeout_ms = timeout_ms
        self.source_id = random.getrandbits(32) | 1
        self.boot_id = random.getrandbits(32) | 1
        self.sequence = 0
        self.rx = bytearray()

    def send(self, msg_type, object_id, payload):
        self.sequence += 1
        header = MAGIC + struct.pack("<BBHHH", 1, msg_type, 0, HEADER_SIZE, len(payload))
        header += struct.pack("<IIIQHBB", self.source_id, self.boot_id, self.sequence,
                              int(time.monotonic() * 1e6), object_id, 0, 1)
        body = header + payload
        frame = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        self.port.write(b"\x00" + cobs_encode(frame) + b"\x00")

    def frames(self, seconds):
        """Yields (type, object_id, payload) for every valid frame within `seconds`."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.rx += self.port.read(4096)
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
                msg_type = body[5]
                payload_len = struct.unpack("<H", body[10:12])[0]
                object_id = struct.unpack("<H", body[32:34])[0]
                yield msg_type, object_id, body[HEADER_SIZE:HEADER_SIZE + payload_len]

    def hello(self):
        payload = struct.pack("<BBHIHHII", ROLE_DIAGNOSTIC_TOOL, 2, 0, 4056, 2, 4, 8, self.timeout_ms)
        payload += os.urandom(16) + struct.pack("<I", 0) + bytes([1, 2])
        self.send(TYPE_CONTROL, HELLO, payload)
        for msg_type, object_id, _ in self.frames(3.0):
            if msg_type == TYPE_CONTROL and object_id == HELLO_RESULT:
                return True
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port")
    parser.add_argument("commands", nargs="*", help="shell lines, e.g. \"btp -status\"")
    parser.add_argument("--wait", type=float, default=1.5, help="seconds to collect output after each line")
    parser.add_argument("--baud", type=int, default=921600, help="UART links only (USB ignores it)")
    args = parser.parse_intermixed_args()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.05
    port.write_timeout = 2  # a device not reading the port (the recovery) must not hang us
    port.dtr = False  # never strap the chip into download mode
    port.rts = False
    port.open()
    session = Session(port, timeout_ms=10000)
    try:
        opened = session.hello()
    except serial.SerialTimeoutException:
        opened = False
    if not opened:
        sys.exit("no HELLO_RESULT: is the device on the Serial link, in clear (no key)?")
    print("[session open]")
    for line in args.commands:
        print("> " + line)
        session.send(TYPE_TERMINAL, TERMINAL_IN, (line + "\n").encode())
        for msg_type, object_id, payload in session.frames(args.wait):
            if msg_type == TYPE_TERMINAL and object_id == TERMINAL_OUT:
                sys.stdout.write(payload.decode("utf-8", "replace"))
                sys.stdout.flush()
    port.close()


if __name__ == "__main__":
    main()

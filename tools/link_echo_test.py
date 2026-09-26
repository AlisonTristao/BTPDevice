#!/usr/bin/env python3
"""PC side of test_apps/link_echo: sends valid BTP frames over a link and
checks that each one comes back byte for byte.

    python tools/link_echo_test.py serial COM5
    python tools/link_echo_test.py tcp btpdev-echo.local        (or an IP)
    python tools/link_echo_test.py ble btpdev-echo              (device name)

Options: --count N (frames, default 200), --max-payload N (default 1000).

Frames use the stream framing every direct link uses: 0x00 || COBS(frame) ||
0x00. Payload sizes cycle from 0 to --max-payload and include 0x00 bytes, so
COBS and fragmentation of the byte stream (USB packets, TCP segments, BLE
notifications) are exercised. Reports lost/corrupt frames and round-trip
times. Needs pyserial for serial and bleak for BLE:

    pip install pyserial bleak
"""

import argparse
import asyncio
import socket
import statistics
import struct
import sys
import time
import zlib

MAGIC = b"BTP\x00"
HEADER_SIZE = 36
TYPE_TELEMETRY = 0x01

# BTP v1 BLE service (BTP/docs/fragmentation-and-transports.md section 8).
BLE_RX = "f9160b78-c242-42f4-8f5e-88df2c51cbe6"  # write
BLE_TX = "20f96ede-2f3b-4e02-b2cf-be6bb75dbe35"  # notify


def encode_frame(sequence, payload):
    header = MAGIC + struct.pack("<BBHHH", 1, TYPE_TELEMETRY, 0, HEADER_SIZE, len(payload))
    header += struct.pack("<IIIQHBB", 0xC0FFEE01, 1, sequence, int(time.monotonic() * 1e6),
                          0x0101, 0, 1)
    body = header + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def cobs_encode(data):
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data) + (1 if code == 1 else 0):
            raise ValueError("bad COBS block")
        out += data[i + 1:i + code]
        i += code
        if code != 255 and i < len(data):
            out.append(0)
    return bytes(out)


def wire(frame):
    return b"\x00" + cobs_encode(frame) + b"\x00"


class Deframer:
    """0x00-delimited stream -> decoded frames."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        frames = []
        for byte in data:
            if byte == 0:
                if self.buffer:
                    try:
                        frames.append(cobs_decode(bytes(self.buffer)))
                    except ValueError:
                        frames.append(None)
                    self.buffer.clear()
            else:
                self.buffer.append(byte)
        return frames


def payload_for(index, max_payload):
    size = (index * 37) % (max_payload + 1)
    return bytes((index + i) % 256 for i in range(size))


class Report:
    def __init__(self, count):
        self.count = count
        self.sent_at = {}
        self.rtts = []
        self.corrupt = 0
        self.unexpected = 0

    def expect(self, frame):
        self.sent_at[frame] = time.monotonic()

    def got(self, frame):
        if frame is None:
            self.corrupt += 1
            return
        sent = self.sent_at.pop(frame, None)
        if sent is None:
            self.unexpected += 1
        else:
            self.rtts.append((time.monotonic() - sent) * 1000.0)

    def done(self):
        return not self.sent_at

    def print(self):
        received = len(self.rtts)
        print(f"sent {self.count}, echoed {received}, lost {len(self.sent_at)}, "
              f"corrupt {self.corrupt}, unexpected {self.unexpected}")
        if self.rtts:
            print(f"round trip ms: min {min(self.rtts):.1f}  median "
                  f"{statistics.median(self.rtts):.1f}  max {max(self.rtts):.1f}")
        return received == self.count and self.corrupt == 0 and self.unexpected == 0


def run_blocking(send, receive, args):
    report = Report(args.count)
    deframer = Deframer()
    for index in range(args.count):
        frame = encode_frame(index, payload_for(index, args.max_payload))
        report.expect(frame)
        send(wire(frame))
        # One frame in flight at a time keeps the RTT meaningful.
        deadline = time.monotonic() + args.timeout
        while frame in report.sent_at and time.monotonic() < deadline:
            for got in deframer.feed(receive()):
                report.got(got)
    return report.print()


def run_serial(args):
    import serial  # pyserial

    port = serial.Serial(args.target, 115200, timeout=0.01)
    port.dtr = True  # the device opens its session on DTR (TraceView does the same)
    time.sleep(0.2)
    port.reset_input_buffer()
    try:
        return run_blocking(port.write, lambda: port.read(4096), args)
    finally:
        port.dtr = False
        port.close()


def run_tcp(args):
    host, _, port = args.target.partition(":")
    sock = socket.create_connection((host, int(port or 44300)), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(0.01)

    def receive():
        try:
            return sock.recv(8192)
        except socket.timeout:
            return b""

    try:
        return run_blocking(sock.sendall, receive, args)
    finally:
        sock.close()


async def run_ble_async(args):
    from bleak import BleakClient, BleakScanner

    print(f"scanning for \"{args.target}\"...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: (adv.local_name or d.name or "").strip().lower() == args.target.lower(),
        timeout=15)
    if device is None:
        print("device not found")
        return False
    report = Report(args.count)
    deframer = Deframer()
    async with BleakClient(device) as client:
        await client.start_notify(BLE_TX, lambda _, data: [report.got(f) for f in deframer.feed(data)])
        chunk = max(20, client.mtu_size - 3)
        for index in range(args.count):
            frame = encode_frame(index, payload_for(index, args.max_payload))
            report.expect(frame)
            data = wire(frame)
            for at in range(0, len(data), chunk):
                await client.write_gatt_char(BLE_RX, data[at:at + chunk], response=True)
            deadline = time.monotonic() + args.timeout
            while frame in report.sent_at and time.monotonic() < deadline:
                await asyncio.sleep(0.002)
    return report.print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("link", choices=["serial", "tcp", "ble"])
    parser.add_argument("target", help="COM port / host[:port] / BLE device name")
    parser.add_argument("--count", type=int, default=200)
    parser.add_argument("--max-payload", type=int, default=1000)
    parser.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for each echo")
    args = parser.parse_args()
    if args.link == "serial":
        ok = run_serial(args)
    elif args.link == "tcp":
        ok = run_tcp(args)
    else:
        ok = asyncio.run(run_ble_async(args))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

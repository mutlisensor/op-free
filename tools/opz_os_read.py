#!/usr/bin/env python3
"""opz_os_read — read the installed OS (`/yaffs2/os.ldr`) off an OP-Z in update mode, with no firmware key.

Hardware check H-U1 (research/update-console.md §8). The bootloader's console command 0x19 opens
`/yaffs2/<name>` read-only and sends the file back in one frame of type 0x20 (Q-52).

    opz_os_read.py frames                      # print every frame this tool can send; opens no port
    opz_os_read.py ping   [--port P] [--dry-run]
    opz_os_read.py stats  [--port P] [--dry-run] [--out-dir D]
    opz_os_read.py list   [--port P] [--dry-run] [--out-dir D]
    opz_os_read.py read os.crc32|os.ldr [--port P] [--dry-run] [--out-dir D]

SAFETY (SAFETY.md rule 1: the console is a root shell; type 0x15 formats the NAND on its header alone)
  The tool can send exactly the frames in ALLOWED_FRAMES, built once at import from fixed parts:
    C0 01 00 00000000            ping
    C0 01 27 00000000            yaffs2 statistics (text)
    C0 01 21 00000001 02         file listing with sizes (text)
    C0 01 19 <len> <name> 00     read a file, <name> one of READ_NAMES
  `encode()` refuses any type outside ALLOWED_TYPES and any name outside READ_NAMES; `Device.send()` refuses any
  byte string that is not one of ALLOWED_FRAMES. Nothing typed on the command line reaches a frame except the
  choice of one of these. None of these commands writes to flash [code, update-console.md §3–§5].

REPLIES
  C0 1E <type+1> <len u32 BE> <payload>. The device escapes C0/7D (as 7D, byte ^ 0x20) in the header as well as
  the payload. Log lines arrive in between as type 0x04 frames. A 0x19 for a missing file gets no reply at all.

Output goes to a git-ignored folder (default dumps/os-read-<UTC time>/ in the repository). Needs pyserial
(`python3 -m pip install pyserial`) except for `frames` and --dry-run.
"""

from __future__ import annotations

import argparse
import datetime
import glob
import hashlib
import os
import re
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ESCAPE, ESCAPE_XOR, START = 0x7D, 0x20, 0xC0
ROUTING_HOST, ROUTING_DEV = 0x01, 0x1E
TYPE_PING, TYPE_READ_FILE, TYPE_LIST, TYPE_STATS = 0x00, 0x19, 0x21, 0x27
TYPE_LOG = 0x04
ALLOWED_TYPES = frozenset((TYPE_PING, TYPE_READ_FILE, TYPE_LIST, TYPE_STATS))
REPLY_TYPE = {TYPE_PING: 0x01, TYPE_READ_FILE: 0x20, TYPE_LIST: 0x22, TYPE_STATS: 0x28}
LIST_ARG_SIZES = 0x02          # bit 1 = sizes; bit 0 (CRC of every file) is never sent
READ_NAMES = ('os.ldr', 'os.crc32')
MAX_NAME = 32

# What 1.2.45 installs (from the decrypted update's opz.ldr and opz.crc32) [proven: computed]
EXPECTED = {
    'os.ldr': {
        'size': 828136,
        'crc32': 0x8F472BBD,
        'sha256': '5be99760c8797bdd4797ed20cfb9a92b6588889d3a81109bae5a45646a9bde07',
    },
    'os.crc32': {'text': '2403806141'},
}


def escape(data):
    out = bytearray()
    for b in data:
        if b in (START, ESCAPE):
            out += bytes((ESCAPE, b ^ ESCAPE_XOR))
        else:
            out.append(b)
    return bytes(out)


def encode(mtype, payload=b''):
    """C0, routing, type, u32 BE length, then everything after C0 escaped (the device un-escapes every byte)."""
    if mtype not in ALLOWED_TYPES:
        raise ValueError(f'REFUSED: type 0x{mtype:02x} is not one of {sorted(ALLOWED_TYPES)}')
    if mtype == TYPE_READ_FILE:
        name = payload[:-1].decode('ascii', 'replace')
        if payload[-1:] != b'\x00' or name not in READ_NAMES:
            raise ValueError(f'REFUSED: 0x19 name {payload!r} is not one of {READ_NAMES}')
    elif mtype == TYPE_LIST:
        if payload != bytes((LIST_ARG_SIZES,)):
            raise ValueError(f'REFUSED: 0x21 argument {payload!r}; only 0x{LIST_ARG_SIZES:02x}')
    elif payload:
        raise ValueError(f'REFUSED: type 0x{mtype:02x} takes no payload')
    body = bytes((ROUTING_HOST, mtype)) + len(payload).to_bytes(4, 'big') + payload
    return bytes((START,)) + escape(body)


def read_frame(name):
    if name not in READ_NAMES:
        raise ValueError(f'REFUSED: {name!r} is not one of {READ_NAMES}')
    raw = name.encode('ascii')
    assert 0 < len(raw) <= MAX_NAME and b'\x00' not in raw
    return encode(TYPE_READ_FILE, raw + b'\x00')


PING = encode(TYPE_PING)
STATS = encode(TYPE_STATS)
LIST = encode(TYPE_LIST, bytes((LIST_ARG_SIZES,)))
READ = {name: read_frame(name) for name in READ_NAMES}
ALLOWED_FRAMES = frozenset((PING, STATS, LIST, *READ.values()))


class FrameReader:
    """Splits the device's byte stream into (routing, type, payload); un-escapes header and payload."""

    def __init__(self):
        self.frames = []
        self._body = None       # un-escaped bytes after C0, or None while outside a frame
        self._esc = False

    def feed(self, data):
        for b in data:
            if b == START:
                self._body, self._esc = bytearray(), False
                continue
            if self._body is None:
                continue
            if self._esc:
                b ^= ESCAPE_XOR
                self._esc = False
            elif b == ESCAPE:
                self._esc = True
                continue
            self._body.append(b)
            body = self._body
            if len(body) >= 6 and len(body) == 6 + int.from_bytes(body[2:6], 'big'):
                self.frames.append((body[0], body[1], bytes(body[6:])))
                self._body = None

    def pending(self):
        """(type, announced length, bytes so far) of a frame still arriving, or None."""
        b = self._body
        if b is None or len(b) < 6:
            return None
        return b[1], int.from_bytes(b[2:6], 'big'), len(b) - 6

    def take(self):
        out, self.frames = self.frames, []
        return out


class Device:
    def __init__(self, port=None, dry_run=False, out=print, transport=None):
        self.dry_run, self.out = dry_run, out
        self.reader = FrameReader()
        self.port = transport
        if self.port is None and not dry_run:
            try:
                import serial
            except ImportError:
                raise SystemExit('pyserial is missing: python3 -m pip install pyserial')
            self.port = serial.Serial(port, 115200, timeout=0.2)

    def send(self, frame):
        if frame not in ALLOWED_FRAMES:
            raise ValueError(f'REFUSED: {frame.hex(" ")} is not an allowed frame')
        self.out(f'  >> {frame.hex(" ")}')
        if not self.dry_run:
            self.port.write(frame)
            self.port.flush()

    def collect(self, want_type, first_timeout, idle_timeout, progress=False):
        """Read until a frame of want_type is complete. Returns (payload or None, log lines, partial length)."""
        logs, start = [], time.monotonic()
        last, started, pend = start, False, None
        while True:
            chunk = self.port.read(65536)
            now = time.monotonic()
            if chunk:
                last = now
                self.reader.feed(chunk)
            for r, t, p in self.reader.take():
                if t == TYPE_LOG:
                    logs.append(p.split(b'\x00', 1)[0].decode('utf-8', 'replace'))
                elif t == want_type and r == ROUTING_DEV:
                    if progress:
                        self.out('')
                    return p, logs, len(p)
                else:
                    self.out(f'  << ignored frame r=0x{r:02x} type=0x{t:02x} len={len(p)}')
            pend = self.reader.pending()
            if pend and pend[0] == want_type:
                started = True
                if progress:
                    print(f'\r  {pend[2]} / {pend[1]} bytes', end='', flush=True)
            if started and now - last > idle_timeout:
                if progress:
                    self.out('')
                return None, logs, pend[2] if pend and pend[0] == want_type else 0
            if not started and now - start > first_timeout:
                return None, logs, 0

    def request(self, frame, mtype, first_timeout=5.0, idle_timeout=3.0, progress=False):
        self.send(frame)
        if self.dry_run:
            return None, [], 0
        return self.collect(REPLY_TYPE[mtype], first_timeout, idle_timeout, progress)

    def ping(self, timeout=2.0):
        payload, _, _ = self.request(PING, TYPE_PING, first_timeout=timeout)
        return payload == b''

    def drain(self):
        if not self.dry_run:
            self.port.reset_input_buffer()
        self.reader = FrameReader()

    def close(self):
        if self.port is not None and not self.dry_run:
            self.port.close()


def find_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if len(ports) != 1:
        raise SystemExit(f'expected exactly one /dev/cu.usbmodem* port, found {ports}; pass --port')
    return ports[0]


def check(name, data):
    """Compare a read file with what 1.2.45 installs. Returns (matches, report lines)."""
    exp = EXPECTED[name]
    lines = [f'size {len(data)}  crc32 {zlib.crc32(data)} (0x{zlib.crc32(data):08X})  '
             f'sha256 {hashlib.sha256(data).hexdigest()}']
    if 'text' in exp:
        text = data.decode('ascii', 'replace').strip()
        ok = text == exp['text']
        lines.append(f'content {text!r}; 1.2.45: {exp["text"]!r} -> {"MATCH" if ok else "DIFFERENT"}')
        return ok, lines
    ok = (len(data) == exp['size'] and zlib.crc32(data) == exp['crc32']
          and hashlib.sha256(data).hexdigest() == exp['sha256'])
    lines.append(f'1.2.45 opz.ldr: size {exp["size"]} crc32 {exp["crc32"]} sha256 {exp["sha256"]} -> '
                 f'{"MATCH" if ok else "DIFFERENT"}')
    return ok, lines


def page_writes(stats_text):
    m = re.search(r'n_page_writes\.*\s*(-?\d+)', stats_text)
    return int(m.group(1)) if m else None


def out_dir(args):
    d = Path(args.out_dir) if args.out_dir else \
        ROOT / 'dumps' / f'os-read-{datetime.datetime.now(datetime.timezone.utc):%Y%m%dT%H%M%SZ}'
    d.mkdir(parents=True, exist_ok=True)
    return d


def start(dev):
    """The port-open reset eats the first frame written (notes), so ping twice and require the second pong."""
    dev.out('PING (warm-up)')
    dev.ping(timeout=1.0)
    dev.drain()
    dev.out('PING (liveness)')
    if dev.dry_run:
        dev.send(PING)
        return
    if not dev.ping():
        raise SystemExit('no PONG: not sending anything else. Is the OP-Z in update mode (hold SCREEN at power-on)?')
    dev.out('  pong')


def show_logs(dev, logs):
    for line in logs:
        dev.out(f'  << LOG {line}')


def cmd_ping(dev, args):
    start(dev)


def cmd_stats(dev, args):
    start(dev)
    dev.out('0x27 yaffs2 statistics')
    payload, logs, _ = dev.request(STATS, TYPE_STATS)
    show_logs(dev, logs)
    if dev.dry_run:
        return
    if payload is None:
        raise SystemExit('no statistics reply')
    text = payload.decode('utf-8', 'replace')
    path = out_dir(args) / 'stats.txt'
    path.write_text(text)
    dev.out(text)
    dev.out(f'n_page_writes {page_writes(text)}; saved {path}')


def cmd_list(dev, args):
    start(dev)
    dev.out('0x21 file listing with sizes')
    payload, logs, _ = dev.request(LIST, TYPE_LIST, first_timeout=15.0)
    show_logs(dev, logs)
    if dev.dry_run:
        return
    if payload is None:
        raise SystemExit('no listing reply')
    text = payload.decode('utf-8', 'replace')
    path = out_dir(args) / 'listing.txt'
    path.write_text(text)
    hits = [l for l in text.splitlines() if l.split(':')[0] in ('/yaffs2/os.ldr', '/yaffs2/os.crc32')]
    dev.out('\n'.join(hits) if hits else 'no /yaffs2/os.ldr or os.crc32 line in the listing')
    if text.endswith('.....'):
        dev.out('(the listing was cut at 32 KB)')
    dev.out(f'{len(text.splitlines())} lines; saved {path}')


def cmd_read(dev, args):
    start(dev)
    dev.out(f'0x19 read /yaffs2/{args.name}')
    payload, logs, got = dev.request(READ[args.name], TYPE_READ_FILE, first_timeout=5.0, idle_timeout=3.0,
                                     progress=args.name == 'os.ldr')
    show_logs(dev, logs)
    if dev.dry_run:
        return 0
    if payload is None:
        if got:
            raise SystemExit(f'short read: {got} bytes, then silence; nothing saved. Read once more before judging')
        raise SystemExit(f'no reply in 5 s: /yaffs2/{args.name} is missing (the bootloader answers nothing then)')
    path = out_dir(args) / args.name
    path.write_bytes(payload)
    ok, lines = check(args.name, payload)
    for line in lines:
        dev.out(line)
    dev.out(f'saved {path}')
    return 0 if ok else 1


def cmd_frames(args):
    print('Every frame opz_os_read can send (nothing else passes Device.send):')
    for label, frame in (('ping', PING), ('stats', STATS), ('list', LIST),
                         *((f'read {n}', READ[n]) for n in READ_NAMES)):
        print(f'  {label:14s} {frame.hex(" ")}')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('frames')
    for c in ('ping', 'stats', 'list', 'read'):
        p = sub.add_parser(c)
        if c == 'read':
            p.add_argument('name', choices=READ_NAMES)
        p.add_argument('--port')
        p.add_argument('--dry-run', action='store_true')
        p.add_argument('--out-dir')
    args = ap.parse_args(argv)
    if args.cmd == 'frames':
        cmd_frames(args)
        return 0
    dev = Device(None if args.dry_run else (args.port or find_port()), dry_run=args.dry_run)
    try:
        return {'ping': cmd_ping, 'stats': cmd_stats, 'list': cmd_list, 'read': cmd_read}[args.cmd](dev, args) or 0
    finally:
        dev.close()


if __name__ == '__main__':
    sys.exit(main())

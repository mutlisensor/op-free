"""Offline tests for tools/opz_os_read.py against a fake update-mode console. Run:
    python3 -m unittest discover -s tools/tests
"""

import hashlib
import io
import os
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import opz_os_read as osr  # noqa: E402


def unescape(data):
    out, esc = bytearray(), False
    for b in data:
        if esc:
            out.append(b ^ 0x20)
            esc = False
        elif b == 0x7D:
            esc = True
        else:
            out.append(b)
    return bytes(out)


def reply(mtype, payload):
    """A frame as the bootloader sends it: header and payload escaped."""
    body = bytes((0x1E, mtype)) + len(payload).to_bytes(4, 'big') + payload
    return b'\xc0' + osr.escape(body)


def log(text):
    return reply(0x04, text.encode() + b'\x00')


class Clock:
    def __init__(self):
        self.t = 0.0

    def monotonic(self):
        return self.t


class FakeConsole:
    """Parses what the tool writes like the bootloader's parser and queues replies; read() hands them out in pieces."""

    def __init__(self, clock, files, eat_first=True, chunk=1000, stall_after=None):
        self.clock, self.files, self.eat_first, self.chunk = clock, files, eat_first, chunk
        self.stall_after = stall_after
        self.written, self.queue, self.sent = [], bytearray(), 0

    def write(self, frame):
        self.written.append(frame)
        if self.eat_first:
            self.eat_first = False
            return
        assert frame[0] == 0xC0
        body = unescape(frame[1:])
        routing, mtype, length, payload = body[0], body[1], int.from_bytes(body[2:6], 'big'), body[6:]
        assert routing == 0x01 and length == len(payload)
        if mtype == 0x00:
            self.queue += reply(0x01, b'')
        elif mtype == 0x27:
            self.queue += reply(0x28, b'--- stats ---\nn_page_writes.............  1234\nn_page_reads.....    9\n')
        elif mtype == 0x21:
            assert payload == b'\x02'
            self.queue += log('listing') + reply(0x22, b'/yaffs2/system/\n/yaffs2/os.ldr: 828136 \n'
                                                      b'/yaffs2/os.crc32: 10 \n')
        elif mtype == 0x19:
            name = payload[:-1].decode()
            assert payload.endswith(b'\x00')
            if name in self.files:
                self.queue += log('reading') + reply(0x20, self.files[name])
        else:
            raise AssertionError(f'unexpected type 0x{mtype:02x}')

    def flush(self):
        pass

    @property
    def in_waiting(self):
        k = min(self.chunk, len(self.queue))
        if self.stall_after is not None:
            k = min(k, max(0, self.stall_after - self.sent))
        return k

    def read(self, n):
        if self.stall_after is not None and self.sent >= self.stall_after:
            self.clock.t += 0.5
            return b''
        if not self.queue:
            self.clock.t += 0.5
            return b''
        k = min(n, self.chunk, len(self.queue))
        if self.stall_after is not None:
            k = min(k, self.stall_after - self.sent)
        out = bytes(self.queue[:k])
        del self.queue[:k]
        self.sent += k
        self.clock.t += 0.01
        return out

    def reset_input_buffer(self):
        self.queue.clear()

    def close(self):
        pass


class Frames(unittest.TestCase):
    def test_exact_frames(self):
        self.assertEqual(osr.PING, bytes.fromhex('c0 01 00 00 00 00 00'))
        self.assertEqual(osr.STATS, bytes.fromhex('c0 01 27 00 00 00 00'))
        self.assertEqual(osr.LIST, bytes.fromhex('c0 01 21 00 00 00 01 02'))
        self.assertEqual(osr.READ['os.ldr'], bytes.fromhex('c0 01 19 00 00 00 07') + b'os.ldr\x00')
        self.assertEqual(osr.READ['os.crc32'], bytes.fromhex('c0 01 19 00 00 00 09') + b'os.crc32\x00')
        self.assertEqual(len(osr.ALLOWED_FRAMES), 5)

    def test_every_other_type_refused(self):
        for t in range(256):
            if t in osr.ALLOWED_TYPES:
                continue
            with self.assertRaises(ValueError, msg=f'type 0x{t:02x}'):
                osr.encode(t)
            with self.assertRaises(ValueError):
                osr.encode(t, b'os.ldr\x00')
        self.assertEqual(osr.ALLOWED_TYPES, frozenset((0x00, 0x19, 0x21, 0x27)))

    def test_payloads_refused(self):
        for bad in (b'os.ldr', b'system/os.ldr\x00', b'../os.ldr\x00', b'x\x00', b''):
            with self.assertRaises(ValueError):
                osr.encode(0x19, bad)
        with self.assertRaises(ValueError):
            osr.read_frame('settings/plugs.json')
        for bad in (b'', b'\x01', b'\x03', b'\x02\x00'):
            with self.assertRaises(ValueError):
                osr.encode(0x21, bad)
        for t in (0x00, 0x27):
            with self.assertRaises(ValueError):
                osr.encode(t, b'\x00')

    def test_no_frame_contains_a_dangerous_type_in_type_position(self):
        for f in osr.ALLOWED_FRAMES:
            self.assertEqual(f[0], 0xC0)
            self.assertEqual(f[1], 0x01)
            self.assertIn(f[2], osr.ALLOWED_TYPES)
            self.assertEqual(f.count(0xC0), 1)

    def test_send_refuses_unlisted_bytes(self):
        dev = osr.Device(dry_run=True, out=lambda s: None)
        for frame in (bytes.fromhex('c0 01 15 00 00 00 00'), osr.PING + b'\x00', b''):
            with self.assertRaises(ValueError):
                dev.send(frame)
        for frame in osr.ALLOWED_FRAMES:
            dev.send(frame)


class Reader(unittest.TestCase):
    def test_escaped_header_and_payload(self):
        payload = bytes(range(256)) * 2 + b'\xc0\x7d' * 30          # 572 = 0x23C; payload full of C0/7D
        r = osr.FrameReader()
        stream = log('hi') + reply(0x20, payload)
        for i in range(0, len(stream), 7):
            r.feed(stream[i:i + 7])
        frames = r.take()
        self.assertEqual([(t, p) for _, t, p in frames][1], (0x20, payload))
        self.assertEqual(frames[0][1], 0x04)

    def test_length_byte_7d_in_header(self):
        payload = b'a' * 0x7D                                           # length 0000007D arrives escaped
        r = osr.FrameReader()
        r.feed(reply(0x20, payload))
        self.assertEqual(r.take(), [(0x1E, 0x20, payload)])

    def test_new_start_byte_resets(self):
        r = osr.FrameReader()
        r.feed(b'\xc0\x1e\x20\x00\x00\x01')                             # cut off
        r.feed(reply(0x01, b''))
        self.assertEqual(r.take(), [(0x1E, 0x01, b'')])


class Commands(unittest.TestCase):
    def run_cmd(self, argv, files, **fake):
        clock = Clock()
        console = FakeConsole(clock, files, **fake)
        lines = []
        with tempfile.TemporaryDirectory() as d, mock.patch.object(osr, 'time', clock), \
                mock.patch.object(osr, 'find_port', lambda: '/dev/fake'), \
                mock.patch.object(osr, 'Device',
                                  lambda port, dry_run=False: RealDevice(port, dry_run, lines.append, console)):
            code, err = None, None
            try:
                with redirect_stdout(io.StringIO()):
                    code = osr.main(argv + ['--out-dir', d])
            except SystemExit as e:
                err = str(e)
            saved = {n: Path(d, n).read_bytes() for n in os.listdir(d)}
        return code, err, lines, console, saved

    def test_read_os_ldr_match(self):
        data = os.urandom(5000)
        exp = {'size': len(data), 'crc32': zlib.crc32(data), 'sha256': hashlib.sha256(data).hexdigest()}
        with mock.patch.dict(osr.EXPECTED, {'os.ldr': exp}):
            code, err, lines, console, saved = self.run_cmd(['read', 'os.ldr'], {'os.ldr': data})
        self.assertIsNone(err)
        self.assertEqual(code, 0)
        self.assertEqual(saved['os.ldr'], data)
        self.assertTrue(any('MATCH' in l and 'DIFFERENT' not in l for l in lines))
        self.assertTrue(all(f in osr.ALLOWED_FRAMES for f in console.written))
        self.assertEqual(console.written, [osr.PING, osr.PING, osr.READ['os.ldr']])

    def test_read_os_ldr_different(self):
        code, err, lines, _, saved = self.run_cmd(['read', 'os.ldr'], {'os.ldr': b'not the os'})
        self.assertEqual(code, 1)
        self.assertTrue(any('DIFFERENT' in l for l in lines))
        self.assertEqual(saved['os.ldr'], b'not the os')

    def test_read_crc32_text(self):
        code, err, lines, _, _ = self.run_cmd(['read', 'os.crc32'], {'os.crc32': b'2403806141'})
        self.assertEqual(code, 0)

    def test_missing_file_is_silence(self):
        code, err, _, console, saved = self.run_cmd(['read', 'os.ldr'], {})
        self.assertIn('missing', err)
        self.assertEqual(saved, {})

    def test_short_read_saves_nothing(self):
        code, err, _, _, saved = self.run_cmd(['read', 'os.ldr'], {'os.ldr': b'x' * 4000},
                                              stall_after=7 + 7 + 2000)
        self.assertIn('short read', err)
        self.assertEqual(saved, {})

    def test_no_pong_sends_nothing_else(self):
        clock = Clock()
        console = FakeConsole(clock, {})
        console.write = lambda frame: console.written.append(frame)       # a console that never answers
        dev = RealDevice('/dev/fake', False, lambda s: None, console)
        with mock.patch.object(osr, 'time', clock), self.assertRaises(SystemExit):
            osr.cmd_read(dev, type('A', (), {'name': 'os.ldr', 'out_dir': None})())
        self.assertEqual(console.written, [osr.PING, osr.PING])

    def test_stats_and_list(self):
        code, err, lines, console, saved = self.run_cmd(['stats'], {})
        self.assertIsNone(err)
        self.assertIn('n_page_writes 1234', '\n'.join(lines))
        self.assertIn('stats.txt', saved)
        code, err, lines, console, saved = self.run_cmd(['list'], {})
        self.assertIn('/yaffs2/os.ldr: 828136', '\n'.join(lines))
        self.assertEqual(console.written[-1], osr.LIST)

    def test_dry_run_opens_no_port(self):
        lines = []
        with mock.patch.object(osr, 'find_port', side_effect=AssertionError('port opened')):
            dev = osr.Device(None, dry_run=True, out=lines.append)
            osr.cmd_read(dev, type('A', (), {'name': 'os.ldr', 'out_dir': None})())
        self.assertIn('  >> ' + osr.READ['os.ldr'].hex(' '), lines)

    def test_expected_matches_the_note(self):
        self.assertEqual(osr.EXPECTED['os.ldr']['crc32'], 2403806141)
        self.assertEqual(str(osr.EXPECTED['os.ldr']['crc32']), osr.EXPECTED['os.crc32']['text'])


RealDevice = osr.Device


if __name__ == '__main__':
    unittest.main()

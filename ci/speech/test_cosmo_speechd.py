# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import os
import struct
import threading
import unittest

_spec = importlib.util.spec_from_file_location(
    'speechd', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cosmo-speechd.py'))
speechd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(speechd)


def h(s):
    return bytes.fromhex(s.replace(' ', ''))


def md_mailbox(msg_id, p16=0, p32=0, seq=0x12):
    return struct.pack('<IHHII', 0xFFFFFFFF, p16, msg_id, 4 | seq << 16, p32)


class FrameTest(unittest.TestCase):
    def test_mailbox_sph_off(self):
        # speech-gen93.org 1.3: magic, param16, msg_id, channel 5, param32
        self.assertEqual(speechd.pack_mailbox(0x2F21), h('ffffffff 0000 212f 05000000 00000000'))

    def test_mailbox_ul_mute(self):
        self.assertEqual(speechd.pack_mailbox(0x2F02, 1), h('ffffffff 0100 022f 05000000 00000000'))
        self.assertEqual(speechd.pack_mailbox(0x2F02, 0), h('ffffffff 0000 022f 05000000 00000000'))

    def test_mailbox_as_ccci_header(self):
        # viewed as ccci_header: data[1] = msg_id << 16 | param16, reserved = param32
        d0, d1, ch, res = struct.unpack('<IIII', speechd.pack_mailbox(0x2F00, 0xFFF4, 0x1234))
        self.assertEqual((d0, d1, ch, res), (0xFFFFFFFF, 0x2F00FFF4, 5, 0x1234))

    def test_sph_info(self):
        info = speechd.sph_info(32000)
        self.assertEqual(len(info), 128)
        # application 0, bt 0, rate enum 2, opendsp 0, param path 1 (shm via CCCI), valid 0
        self.assertEqual(info[:6], h('00 00 02 00 01 00'))
        self.assertEqual(info[6:], bytes(122))
        self.assertEqual(speechd.sph_info(8000)[2], 0)
        self.assertEqual(speechd.sph_info(16000)[2], 1)
        self.assertEqual(speechd.sph_info(32000, ext_dev=6)[10:12], h('0600'))

    def test_sph_on_payload(self):
        frame = speechd.pack_payload(0x2F20, 25, speechd.sph_info(32000))
        self.assertEqual(len(frame), 128 + 22)
        # d0 0, d1 size+6 (the kernel writes 150), ch 5, len size+6, id, 0xA2A2, type 25, size 128
        self.assertEqual(frame[:22], h('00000000 86000000 05000000 8600 202f a2a2 1900 8000'))
        self.assertEqual(frame[22:28], h('00 00 02 00 01 00'))

    def test_payload_limit(self):
        speechd.pack_payload(0x2F20, 25, bytes(0xD6A))
        with self.assertRaises(ValueError):
            speechd.pack_payload(0x2F20, 25, bytes(0xD6B))

    def test_parse_md_mailbox(self):
        f = speechd.parse(md_mailbox(0xAF20))
        self.assertEqual((f.kind, f.msg_id, f.p16, f.p32), ('mailbox', 0xAF20, 0, 0))
        f = speechd.parse(md_mailbox(0xAF75, 3, 7))
        self.assertEqual((f.msg_id, f.p16, f.p32), (0xAF75, 3, 7))

    def md_payload(self, msg_id, dtype, data, sync=0x1234, d1=None):
        n = len(data) + 26
        return struct.pack('<IIIHHHHHHH', 0, n if d1 is None else d1, 4, len(data) + 10, msg_id,
                           sync, dtype, len(data), 1, 1) + data

    def test_parse_md_payload(self):
        data = b'AMR-WB'.ljust(92, b'\0') + b'HD'.ljust(92, b'\0')
        f = speechd.parse(self.md_payload(0xAF90, 20, data))
        self.assertEqual((f.kind, f.msg_id, f.data_type, f.idx, f.total), ('payload', 0xAF90, 20, 1, 1))
        self.assertEqual(f.data, data)
        self.assertEqual(speechd.cstr(f.data[:92]), 'AMR-WB')

    def test_parse_rejects(self):
        with self.assertRaises(ValueError):
            speechd.parse(bytes(15))
        with self.assertRaises(ValueError):
            speechd.parse(self.md_payload(0xAF90, 20, bytes(8), sync=0xA2A2))
        with self.assertRaises(ValueError):
            speechd.parse(self.md_payload(0xAF90, 20, bytes(8), d1=40))

    def test_names(self):
        self.assertEqual(speechd.msg_name(0xAF20), 'SPH_ON_ACK')
        self.assertEqual(speechd.msg_name(0xAFA0), 'MD_ALIVE')
        self.assertEqual(speechd.msg_name(0x2F99), '2F99')


class PcmParamsTest(unittest.TestCase):
    def test_sizes(self):
        if speechd.UL == 8:
            self.assertEqual(speechd.HW_PARAMS_SIZE, 608)
            self.assertEqual(speechd.SW_PARAMS.size, 136)
            self.assertEqual(speechd.PCM_IOCTL_HW_PARAMS, 0xC2604111)
            self.assertEqual(speechd.PCM_IOCTL_SW_PARAMS, 0xC0884113)
        self.assertEqual(speechd.PCM_IOCTL_START, 0x4142)

    def test_hw_params(self):
        b = speechd.hw_params(2, 32000, 1024, 2)
        self.assertEqual(struct.unpack_from('<III', b, 4 + 8 * 32 + 12 * 3), (32000, 32000, 4))
        self.assertEqual(struct.unpack_from('<III', b, 4 + 8 * 32 + 12 * 2), (2, 2, 4))
        self.assertEqual(struct.unpack_from('<I', b, 4), (8,))		# RW_INTERLEAVED
        self.assertEqual(struct.unpack_from('<I', b, 36), (4,))		# S16_LE


class FakePcm:
    def __init__(self):
        self.events = []

    def open(self, rate):
        self.events.append(('open', rate))

    def close(self):
        self.events.append(('close',))


class SpeechTest(unittest.TestCase):
    """The start and stop sequences against a scripted modem on a socket pair."""

    def setUp(self):
        import socket
        self.ap, self.md = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.pcm = FakePcm()
        self.log = []
        self.sp = speechd.Speech(self.ap.fileno(), self.log.append, self.pcm, 1.0)
        self.sent = []
        self.stop = False
        self.modem = threading.Thread(target=self.run_modem, daemon=True)
        self.modem.start()
        threading.Thread(target=self.run_reader, daemon=True).start()

    def tearDown(self):
        self.md.close()
        self.ap.close()

    def run_reader(self):
        while True:
            try:
                raw = self.ap.recv(4096)
            except OSError:
                return
            if not raw:
                return
            self.sp.handle(raw)

    def run_modem(self):
        while True:
            try:
                raw = self.md.recv(4096)
            except OSError:
                return
            if not raw:
                return
            msg_id = struct.unpack_from('<H', raw, 6 if raw[:4] == b'\xff' * 4 else 14)[0]
            self.sent.append(msg_id)
            if msg_id in speechd.NEED_ACK:
                self.md.send(md_mailbox(msg_id | 0x8000))

    def test_start_stop(self):
        self.assertEqual(self.sp.command('start'), 'ok speech on at 32000 Hz')
        self.assertEqual(self.sent, [0x2F20, 0x2F02])
        self.assertEqual(self.pcm.events, [('open', 32000)])
        self.assertEqual(self.sp.command('mute on'), 'ok UL muted')
        self.assertEqual(self.sp.command('stop'), 'ok speech off')
        self.assertEqual(self.sent, [0x2F20, 0x2F02, 0x2F02, 0x2F02, 0x2F21])
        self.assertEqual(self.pcm.events, [('open', 32000), ('close',)])

    def test_rejects(self):
        self.assertTrue(self.sp.command('start 44100').startswith('error'))
        self.assertTrue(self.sp.command('mute on').startswith('error'))
        self.assertEqual(self.sent, [])

    def test_modem_messages_answered(self):
        import time
        for m in (0xAFA0, 0xAF78, 0xAFA0, 0xAF90):
            self.md.send(md_mailbox(m))
        end = time.monotonic() + 2
        while len(self.sent) < 4 and time.monotonic() < end:
            time.sleep(0.01)
        self.assertEqual(self.sent, [0x2FA0, 0x2F78, 0x2FA0, 0x2F90])

    def test_epof_drops(self):
        self.sp.handle(md_mailbox(0xAF78))
        self.sp.on = True
        self.assertEqual(self.sp.command('mute off'), 'error: not acked')


if __name__ == '__main__':
    unittest.main()

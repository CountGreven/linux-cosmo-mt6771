# SPDX-License-Identifier: GPL-2.0-only
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import mdfsd  # noqa: E402


def wide(s):
    return (s + '\0').encode('utf-16-le')


def request(op, args, slot=0, d0=0):
    body = struct.pack('<I', len(args))
    for a in args:
        body += struct.pack('<I', len(a)) + a + b'\0' * (-len(a) & 3)
    return mdfsd.HDR.pack(d0, 20 + len(body), 14, slot) + struct.pack('<I', op) + body


def reply_args(msgs):
    """Reassemble and parse a reply the way the modem does."""
    body = b''.join(m[20:] for m in msgs)
    n, pos, args = struct.unpack_from('<I', body)[0], 4, []
    for _ in range(n):
        ln = struct.unpack_from('<I', body, pos)[0]
        args.append(body[pos + 4:pos + 4 + ln])
        pos += 4 + ((ln + 3) & ~3)
    assert pos == len(body)
    return args


class MdfsdTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        os.makedirs(self.root + '/mnt/vendor/nvdata/md/NVRAM/NVD_IMEI')
        with open(self.root + '/mnt/vendor/nvdata/md/NVRAM/NVD_IMEI/MP0B_001', 'wb') as f:
            f.write(bytes(range(256)) * 20)
        self.svc = mdfsd.Service(self.root, '1_ulwctg_n', lambda s: None)

    def tearDown(self):
        self.tmp.cleanup()

    def call(self, op, args):
        hdr, got_op, payload = self.svc.reassemble(request(op, args))
        out, _ = self.svc.handle(got_op, self.svc.parse_args(payload))
        return self.svc.build(hdr, got_op, out)

    def test_wide_path_mapping(self):
        err, p = self.svc.map_name(wide('Z:\\NVRAM\\NVD_IMEI'))
        self.assertEqual(err, 0)
        self.assertEqual(p, self.root + '/mnt/vendor/nvdata/md/NVRAM/NVD_IMEI')
        self.assertEqual(self.svc.map_name(wide('Z:\\..\\x'))[0], mdfsd.FS_ACCESS_DENIED)
        self.assertEqual(self.svc.map_name(wide('Q:\\x'))[0], mdfsd.FS_PATH_NOT_FOUND)

    def test_getdiskinfo_reply(self):
        msgs = self.call(0x100E, [wide('Z:\\'), struct.pack('<I', 3)])
        self.assertEqual(len(msgs), 1)
        d0, ln, ch, slot = mdfsd.HDR.unpack_from(msgs[0])
        self.assertEqual((ln, ch, slot), (len(msgs[0]), 15, 0))
        self.assertEqual(struct.unpack_from('<I', msgs[0], 16)[0], 0xFFFF100E)
        args = reply_args(msgs)
        self.assertEqual(struct.unpack('<i', args[0])[0], 0)
        self.assertEqual(len(args[1]), 84)
        self.assertEqual(struct.unpack_from('<I', args[1], 28 + 5 * 4)[0], 512)

    def test_open_read_close(self):
        name = wide('Z:\\NVRAM\\NVD_IMEI\\MP0B_001')
        args = reply_args(self.call(0x1001, [name, struct.pack('<I', mdfsd.FS_READ_ONLY)]))
        h = struct.unpack('<i', args[0])[0]
        self.assertEqual(h, 1)
        args = reply_args(self.call(0x1003, [struct.pack('<i', h), struct.pack('<i', 5000)]))
        self.assertEqual(struct.unpack('<i', args[0])[0], 0)
        self.assertEqual(struct.unpack('<I', args[1])[0], 5000)
        self.assertEqual(args[2], (bytes(range(256)) * 20)[:5000])
        args = reply_args(self.call(0x1005, [struct.pack('<i', h)]))
        self.assertEqual(struct.unpack('<i', args[0])[0], 0)
        self.assertEqual(self.svc.handles, {})

    def test_open_missing_file(self):
        args = reply_args(self.call(0x1001, [wide('Z:\\nope'), struct.pack('<I', 0x100)]))
        self.assertEqual(struct.unpack('<i', args[0])[0], mdfsd.FS_FILE_NOT_FOUND)

    def test_large_reply_is_fragmented(self):
        msgs = self.svc.build(mdfsd.HDR.pack(0, 0, 14, 2), 0x1003, [b'\x00' * 4, b'\x11' * 8000])
        self.assertEqual(len(msgs), 3)
        for m in msgs[:-1]:
            d0, ln, ch, slot = mdfsd.HDR.unpack_from(m)
            self.assertTrue(d0 & mdfsd.FS_REQ_SEND_AGAIN)
            self.assertEqual(ln, 20 + mdfsd.MAX_FS_PKT_BYTE)
            self.assertEqual(slot, 2)
        self.assertFalse(mdfsd.HDR.unpack_from(msgs[-1])[0] & mdfsd.FS_REQ_SEND_AGAIN)
        self.assertEqual(reply_args(msgs)[1], b'\x11' * 8000)

    def test_fragmented_request_is_reassembled(self):
        whole = request(0x1004, [struct.pack('<i', 1), b'\x22' * 5000, struct.pack('<i', 5000)])
        body = whole[20:]
        first = mdfsd.HDR.pack(mdfsd.FS_REQ_SEND_AGAIN, 20 + 3000, 14, 1) + whole[16:20] + body[:3000]
        last = mdfsd.HDR.pack(0, 20 + len(body) - 3000, 14, 1) + whole[16:20] + body[3000:]
        self.assertIsNone(self.svc.reassemble(first))
        hdr, op, payload = self.svc.reassemble(last)
        self.assertEqual(op, 0x1004)
        self.assertEqual(self.svc.parse_args(payload)[1], b'\x22' * 5000)

    def test_findfirst_findnext(self):
        d = self.root + '/mnt/vendor/nvdata/md/NVRAM/NVD_IMEI/'
        open(d + 'MP0B_002', 'wb').close()
        pat = wide('Z:\\NVRAM\\NVD_IMEI\\MP0B*')
        args = reply_args(self.call(0x1012, [pat, b'\0', b'\x10', struct.pack('<I', 32)]))
        h = struct.unpack('<i', args[0])[0]
        self.assertGreater(h, 0)
        self.assertEqual(len(args[1]), 52)
        self.assertEqual(mdfsd.wide_to_str(args[2]), 'MP0B_001')
        args = reply_args(self.call(0x1013, [struct.pack('<i', h), struct.pack('<I', 32)]))
        self.assertEqual(mdfsd.wide_to_str(args[2]), 'MP0B_002')
        args = reply_args(self.call(0x1013, [struct.pack('<i', h), struct.pack('<I', 32)]))
        self.assertEqual(struct.unpack('<i', args[0])[0], mdfsd.FS_NO_MORE_FILES)

    def test_cmpt_read(self):
        para = struct.pack('<10I', 1 | 2 | 4 | 8 | 0x10, 0, 0, mdfsd.FS_READ_ONLY, 0, 16, 0, 0, 8, 0)
        args = reply_args(self.call(0x1022, [wide('Z:\\NVRAM\\NVD_IMEI\\MP0B_001'), para]))
        self.assertEqual(struct.unpack('<ii', args[0]), (0x1f, 0))
        self.assertEqual(struct.unpack('<I', args[1])[0], 5120)
        self.assertEqual(args[3], bytes(range(16, 24)))


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""File service for the MT6771 modem (mtk_md): answers the modem's file requests on /dev/mtk_md_fs.

The modem keeps its NVRAM (calibration, IMEI, network settings) on the AP and asks for it through
CCCI channel 14. MediaTek's Android daemon ccci_fsd answers there; this is the same protocol,
written from MediaTek's published ccci_fsd.c (Apache-2.0) and checked against the modem's side
(ccci_fs_apis.c). Drives map to directories by prefix, as ccci_fsd does.

--root PREFIX serves PREFIX/<real path> instead of the real directories: bring-up runs against a
copy (--init-root makes one) so a mistake cannot touch the phone's calibration.
"""

import argparse
import errno
import fnmatch
import os
import struct
import sys
import time

# ccci_fsd.h
FS_API_RESP_ID = 0xFFFF0000
FS_REQ_SEND_AGAIN = 0x80000000
MAX_FS_PKT_BYTE = 3584 - 128
FS_MAX_BUF_SIZE = 16 + 4 + 4 + 4 * 12 + 0x4000 + 128
FS_MAX_ARG_NUM = 6
FS_REQ_BUFFER_NUM = 5
FS_FILE_MAX = 129
HDR = struct.Struct('<IIII')

FS_NO_ERROR = 0
FS_ERROR_RESERVED = -1
FS_PARAM_ERROR = -2
FS_DRIVE_NOT_FOUND = -4
FS_TOO_MANY_FILES = -5
FS_NO_MORE_FILES = -6
FS_FILE_NOT_FOUND = -9
FS_INVALID_FILE_HANDLE = -10
FS_ACCESS_DENIED = -16
FS_GENERAL_FAILURE = -18
FS_PATH_NOT_FOUND = -19
FS_PATH_OVER_LEN_ERROR = -49

FS_ATTR_READ_ONLY = 0x01
FS_ATTR_DIR = 0x10
FS_READ_ONLY = 0x100
FS_CREATE = 0x10000
FS_CREATE_ALWAYS = 0x20000
FS_COMMITTED = 0x01000000
FS_NONBLOCK_MODE = 0x10000000
FS_FILE_TYPE = 0x04
FS_DIR_TYPE = 0x08
FS_RECURSIVE_TYPE = 0x10
FS_COUNT_IN_CLUSTER = 0x02
FS_NO_ALT_DRIVE = 0x01
FS_ONLY_ALT_SERIAL = 0x02
FS_DRIVE_I_SYSTEM = 0x04
FS_DRIVE_V_NORMAL = 0x08
FS_DRIVE_V_REMOVABLE = 0x10
FS_NOT_MATCH, FS_LFN_MATCH = 0, 1
FS_CMPT_OPEN, FS_CMPT_GETFILESIZE, FS_CMPT_SEEK, FS_CMPT_READ, FS_CMPT_CLOSE, FS_CMPT_WRITE = (
    1, 2, 4, 8, 16, 32)

OPS = {
    0x1001: 'OPEN', 0x1002: 'SEEK', 0x1003: 'READ', 0x1004: 'WRITE', 0x1005: 'CLOSE',
    0x1006: 'CLOSEALL', 0x1007: 'CREATEDIR', 0x1008: 'REMOVEDIR', 0x1009: 'GETFILESIZE',
    0x100A: 'GETFOLDERSIZE', 0x100B: 'RENAME', 0x100C: 'MOVE', 0x100D: 'COUNT',
    0x100E: 'GETDISKINFO', 0x100F: 'DELETE', 0x1010: 'GETATTRIBUTES', 0x1011: 'OPENHINT',
    0x1012: 'FINDFIRST', 0x1013: 'FINDNEXT', 0x1014: 'FINDCLOSE', 0x1015: 'LOCKFAT',
    0x1016: 'UNLOCKALL', 0x1017: 'SHUTDOWN', 0x1018: 'XDELETE', 0x1019: 'CLEARDISKFLAG',
    0x101A: 'GETDRIVE', 0x101B: 'GETCLUSTERSIZE', 0x101C: 'SETDISKFLAG', 0x101D: 'OTP_WRITE',
    0x101E: 'OTP_READ', 0x101F: 'OTP_QUERYLEN', 0x1020: 'OTP_LOCK', 0x1021: 'RESTORE',
    0x1022: 'CMPT_READ', 0x1023: 'BIN_REGION_ACCESS', 0x1024: 'CMPT_WRITE',
    0x1025: 'GETFILEDETAIL',
}

# The phone's own ccci_fsd (MD1): drive letter, directory. W: is the DSP image, a single file.
DRIVES = [
    ('Z', '/mnt/vendor/nvdata/md'),
    ('X', '/mnt/vendor/protect_f/md'),
    ('Y', '/mnt/vendor/protect_s/md'),
    ('W', '/vendor/firmware'),
    ('V', '/mnt/vendor/nvdata/md_cmn'),
    ('U', '/data/vendor/mdlpm'),
    ('T', '/vendor/etc/mdota'),
]

DOS_ENTRY = struct.Struct('<8s3sBBBiHHiHI5I')		# FS_DOSDirEntry, 52 bytes
DISK_INFO = struct.Struct('<24sBB2s14I')		# FS_DiskInfo, 84 bytes
CMPT_R = struct.Struct('<10I')				# nvram_fs_para_cmpt_t, 32-bit pointer
CMPT_W = struct.Struct('<8IiiI')			# nvram_fs_para_cmptw_t


def errconv(e):
    return {errno.EACCES: FS_ACCESS_DENIED, errno.ENOENT: FS_FILE_NOT_FOUND,
            errno.EMFILE: FS_TOO_MANY_FILES}.get(e, FS_GENERAL_FAILURE)


def i32(b, off=0):
    return struct.unpack_from('<i', b, off)[0]


def u32(b, off=0):
    return struct.unpack_from('<I', b, off)[0]


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def p_i32(v):
    return struct.pack('<i', v)


def p_u32(v):
    return struct.pack('<I', v & 0xffffffff)


def wide_to_str(b):
    """FS_ConvWcsToCs: every other byte up to a NUL, '\\' to '/'; '..' refused."""
    out = []
    for k in range(0, len(b) - 1, 2):
        c = b[k]
        if c == 0:
            break
        c = '/' if c == 0x5c else chr(c)
        if c == '.' and out and out[-1] == '.':
            return None
        out.append(c)
    return ''.join(out)


def str_to_wide(s, max_len):
    """FS_ConvCsToWcs: at most max_len characters, the last one dropped when it fills max_len."""
    s = s.replace('/', '\\')[:max_len]
    if max_len and len(s) == max_len:
        s = s[:-1]
    return len(s), (s.encode('latin-1', 'replace') + b'\0').decode('latin-1').encode('utf-16-le')


def dos_datetime(t):
    tm = time.localtime(t)
    # ccci_fsd puts the day where the month belongs; the modem only compares these
    return ((tm.tm_sec & 0x1f) | (tm.tm_min & 0x3f) << 5 | (tm.tm_hour & 0x1f) << 11 |
            (tm.tm_mday & 0x1f) << 16 | (tm.tm_mday & 0x0f) << 21 |
            ((tm.tm_year - 1980) & 0x7f) << 25)


class Handle:
    def __init__(self, path, fd=None, flag=0, search=None):
        self.path = path
        self.fd = fd
        self.flag = flag
        self.search = search		# (dir entries iterator, pattern, attr, mask) for FINDFIRST


class Service:
    def __init__(self, root, postfix, log):
        self.root = root.rstrip('/')
        self.postfix = postfix
        self.log = log
        self.handles = {}
        self.slots = [None] * FS_REQ_BUFFER_NUM

    # ---- paths --------------------------------------------------------------------------------

    def real(self, path):
        return self.root + path if self.root else path

    def map_name(self, wide):
        """Modem path to Linux path, or an FS_* error."""
        name = wide_to_str(wide)
        if name is None:
            return FS_ACCESS_DENIED, None
        for letter, base in DRIVES:
            if name[:2].upper() == letter + ':':
                if letter == 'W':
                    p = '%s/dsp_%s.bin' % (base, self.postfix)
                    if not os.path.exists(self.real(p)):
                        p = '/custom/etc/firmware/dsp_%s.bin' % self.postfix
                    return 0, self.real(p)
                p = base + name[2:]
                if len(p) + 1 > 4096:
                    return FS_PATH_OVER_LEN_ERROR, None
                return 0, self.real(p)
        return FS_PATH_NOT_FOUND, None

    def drive_dir(self, letter):
        for l, base in DRIVES:
            if l == letter:
                return self.real(base)
        return None

    def attr(self, path):
        try:
            st = os.stat(path)
        except OSError as e:
            return errconv(e.errno)
        a = 0
        if (st.st_mode & 0o444) and not (st.st_mode & 0o222) and not (st.st_mode & 0o111):
            a |= FS_ATTR_READ_ONLY
        if os.path.isdir(path):
            a |= FS_ATTR_DIR
        return a

    def dos_entry(self, path):
        try:
            st = os.stat(path)
        except OSError:
            st = None
        a = self.attr(path)
        a = a if a >= 0 else 0
        if st is None:
            return DOS_ENTRY.pack(b'', b'', a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        return DOS_ENTRY.pack(b'\0' * 8, b'\0' * 3, a, 0, 0, s32(dos_datetime(st.st_atime)), 0, 0,
                              s32(dos_datetime(st.st_mtime)), 0, st.st_size & 0xffffffff,
                              0, 0, 0, 0, 0)

    # ---- handles ------------------------------------------------------------------------------

    def new_handle(self, h):
        for i in range(1, FS_FILE_MAX):
            if i not in self.handles:
                self.handles[i] = h
                return i
        return FS_TOO_MANY_FILES

    def get(self, idx, search=False):
        h = self.handles.get(idx)
        if h is None or (h.search is not None) != search:
            return None
        return h

    # ---- operations ---------------------------------------------------------------------------

    def op_open(self, wide, flag):
        err, path = self.map_name(wide)
        if err:
            return err
        lf = os.O_RDONLY if flag & FS_READ_ONLY else os.O_RDWR
        if flag & FS_CREATE:
            lf |= os.O_CREAT | os.O_RDWR
        if flag & FS_CREATE_ALWAYS:
            lf |= os.O_CREAT | os.O_RDWR | os.O_TRUNC
        if flag & FS_NONBLOCK_MODE:
            lf |= os.O_NONBLOCK
        if len(self.handles) >= FS_FILE_MAX - 1:
            return FS_TOO_MANY_FILES
        try:
            fd = os.open(path, lf, 0o660)
        except OSError as e:
            return errconv(e.errno)
        return self.new_handle(Handle(path, fd, flag))

    def op_seek(self, idx, off, whence):
        h = self.get(idx)
        if h is None:
            return FS_INVALID_FILE_HANDLE
        if whence not in (0, 1, 2):
            return FS_PARAM_ERROR
        try:
            return os.lseek(h.fd, off, whence)
        except OSError as e:
            return errconv(e.errno)

    def op_read(self, idx, n):
        h = self.get(idx)
        if h is None:
            return FS_INVALID_FILE_HANDLE, b''
        try:
            return 0, os.read(h.fd, max(0, min(n, 0x4000)))
        except OSError as e:
            return errconv(e.errno), b''

    def op_write(self, idx, data):
        h = self.get(idx)
        if h is None:
            return FS_INVALID_FILE_HANDLE, 0
        try:
            return 0, os.write(h.fd, data)
        except OSError as e:
            return errconv(e.errno), 0

    def op_close(self, idx):
        h = self.get(idx)
        if h is None:
            return FS_INVALID_FILE_HANDLE
        ret = 0
        try:
            if h.flag & (FS_CREATE | FS_CREATE_ALWAYS | FS_COMMITTED):
                os.fsync(h.fd)
            os.close(h.fd)
        except OSError as e:
            ret = errconv(e.errno)
        del self.handles[idx]
        return ret

    def op_closeall(self):
        for idx in list(self.handles):
            h = self.handles.pop(idx)
            if h.fd is not None:
                try:
                    os.close(h.fd)
                except OSError:
                    pass
        return 0

    def op_filesize(self, idx):
        h = self.get(idx)
        if h is None:
            return FS_INVALID_FILE_HANDLE, 0
        try:
            return 0, os.fstat(h.fd).st_size
        except OSError as e:
            return errconv(e.errno), 0

    def op_path(self, wide, fn):
        err, path = self.map_name(wide)
        if err:
            return err
        try:
            fn(path)
            return 0
        except OSError as e:
            return errconv(e.errno)

    def op_rename(self, old, new):
        e1, p1 = self.map_name(old)
        if e1:
            return e1
        e2, p2 = self.map_name(new)
        if e2:
            return e2
        try:
            os.rename(p1, p2)
            return 0
        except OSError as e:
            return errconv(e.errno)

    def op_move(self, src, dst, flag):
        # FS_MOVE_COPY (1) copies, anything else renames; the modem uses neither at boot
        e1, p1 = self.map_name(src)
        if e1:
            return e1
        e2, p2 = self.map_name(dst)
        if e2:
            return e2
        try:
            if flag & 1:
                with open(p1, 'rb') as a, open(p2, 'wb') as b:
                    b.write(a.read())
            else:
                os.rename(p1, p2)
            return 0
        except OSError as e:
            return errconv(e.errno)

    def cluster_size(self, letter):
        d = self.drive_dir(letter)
        if d is None:
            return FS_DRIVE_NOT_FOUND
        try:
            return os.statvfs(d).f_bsize
        except OSError:
            return FS_GENERAL_FAILURE

    def recursive_search(self, d, flag):
        """FS_RecursiveSearch: (entries found, clusters) under d."""
        cs = self.cluster_size('Z')
        found = clusters = 0
        try:
            names = os.listdir(d)
        except OSError:
            return 0, 0
        for n in names:
            p = os.path.join(d, n)
            try:
                st = os.stat(p)
            except OSError:
                continue
            if os.path.isdir(p):
                if flag & FS_DIR_TYPE:
                    found += 1
                if flag & FS_RECURSIVE_TYPE:
                    f, c = self.recursive_search(p, flag)
                    found += f
                    clusters += c
            elif os.path.isfile(p) and flag & FS_FILE_TYPE:
                if st.st_size > 0 and cs > 0:
                    clusters += st.st_size // cs + 1
                found += 1
        return found, clusters

    def op_foldersize(self, wide, flag):
        err, path = self.map_name(wide)
        if err:
            return err
        if not os.path.isdir(path):
            return FS_PARAM_ERROR if os.path.exists(path) else FS_FILE_NOT_FOUND
        letter = wide_to_str(wide)[0].upper()
        _, clusters = self.recursive_search(path, FS_FILE_TYPE | FS_DIR_TYPE | FS_RECURSIVE_TYPE)
        return clusters if flag == FS_COUNT_IN_CLUSTER else clusters * self.cluster_size(letter)

    def op_count(self, wide, flag):
        err, path = self.map_name(wide)
        if err:
            return err
        if not os.path.isdir(path):
            return FS_PARAM_ERROR if os.path.exists(path) else FS_FILE_NOT_FOUND
        return self.recursive_search(path, flag)[0]

    def recursive_delete(self, d, flag):
        n = 0
        for name in os.listdir(d):
            p = os.path.join(d, name)
            if os.path.isdir(p):
                if flag & FS_RECURSIVE_TYPE:
                    n += self.recursive_delete(p, flag)
                if flag & FS_DIR_TYPE:
                    os.rmdir(p)
                    n += 1
            elif os.path.isfile(p) and flag & FS_FILE_TYPE:
                os.unlink(p)
                n += 1
        return n

    def op_xdelete(self, wide, flag):
        err, path = self.map_name(wide)
        if err:
            return err
        if not flag & (FS_FILE_TYPE | FS_DIR_TYPE | FS_RECURSIVE_TYPE) or \
                flag & ~(FS_FILE_TYPE | FS_DIR_TYPE | FS_RECURSIVE_TYPE):
            return FS_PARAM_ERROR
        if not os.path.isdir(path):
            return FS_PARAM_ERROR if os.path.exists(path) else FS_FILE_NOT_FOUND
        try:
            n = self.recursive_delete(path, flag)
            if flag & FS_DIR_TYPE and flag & FS_RECURSIVE_TYPE:
                os.rmdir(path)
                n += 1
            return n
        except OSError as e:
            return errconv(e.errno)

    def find_in(self, it, pattern, dirname, attr, mask, max_len):
        for name in it:
            if name in ('.', '..') or not fnmatch.fnmatchcase(name, pattern):
                continue
            p = os.path.join(dirname, name)
            a = self.attr(p)
            a = a if a >= 0 else 0
            if a & attr == attr and a & mask == 0:
                n, wide = str_to_wide(name, max_len)
                entry = bytearray(self.dos_entry(p))
                entry[12] = FS_LFN_MATCH if len(name) < max_len else FS_NOT_MATCH
                return p, bytes(entry), n, wide
        return None

    def op_findfirst(self, wide, attr, mask, max_len):
        err, path = self.map_name(wide)
        if err:
            return err, None, 0, b''
        dirname, pattern = os.path.split(path)
        if not dirname:
            return FS_PATH_NOT_FOUND, None, 0, b''
        try:
            it = iter(sorted(os.listdir(dirname)))
        except OSError as e:
            return errconv(e.errno), None, 0, b''
        hit = self.find_in(it, pattern, dirname, attr, mask, max_len)
        if hit is None:
            return FS_NO_MORE_FILES, None, 0, b''
        p, entry, n, name = hit
        idx = self.new_handle(Handle(p, search=(it, pattern, attr, mask)))
        return idx, entry, n, name

    def op_findnext(self, idx, max_len):
        h = self.get(idx, search=True)
        if h is None:
            return FS_INVALID_FILE_HANDLE, None, 0, b''
        it, pattern, attr, mask = h.search
        hit = self.find_in(it, pattern, os.path.dirname(h.path), attr, mask, max_len)
        if hit is None:
            return FS_NO_MORE_FILES, None, 0, b''
        h.path, entry, n, name = hit
        return 0, entry, n, name

    def op_findclose(self, idx):
        if self.get(idx, search=True) is None:
            return FS_INVALID_FILE_HANDLE
        del self.handles[idx]
        return 0

    def op_getdrive(self, typ, serial, altmask):
        allowed = FS_DRIVE_I_SYSTEM | FS_DRIVE_V_NORMAL | FS_DRIVE_V_REMOVABLE
        if not typ & allowed or typ & ~allowed or serial < 1 or \
                (typ != FS_DRIVE_V_REMOVABLE and serial > 2) or \
                (typ == FS_DRIVE_V_REMOVABLE and serial > 1) or \
                (altmask != FS_NO_ALT_DRIVE and altmask & FS_NO_ALT_DRIVE) or \
                (altmask != FS_ONLY_ALT_SERIAL and altmask & FS_ONLY_ALT_SERIAL):
            return FS_PARAM_ERROR
        return ord('Z') if typ == FS_DRIVE_I_SYSTEM else FS_PARAM_ERROR

    def op_diskinfo(self):
        try:
            st = os.statvfs(self.drive_dir('Z'))
        except OSError:
            return -1, DISK_INFO.pack(b'', 0, 0, b'', *([0] * 14))
        vals = [0] * 14
        vals[5] = 512				# BytesPerSector
        vals[6] = st.f_bsize // 512		# SectorsPerCluster
        vals[7] = st.f_blocks & 0xffffffff	# TotalClusters
        vals[9] = 512				# FreeClusters: ccci_fsd's constant
        return 0, DISK_INFO.pack(b'', 0, 0, b'', *vals)

    def op_cmpt_read(self, wide, para):
        opid, _, _, flag, _, off, whence, _, length, _ = CMPT_R.unpack(para[:CMPT_R.size])
        bitmap = 0
        size = 0
        data = b''
        if not opid & FS_CMPT_OPEN:
            return (bitmap, FS_GENERAL_FAILURE), 0, b''
        idx = self.op_open(wide, flag)
        if idx < 0:
            return (bitmap, idx), 0, b''
        bitmap |= FS_CMPT_OPEN
        if opid & FS_CMPT_GETFILESIZE:
            r, size = self.op_filesize(idx)
            if r < 0:
                self.op_close(idx)
                return (bitmap, r), 0, b''
            bitmap |= FS_CMPT_GETFILESIZE
        if opid & FS_CMPT_SEEK:
            r = self.op_seek(idx, s32(off), whence)
            if r < 0:
                self.op_close(idx)
                return (bitmap, r), size, b''
            bitmap |= FS_CMPT_SEEK
        if opid & FS_CMPT_READ:
            r, data = self.op_read(idx, length)
            if r < 0:
                self.op_close(idx)
                return (bitmap, r), size, b''
            bitmap |= FS_CMPT_READ
        if opid & FS_CMPT_CLOSE:
            r = self.op_close(idx)
            if r < 0:
                return (bitmap, r), size, data
            bitmap |= FS_CMPT_CLOSE
        else:
            self.op_close(idx)
            return (bitmap, 0), size, data
        return (bitmap, 0), size, data

    def op_cmpt_write(self, wide, para, data):
        vals = CMPT_W.unpack(para[:CMPT_W.size])
        opid, _, _, flag, off, whence, _, length, fh = vals[:9]
        bitmap = 0
        if fh < 0:
            if not opid & FS_CMPT_OPEN:
                return (bitmap, FS_PARAM_ERROR), -1, 0
            idx = self.op_open(wide, flag)
            if idx < 0:
                return (bitmap, idx), -1, 0
            bitmap |= FS_CMPT_OPEN
        else:
            idx = fh
        written = 0
        if opid & FS_CMPT_SEEK:
            r = self.op_seek(idx, s32(off), whence)
            if r < 0:
                self.op_close(idx)
                return (bitmap, r), idx, 0
            bitmap |= FS_CMPT_SEEK
        if opid & FS_CMPT_WRITE:
            r, written = self.op_write(idx, data[:length])
            if r < 0:
                self.op_close(idx)
                return (bitmap, r), idx, 0
            bitmap |= FS_CMPT_WRITE
        if opid & FS_CMPT_CLOSE:
            r = self.op_close(idx)
            if r < 0:
                return (bitmap, r), idx, written
            bitmap |= FS_CMPT_CLOSE
        return (bitmap, 0), idx, written

    # ---- dispatch -----------------------------------------------------------------------------

    def handle(self, op, args):
        """One request: the reply's arguments (a list of bytes), and a line for the log."""
        name = OPS.get(op, '%#x' % op)

        def path_of(k=0):
            return wide_to_str(args[k]) if len(args) > k else ''

        if op == 0x1001 or op == 0x1011:
            r = self.op_open(args[0], i32(args[1]))
            out = [p_i32(r)] + ([args[2][:8].ljust(8, b'\0')] if op == 0x1011 else [])
            return out, '%s %s flag %#x -> %d' % (name, path_of(), u32(args[1]), r)
        if op == 0x1002:
            r = self.op_seek(i32(args[0]), i32(args[1]), u32(args[2]))
            return [p_i32(r)], '%s h%d %d/%d -> %d' % (name, i32(args[0]), i32(args[1]), u32(args[2]), r)
        if op == 0x1003:
            r, data = self.op_read(i32(args[0]), i32(args[1]))
            return [p_i32(r), p_u32(len(data)), data], '%s h%d %d -> %d, %d bytes' % (
                name, i32(args[0]), i32(args[1]), r, len(data))
        if op == 0x1004:
            n = i32(args[2])
            r, w = self.op_write(i32(args[0]), args[1][:n])
            return [p_i32(r), p_u32(w)], '%s h%d %d -> %d, %d written' % (name, i32(args[0]), n, r, w)
        if op == 0x1005:
            r = self.op_close(i32(args[0]))
            return [p_i32(r)], '%s h%d -> %d' % (name, i32(args[0]), r)
        if op == 0x1006:
            return [p_i32(self.op_closeall())], name
        if op == 0x1007:
            r = self.op_path(args[0], lambda p: os.mkdir(p, 0o770))
            return [p_i32(r)], '%s %s -> %d' % (name, path_of(), r)
        if op == 0x1008:
            r = self.op_path(args[0], os.rmdir)
            return [p_i32(r)], '%s %s -> %d' % (name, path_of(), r)
        if op == 0x1009:
            r, size = self.op_filesize(i32(args[0]))
            return [p_i32(r), p_u32(size)], '%s h%d -> %d, %d' % (name, i32(args[0]), r, size)
        if op == 0x100A:
            r = self.op_foldersize(args[0], u32(args[1]))
            return [p_i32(r)], '%s %s -> %d' % (name, path_of(), r)
        if op == 0x100B:
            r = self.op_rename(args[0], args[1])
            return [p_i32(r)], '%s %s %s -> %d' % (name, path_of(0), path_of(1), r)
        if op == 0x100C:
            r = self.op_move(args[0], args[1], u32(args[2]))
            return [p_i32(r)], '%s %s %s -> %d' % (name, path_of(0), path_of(1), r)
        if op == 0x100D:
            r = self.op_count(args[0], u32(args[1]))
            return [p_i32(r)], '%s %s flag %#x -> %d' % (name, path_of(), u32(args[1]), r)
        if op == 0x100E:
            r, info = self.op_diskinfo()
            return [p_i32(r), info], '%s %s -> %d' % (name, path_of(), r)
        if op == 0x100F:
            r = self.op_path(args[0], os.unlink)
            return [p_i32(r)], '%s %s -> %d' % (name, path_of(), r)
        if op == 0x1010:
            err, path = self.map_name(args[0])
            r = err if err else self.attr(path)
            return [p_i32(r)], '%s %s -> %d' % (name, path_of(), r)
        if op == 0x1012:
            r, entry, n, wide = self.op_findfirst(args[0], args[1][0], args[2][0], u32(args[3]))
            out = [p_i32(r), entry or bytes(DOS_ENTRY.size), wide if n else b'']
            return out, '%s %s -> %d%s' % (name, path_of(), r, ' ' + wide_to_str(wide) if n else '')
        if op == 0x1013:
            r, entry, n, wide = self.op_findnext(i32(args[0]), u32(args[1]))
            out = [p_i32(r), entry or bytes(DOS_ENTRY.size), wide if n else b'']
            return out, '%s h%d -> %d%s' % (name, i32(args[0]), r, ' ' + wide_to_str(wide) if n else '')
        if op == 0x1014:
            r = self.op_findclose(i32(args[0]))
            return [p_i32(r)], '%s h%d -> %d' % (name, i32(args[0]), r)
        if op in (0x1015, 0x1019, 0x101C):
            return [p_i32(0)], name
        if op == 0x1016:
            return [p_i32(1)], name
        if op == 0x1017:
            self.op_closeall()
            return [], name
        if op == 0x1018:
            r = self.op_xdelete(args[0], u32(args[1]))
            return [p_i32(r)], '%s %s flag %#x -> %d' % (name, path_of(), u32(args[1]), r)
        if op == 0x101A:
            r = self.op_getdrive(u32(args[0]), u32(args[1]), u32(args[2]))
            return [p_i32(r)], '%s type %#x -> %d' % (name, u32(args[0]), r)
        if op == 0x101B:
            r = self.cluster_size(chr(u32(args[0]) & 0xff))
            return [p_i32(r)], '%s %c -> %d' % (name, chr(u32(args[0]) & 0xff), r)
        if op == 0x101E:
            n = u32(args[2])
            return [p_i32(FS_GENERAL_FAILURE), bytes(n)], '%s: no OTP on this service' % name
        if op == 0x101F:
            return [p_i32(FS_GENERAL_FAILURE), p_u32(0)], '%s: no OTP' % name
        if op in (0x101D, 0x1020):
            return [p_i32(FS_GENERAL_FAILURE)], '%s: no OTP' % name
        if op == 0x1021:
            # libnvram's bin region restore is not implemented: the modem keeps its own backups
            return [p_i32(FS_GENERAL_FAILURE)], '%s %s -> not supported' % (name, path_of())
        if op == 0x1022:
            ret, size, data = self.op_cmpt_read(args[0], args[1])
            out = [struct.pack('<ii', *ret), p_u32(size), p_u32(len(data)), data]
            return out, '%s %s map %#x -> %s, size %d, %d read' % (
                name, path_of(), u32(args[1]), ret, size, len(data))
        if op == 0x1023:
            return [p_i32(FS_PARAM_ERROR)], name
        if op == 0x1024:
            ret, fh, w = self.op_cmpt_write(args[0], args[1], args[2])
            out = [struct.pack('<ii', *ret), p_i32(fh), p_u32(w)]
            return out, '%s %s map %#x -> %s, h%d, %d written' % (
                name, path_of(), u32(args[1]), ret, fh, w)
        if op == 0x1025:
            err, path = self.map_name(args[0])
            detail = bytes(24)
            if not err:
                try:
                    st = os.stat(path)
                    detail = struct.pack('<3Q', int(st.st_mtime), int(st.st_atime), int(st.st_ctime))
                except OSError as e:
                    err = errconv(e.errno)
            return [p_i32(err), detail], '%s %s -> %d' % (name, path_of(), err)
        return [p_i32(FS_PARAM_ERROR)], 'unknown operation %#x' % op

    # ---- framing ------------------------------------------------------------------------------

    @staticmethod
    def parse_args(payload):
        """FS_GetPackInfo: argument count, then {u32 length, data padded to 4}; none may be empty."""
        if len(payload) < 4:
            return None
        n = u32(payload)
        if n > FS_MAX_ARG_NUM:
            return None
        pos, args = 4, []
        for _ in range(n):
            if pos + 4 > len(payload):
                return None
            ln = u32(payload, pos)
            pos += 4
            if ln == 0 or pos + ln > len(payload):
                return None
            args.append(payload[pos:pos + ln])
            pos += (ln + 3) & ~3
        if pos > FS_MAX_BUF_SIZE:
            return None
        return args

    def reassemble(self, msg):
        """One request once its last fragment is in: (header, op, payload), else None."""
        d0, length, ch, slot = HDR.unpack_from(msg)
        if (ch & 0xffff) != 14 or slot >= FS_REQ_BUFFER_NUM or length > len(msg):
            self.log('bad message: channel %#x slot %d length %d/%d' % (ch, slot, length, len(msg)))
            return None
        op = u32(msg, 16)
        body = msg[20:length]
        pending = self.slots[slot]
        if pending is None:
            pending = [msg[:16], op, bytearray()]
        pending[2] += body
        if d0 & FS_REQ_SEND_AGAIN:
            self.slots[slot] = pending
            return None
        self.slots[slot] = None
        hdr = bytearray(msg[:16])
        return hdr, pending[1], bytes(pending[2])

    @staticmethod
    def build(hdr, op, out):
        """FS_WriteToMD: the reply, split into messages of at most MAX_FS_PKT_BYTE of data."""
        body = bytearray(p_u32(len(out)))
        for a in out:
            body += p_u32(len(a)) + a + b'\0' * (-len(a) & 3)
        d0, _, ch, slot = HDR.unpack_from(hdr)
        respop = p_u32(FS_API_RESP_ID | op)
        msgs = []
        for pos in range(0, max(len(body), 1), MAX_FS_PKT_BYTE):
            chunk = bytes(body[pos:pos + MAX_FS_PKT_BYTE])
            last = pos + MAX_FS_PKT_BYTE >= len(body)
            h0 = (d0 & ~FS_REQ_SEND_AGAIN) if last else (d0 | FS_REQ_SEND_AGAIN)
            msgs.append(HDR.pack(h0, 16 + 4 + len(chunk), ch + 1, slot) + respop + chunk)
        return msgs


def init_root(root):
    import shutil
    for _, base in DRIVES:
        src, dst = base, root.rstrip('/') + base
        if base == '/vendor/firmware' or not os.path.isdir(src):
            continue
        if os.path.exists(dst):
            continue
        shutil.copytree(src, dst, symlinks=True)
        print('copied %s to %s' % (src, dst))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('--dev', default='/dev/mtk_md_fs')
    ap.add_argument('--root', default='', help='serve ROOT/<path> instead of the real directories')
    ap.add_argument('--init-root', action='store_true', help='copy the real directories into ROOT first')
    ap.add_argument('--postfix', default='1_ulwctg_n', help="the modem image's postfix, for W:")
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args()

    def log(s):
        if not a.quiet:
            print('%.3f %s' % (time.monotonic(), s), flush=True)

    if a.init_root:
        if not a.root:
            sys.exit('--init-root needs --root')
        init_root(a.root)
    svc = Service(a.root, a.postfix, log)
    fd = os.open(a.dev, os.O_RDWR)
    log('serving %s from %s' % (a.dev, a.root or 'the real directories'))
    while True:
        msg = os.read(fd, 8192)
        req = svc.reassemble(msg)
        if req is None:
            continue
        hdr, op, payload = req
        args = svc.parse_args(payload)
        if args is None:
            out, line = [p_i32(FS_PARAM_ERROR)], 'malformed %s' % OPS.get(op, hex(op))
        else:
            try:
                out, line = svc.handle(op, args)
            except (IndexError, struct.error) as e:
                out, line = [p_i32(FS_PARAM_ERROR)], '%s: bad arguments (%s)' % (OPS.get(op, hex(op)), e)
        log(line)
        for m in svc.build(hdr, op, out):
            os.write(fd, m)


if __name__ == '__main__':
    main()

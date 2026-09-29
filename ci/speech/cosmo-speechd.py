#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Voice call control for the MT6771 modem: the Gen93 speech messages on /dev/ccci_aud.

The vendor audio HAL (SpeechDriverNormal, SpeechMessengerNormal) starts the modem's speech path
with SPH_ON and stops it with SPH_OFF; the voice samples themselves run over the AFE PCM2
interface, kept up by the card's hostless Voice_MD1 PCM. This is the minimal sequence of
/storage/notes/projects/cosmo/hw-spec/speech-gen93.org section 3, without the parameter blob
(sph_param_valid = 0). The raw audio share memory (/dev/ccci_raw_audio) is formatted once per
boot after the modem is ready, as the HAL does, and its ring indices are reset after SPH_OFF.

  cosmo-speechd.py daemon            serve /run/cosmo-speechd.sock, answer the modem at any time
  cosmo-speechd.py start [RATE]      PCMs up, SPH_ON, UL unmute (RATE 32000, 16000 or 8000)
  cosmo-speechd.py stop              UL mute, PCMs down, SPH_OFF
  cosmo-speechd.py mute on|off       UL mute
  cosmo-speechd.py mailbox ID [P16 [P32]]   send any mailbox (hex ids), for debugging
  cosmo-speechd.py status
"""

import argparse
import errno
import fcntl
import mmap
import os
import select
import signal
import socket
import struct
import sys
import threading
import time

CCCI_MAGIC = 0xFFFFFFFF
PCM_TX_CH = 5
HDR = struct.Struct('<IIII')
MAILBOX = struct.Struct('<IHHII')		# magic, param16, msg_id, channel, param32
AP_PAYLOAD = struct.Struct('<IIIHHHHH')		# d0, d1, channel, len, msg_id, sync, type, size
MD_PAYLOAD = struct.Struct('<IIIHHHHHHH')	# ... sync, type, size, idx, total_idx
AP_SYNC = 0xA2A2
MD_SYNC = 0x1234
CCCI_MTU = 3456
READ_SIZE = 0xd80
MAX_AP_DATA = 0xD6A

SPH_ON, SPH_OFF, MUTE_UL, MUTE_DL = 0x2F20, 0x2F21, 0x2F02, 0x2F03
EPOF_ACK, NW_CODEC_INFO_ACK, MD_ALIVE_ACK = 0x2F78, 0x2F90, 0x2FA0
EPOF_NOTIFY, NW_CODEC_INFO_NOTIFY, MD_ALIVE = 0xAF78, 0xAF90, 0xAFA0
TYPE_NW_CODEC_INFO, TYPE_SPH_INFO = 20, 25

NAMES = {
    0x2F00: 'DL_DIGIT_VOLUME', 0x2F01: 'UL_DIGIT_VOLUME', 0x2F02: 'MUTE_UL', 0x2F03: 'MUTE_DL',
    0x2F05: 'DL_ENH_REF_VOLUME', 0x2F08: 'MUTE_UL_SOURCE', 0x2F09: 'MUTE_DL_CODEC',
    0x2F20: 'SPH_ON', 0x2F21: 'SPH_OFF', 0x2F23: 'CTRL_SPH_ENH', 0x2F2B: 'SPH_DEV_CHANGE',
    0x2F78: 'EPOF_ACK', 0x2F7B: 'DYNAMIC_PAR_SHM', 0x2F90: 'NW_CODEC_INFO_ACK',
    0x2FA0: 'MD_ALIVE_ACK', 0xAF70: 'EM_DATA_REQUEST', 0xAF75: 'NETWORK_STATUS',
    0xAF78: 'EPOF_NOTIFY', 0xAF90: 'NW_CODEC_INFO_NOTIFY', 0xAFA0: 'MD_ALIVE',
}

# getSyncType @0xd9430: AP messages the modem acks with id | 0x8000
NEED_ACK = {0x2F02, 0x2F03, 0x2F08, 0x2F20, 0x2F21, 0x2F2B, 0x2F30, 0x2F31, 0x2F3A, 0x2F3B, 0x2F3C,
            0x2F3D, 0x2F40, 0x2F41, 0x2F43, 0x2F44, 0x2F79, 0x2F7B, 0x2F80}
MD_REPLY = {MD_ALIVE: MD_ALIVE_ACK, EPOF_NOTIFY: EPOF_ACK, NW_CODEC_INFO_NOTIFY: NW_CODEC_INFO_ACK}

RATE_ENUM = {8000: 0, 16000: 1, 32000: 2}
SPH_INFO = struct.Struct('<6BHHH2BHBBHII12H16H44s')


def msg_name(msg_id):
    if msg_id in NAMES:
        return NAMES[msg_id]
    if msg_id & 0xFF00 == 0xAF00 and (msg_id & 0x7FFF) in NAMES:
        return NAMES[msg_id & 0x7FFF] + '_ACK'
    return '%04X' % msg_id


def pack_mailbox(msg_id, p16=0, p32=0):
    return MAILBOX.pack(CCCI_MAGIC, p16 & 0xFFFF, msg_id, PCM_TX_CH, p32 & 0xFFFFFFFF)


def pack_payload(msg_id, data_type, data):
    """speechMessageToCcciMessage: the kernel rewrites data[1] with the write length."""
    if len(data) > MAX_AP_DATA:
        raise ValueError('payload of %d bytes' % len(data))
    n = len(data) + 6
    return AP_PAYLOAD.pack(0, n, PCM_TX_CH, n, msg_id, AP_SYNC, data_type, len(data)) + data


def sph_info(rate, ext_dev=0, application=0):
    """configSpeechInfo @0x920b8 with no parameter blob: path 1 (shm via CCCI), valid 0."""
    return SPH_INFO.pack(application, 0, RATE_ENUM[rate], 0, 1, 0, 0, 0, ext_dev, 0, 0, 0, 0, 0,
                         0, 0, 0, *([0] * 28), b'')


class Frame:
    def __init__(self, raw, kind, msg_id, p16=0, p32=0, data_type=None, data=b'', idx=0, total=0):
        self.raw, self.kind, self.msg_id = raw, kind, msg_id
        self.p16, self.p32 = p16, p32
        self.data_type, self.data, self.idx, self.total = data_type, data, idx, total

    def __repr__(self):
        if self.kind == 'mailbox':
            return '%s mailbox p16 %#x p32 %#x' % (msg_name(self.msg_id), self.p16, self.p32)
        return '%s payload type %d, %d bytes, part %d/%d' % (
            msg_name(self.msg_id), self.data_type, len(self.data), self.idx, self.total)


def parse(raw):
    """readSpeechMessage / ccciMessageToSpeechMessage: a mailbox by data[0], else an MD payload."""
    if len(raw) < HDR.size:
        raise ValueError('short frame of %d bytes' % len(raw))
    if struct.unpack_from('<I', raw)[0] == CCCI_MAGIC:
        _, p16, msg_id, _, p32 = MAILBOX.unpack_from(raw)
        return Frame(raw, 'mailbox', msg_id, p16, p32)
    if len(raw) < MD_PAYLOAD.size:
        raise ValueError('short payload of %d bytes' % len(raw))
    _, d1, _, _, msg_id, sync, dtype, size, idx, total = MD_PAYLOAD.unpack_from(raw)
    if sync != MD_SYNC:
        raise ValueError('payload sync %#x' % sync)
    if d1 != len(raw) or size + MD_PAYLOAD.size != len(raw):
        raise ValueError('payload length %d, data[1] %d, data_size %d' % (len(raw), d1, size))
    return Frame(raw, 'payload', msg_id, data_type=dtype, data=raw[MD_PAYLOAD.size:], idx=idx,
                 total=total)


def cstr(b):
    return b.split(b'\0', 1)[0].decode('latin-1')


# ---- raw audio share memory (sph_shm_t, speech-gen93.org 1.5) ------------------------------------

CCCI_IOC_SMEM_BASE, CCCI_IOC_SMEM_LEN = 0x80044330, 0x80044331
SHM_SIZE = 0xD000
SHM_GUARD = b'\x0a' * 32
SHM_TAIL = SHM_SIZE - len(SHM_GUARD)
SHM_AP_FLAG, SHM_MD_FLAG, SHM_REGIONS_AT, SHM_CHECKSUM = 0x20, 0x24, 0x28, 0x7C
SHM_REGIONS = ((0x80, 0x3000), (0x3080, 0x2000), (0x5080, 0x7F60))	# sph_param, ap_data, md_data
SHM_FORMATTED, SHM_BUSY = 1, 2		# ap_flag bits; md_flag bit1 is the modem reading
REGION = struct.Struct('<4I')		# offset, size, read_idx, write_idx


def shm_format(buf):
    """formatShareMemory @0xa0900, in its store order: ap_flag bit0 goes up last."""
    if len(buf) < SHM_SIZE:
        raise ValueError('share memory of %d bytes, sph_shm_t needs %d' % (len(buf), SHM_SIZE))
    buf[0:len(SHM_GUARD)] = SHM_GUARD
    struct.pack_into('<II', buf, SHM_AP_FLAG, 0, 0)
    for k, (off, size) in enumerate(SHM_REGIONS):
        REGION.pack_into(buf, SHM_REGIONS_AT + REGION.size * k, off, size, 0, 0)
    struct.pack_into('<9I', buf, 0x58, *([0] * 9))
    struct.pack_into('<I', buf, SHM_CHECKSUM, SHM_CHECKSUM)
    for off, size in SHM_REGIONS:
        buf[off:off + size] = bytes(size)
    buf[SHM_TAIL:SHM_SIZE] = SHM_GUARD
    ap = struct.unpack_from('<I', buf, SHM_AP_FLAG)[0]
    struct.pack_into('<I', buf, SHM_AP_FLAG, ap | SHM_FORMATTED)


def shm_header(buf):
    ap, md = struct.unpack_from('<II', buf, SHM_AP_FLAG)
    regions = [REGION.unpack_from(buf, SHM_REGIONS_AT + REGION.size * k) for k in range(3)]
    return ap, md, regions, struct.unpack_from('<I', buf, SHM_CHECKSUM)[0]


def shm_intact(buf):
    if len(buf) < SHM_SIZE:
        return False
    ap, _, regions, checksum = shm_header(buf)
    return (bytes(buf[:len(SHM_GUARD)]) == SHM_GUARD and bytes(buf[SHM_TAIL:SHM_SIZE]) == SHM_GUARD
            and ap & SHM_FORMATTED and checksum == SHM_CHECKSUM
            and [r[:2] for r in regions] == [tuple(r) for r in SHM_REGIONS])


def shm_reset_indices(buf):
    """resetShareMemoryIndex @0xa0cf0: all six indices to 0 unless the modem is reading."""
    ap = struct.unpack_from('<I', buf, SHM_AP_FLAG)[0]
    struct.pack_into('<I', buf, SHM_AP_FLAG, ap | SHM_BUSY)
    ok = not struct.unpack_from('<I', buf, SHM_MD_FLAG)[0] & SHM_BUSY
    if ok:
        for k in range(3):
            struct.pack_into('<II', buf, SHM_REGIONS_AT + REGION.size * k + 8, 0, 0)
    ap = struct.unpack_from('<I', buf, SHM_AP_FLAG)[0]
    struct.pack_into('<I', buf, SHM_AP_FLAG, ap & ~SHM_BUSY)
    return ok


class RawAudio:
    """ccci_smem_get(): open, SMEM_BASE, SMEM_LEN, mmap the length; the fd stays open. The
    vendor.audiohal.speech.shm_init property becomes a file in /run; a header that is not
    intact (the driver cleared the memory since) is formatted again."""

    def __init__(self, log, dev, marker, buf=None):
        self.log, self.dev, self.marker, self.buf = log, dev, marker, buf
        self.lock = threading.Lock()

    def open(self):
        fd = os.open(self.dev, os.O_RDWR | os.O_CLOEXEC)
        try:
            arg = bytearray(4)
            fcntl.ioctl(fd, CCCI_IOC_SMEM_BASE, arg, True)
            base = struct.unpack('<I', arg)[0]
            fcntl.ioctl(fd, CCCI_IOC_SMEM_LEN, arg, True)
            length = struct.unpack('<I', arg)[0]
            if length < SHM_SIZE:
                raise ValueError('%s: %d bytes, sph_shm_t needs %d' % (self.dev, length, SHM_SIZE))
            self.buf = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        except (OSError, ValueError):
            os.close(fd)
            raise
        self.fd = fd
        self.log('%s: base %#x, %d bytes' % (self.dev, base, length))

    def ensure(self):
        with self.lock:
            if self.buf is None:
                self.open()
            if not os.path.exists(self.marker) or not shm_intact(self.buf):
                shm_format(self.buf)
                with open(self.marker, 'w'):
                    pass
                self.log('raw audio shm formatted')
            ap, md, regions, checksum = shm_header(self.buf)
            self.log('raw audio shm ap_flag %#x md_flag %#x checksum %d, regions %s' % (
                ap, md, checksum, ' '.join('%#x+%#x r%d w%d' % r for r in regions)))

    def reset_indices(self):
        with self.lock:
            if self.buf is not None and not shm_reset_indices(self.buf):
                self.log('raw audio shm: modem still reading, indices kept')


def wait_modem_ready(fd, tries=3000):
    """checkModemReady every 100 ms, as formatShareMemoryThread: mtk_md offers POLLOUT once
    the modem is ready."""
    p = select.poll()
    p.register(fd, select.POLLOUT)
    for _ in range(tries):
        if p.poll(0):
            return True
        time.sleep(0.1)
    return False


# ---- hostless PCM, straight ioctls ----------------------------------------------------------------

UL = struct.calcsize('L')
ULONG_MAX = (1 << (8 * UL)) - 1
HW_PARAMS_SIZE = 4 + 8 * 32 + 21 * 12 + 6 * 4 + UL + 64
SW_PARAMS = struct.Struct('@iII7LII56s')


def _iowr(nr, size):
    return (3 << 30) | (size << 16) | (ord('A') << 8) | nr


def _io(nr):
    return (ord('A') << 8) | nr


PCM_IOCTL_HW_PARAMS = _iowr(0x11, HW_PARAMS_SIZE)
PCM_IOCTL_SW_PARAMS = _iowr(0x13, SW_PARAMS.size)
PCM_IOCTL_PREPARE = _io(0x40)
PCM_IOCTL_START = _io(0x42)
PCM_IOCTL_DROP = _io(0x43)
ACCESS_RW_INTERLEAVED, FORMAT_S16_LE = 3, 2
P_CHANNELS, P_RATE, P_PERIOD_SIZE, P_PERIODS = 10, 11, 13, 15


def hw_params(channels, rate, period, periods):
    """snd_pcm_hw_params: everything open except what is fixed; the kernel refines and chooses."""
    b = bytearray(HW_PARAMS_SIZE)
    masks = [1 << ACCESS_RW_INTERLEAVED, 1 << FORMAT_S16_LE, 1]
    for k, m in enumerate(masks):
        struct.pack_into('<I', b, 4 + 32 * k, m)
    fixed = {P_CHANNELS: channels, P_RATE: rate, P_PERIOD_SIZE: period, P_PERIODS: periods}
    for k in range(12):
        v = fixed.get(8 + k)
        lo, hi, flags = (v, v, 1 << 2) if v is not None else (0, 0xFFFFFFFF, 0)
        struct.pack_into('<III', b, 4 + 8 * 32 + 12 * k, lo, hi, flags)
    struct.pack_into('<I', b, 4 + 8 * 32 + 21 * 12, 0xFFFFFFFF)	# rmask
    return b


def sw_params(period):
    """Never auto-start or stop, and count the buffer as always full of silence: the hostless
    pointer never moves, so a playback START would otherwise fail for lack of data."""
    return bytearray(SW_PARAMS.pack(0, 1, 0, period, 1, ULONG_MAX, ULONG_MAX, 0, ULONG_MAX, 0,
                                    0, 0, b''))


def card_index(card):
    if card.isdigit():
        return int(card)
    return int(os.readlink('/proc/asound/' + card)[len('card'):])


class HostlessPcm:
    def __init__(self, log, card, device, channels, period, periods):
        self.log, self.card, self.device = log, card, device
        self.channels, self.period, self.periods = channels, period, periods
        self.fds = []

    def open(self, rate):
        idx = card_index(self.card)
        try:
            for d in 'cp':		# the HAL opens capture first
                path = '/dev/snd/pcmC%dD%d%s' % (idx, self.device, d)
                fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
                self.fds.append(fd)
                fcntl.ioctl(fd, PCM_IOCTL_HW_PARAMS,
                            hw_params(self.channels, rate, self.period, self.periods), True)
                fcntl.ioctl(fd, PCM_IOCTL_SW_PARAMS, sw_params(self.period), True)
                fcntl.ioctl(fd, PCM_IOCTL_PREPARE)
                self.log('pcm %s prepared at %d Hz, %d ch' % (path, rate, self.channels))
            for fd in self.fds:
                fcntl.ioctl(fd, PCM_IOCTL_START)
            self.log('pcm started')
        except OSError:
            self.close()
            raise

    def close(self):
        for fd in self.fds:
            try:
                fcntl.ioctl(fd, PCM_IOCTL_DROP)
            except OSError:
                pass
            os.close(fd)
        if self.fds:
            self.log('pcm closed')
        self.fds = []


# ---- modem side -----------------------------------------------------------------------------------

class Speech:
    def __init__(self, fd, log, pcm, ack_timeout, shm=None):
        self.fd, self.log, self.pcm, self.ack_timeout = fd, log, pcm, ack_timeout
        self.shm = shm
        self.cond = threading.Condition()
        self.acks = set()
        self.epof = False
        self.queue = threading.Lock()	# SpeechMessageQueue: one need-ack message at a time
        self.on = False
        self.rate = None
        self.muted = True

    def write(self, frame, what):
        if self.epof and not what.startswith('MD_ALIVE_ACK'):
            self.log('tx dropped after EPOF: %s' % what)
            return False
        self.log('tx %s: %s' % (what, frame.hex()))
        for attempt in range(20):	# sendSpeechMessage: 20 tries, 2 ms apart
            try:
                os.write(self.fd, frame)
                return True
            except OSError as e:
                if attempt == 19 or e.errno not in (errno.ENODEV, errno.EAGAIN, errno.EBUSY):
                    self.log('tx %s failed: %s' % (what, e))
                    return False
                time.sleep(0.002)
        return False

    def send(self, msg_id, frame, what):
        if msg_id not in NEED_ACK:
            return self.write(frame, what)
        with self.queue:
            with self.cond:
                self.acks.discard(msg_id)
            if not self.write(frame, what):
                return False
            end = time.monotonic() + self.ack_timeout
            with self.cond:
                while msg_id not in self.acks and not self.epof:
                    left = end - time.monotonic()
                    if left <= 0:
                        self.log('no ack for %s within %.1f s' % (msg_name(msg_id), self.ack_timeout))
                        return False
                    self.cond.wait(left)
                return msg_id in self.acks

    def mailbox(self, msg_id, p16=0, p32=0):
        what = '%s mailbox p16 %#x p32 %#x' % (msg_name(msg_id), p16, p32)
        return self.send(msg_id, pack_mailbox(msg_id, p16, p32), what)

    def handle(self, raw):
        try:
            f = parse(raw)
        except ValueError as e:
            self.log('rx bad frame (%s): %s' % (e, raw.hex()))
            return
        self.log('rx %r: %s' % (f, raw.hex()))
        mid = f.msg_id
        if mid == NW_CODEC_INFO_NOTIFY and f.kind == 'payload' and f.data_type == TYPE_NW_CODEC_INFO:
            self.log('network codec "%s", hd voice "%s"' % (cstr(f.data[:92]), cstr(f.data[92:184])))
        if mid == EPOF_NOTIFY:
            self.write(pack_mailbox(EPOF_ACK), msg_name(EPOF_ACK))
            with self.cond:
                self.epof = True
                self.cond.notify_all()
            return
        if mid == MD_ALIVE:
            with self.cond:
                self.epof = False
            self.write(pack_mailbox(MD_ALIVE_ACK), msg_name(MD_ALIVE_ACK))
            return
        if mid in MD_REPLY:
            self.write(pack_mailbox(MD_REPLY[mid]), msg_name(MD_REPLY[mid]))
            return
        if mid & 0xFF00 == 0xAF00 and (mid & 0x7FFF) in NEED_ACK:
            with self.cond:
                self.acks.add(mid & 0x7FFF)
                self.cond.notify_all()
            return
        if mid not in (0xAF70, 0xAF75):
            self.log('rx %s not handled' % msg_name(mid))

    def reader(self):
        while True:
            try:
                raw = os.read(self.fd, READ_SIZE)
            except OSError as e:
                self.log('read: %s' % e)
                time.sleep(1)
                continue
            self.handle(raw)

    # commands, run one at a time from the control socket

    def start(self, rate):
        if self.on:
            return 'error: speech already on at %d Hz' % self.rate
        if rate not in RATE_ENUM:
            return 'error: rate %d, want one of %s' % (rate, sorted(RATE_ENUM))
        self.format_shm()
        try:
            self.pcm.open(rate)
        except OSError as e:
            return 'error: pcm: %s' % e
        self.on, self.rate, self.muted = True, rate, True
        if not self.send(SPH_ON, pack_payload(SPH_ON, TYPE_SPH_INFO, sph_info(rate)),
                         'SPH_ON payload rate %d' % rate):
            return 'error: SPH_ON not acked (pcm left up; send stop)'
        if not self.mailbox(MUTE_UL, 0):
            return 'error: UL unmute not acked'
        self.muted = False
        return 'ok speech on at %d Hz' % rate

    def stop(self):
        if not self.on:
            return 'ok speech already off'
        notes = []
        if not self.mailbox(MUTE_UL, 1):
            notes.append('UL mute not acked')
        self.muted = True
        self.pcm.close()
        if not self.mailbox(SPH_OFF):
            notes.append('SPH_OFF not acked')
        elif self.shm:
            self.shm.reset_indices()
        self.on = False
        return 'ok speech off' + ('' if not notes else ' (%s)' % ', '.join(notes))

    def format_shm(self):
        if not self.shm:
            return
        try:
            self.shm.ensure()
        except (OSError, ValueError) as e:
            self.log('raw audio shm: %s' % e)

    def format_shm_when_ready(self):
        if wait_modem_ready(self.fd):
            self.format_shm()
        else:
            self.log('modem not ready after 300 s, raw audio shm left for start')

    def mute(self, on):
        if not self.on:
            return 'error: speech off'	# the HAL does not send mute without an application
        if not self.mailbox(MUTE_UL, 1 if on else 0):
            return 'error: not acked'
        self.muted = on
        return 'ok UL %s' % ('muted' if on else 'unmuted')

    def status(self):
        return 'ok on=%d rate=%s ul_muted=%d epof=%d' % (self.on, self.rate, self.muted, self.epof)

    def command(self, line):
        w = line.split()
        if not w:
            return 'error: empty command'
        try:
            if w[0] == 'start':
                return self.start(int(w[1]) if len(w) > 1 else 32000)
            if w[0] == 'stop':
                return self.stop()
            if w[0] == 'mute' and len(w) == 2 and w[1] in ('on', 'off'):
                return self.mute(w[1] == 'on')
            if w[0] == 'status':
                return self.status()
            if w[0] == 'mailbox' and 2 <= len(w) <= 4:
                args = [int(x, 16) for x in w[1:]]
                return 'ok' if self.mailbox(*args) else 'error: not sent or not acked'
        except ValueError as e:
            return 'error: %s' % e
        return 'error: unknown command %r' % line


def serve(a, log):
    while True:
        try:
            fd = os.open(a.dev, os.O_RDWR)
            break
        except OSError as e:
            if e.errno not in (errno.ENOENT, errno.ENODEV):
                raise
            time.sleep(0.5)
    log('opened %s' % a.dev)
    pcm = HostlessPcm(log, a.card, a.pcm, a.channels, a.period, 2)
    shm = RawAudio(log, a.raw_audio, a.shm_marker)
    sp = Speech(fd, log, pcm, a.ack_timeout, shm)
    threading.Thread(target=sp.reader, daemon=True).start()
    threading.Thread(target=sp.format_shm_when_ready, daemon=True).start()

    try:
        os.unlink(a.socket)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(a.socket)
    os.chmod(a.socket, 0o660)
    srv.listen(4)

    def terminate(signum, frame):
        log('signal %d: %s' % (signum, sp.stop()))
        sys.exit(0)

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    log('control socket %s' % a.socket)
    while True:
        conn, _ = srv.accept()
        with conn:
            conn.settimeout(5)
            try:
                line = conn.recv(256).decode('ascii', 'replace').strip()
            except OSError:
                continue
            log('command: %s' % line)
            reply = sp.command(line)
            log(reply)
            try:
                conn.sendall((reply + '\n').encode())
            except OSError:
                pass


def client(a):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(a.ack_timeout * 3 + 10)
    s.connect(a.socket)
    s.sendall((' '.join([a.cmd] + a.args) + '\n').encode())
    reply = s.recv(4096).decode('ascii', 'replace').strip()
    print(reply)
    return 0 if reply.startswith('ok') else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('cmd', choices=['daemon', 'start', 'stop', 'mute', 'status', 'mailbox'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--dev', default='/dev/ccci_aud')
    ap.add_argument('--raw-audio', default='/dev/ccci_raw_audio')
    ap.add_argument('--shm-marker', default='/run/cosmo-speechd.shm_init',
                    help='present once the raw audio shm was formatted this boot')
    ap.add_argument('--socket', default='/run/cosmo-speechd.sock')
    ap.add_argument('--card', default='cosmo', help='ALSA card id or index')
    ap.add_argument('--pcm', type=int, default=3, help='Voice_MD1 PCM device number')
    ap.add_argument('--channels', type=int, default=2)
    ap.add_argument('--period', type=int, default=1024, help='frames (HAL: 1024 x 2)')
    ap.add_argument('--ack-timeout', type=float, default=3.0, help='seconds')
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args()

    if a.cmd != 'daemon':
        sys.exit(client(a))

    def log(s):
        if not a.quiet:
            print('%.3f %s' % (time.monotonic(), s), flush=True)

    serve(a, log)


if __name__ == '__main__':
    main()

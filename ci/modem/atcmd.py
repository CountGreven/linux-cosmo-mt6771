#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Send AT commands to /dev/wwan0at0 and print the answers: atcmd.py AT ATI AT+CSQ"""
import os, re, select, sys, time

fd = os.open('/dev/wwan0at0', os.O_RDWR | os.O_NONBLOCK)


def cmd(c, wait):
    os.write(fd, (c + '\r').encode())
    out = b''
    end = time.time() + wait
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            out += os.read(fd, 8192)
            if b'\r\nOK\r\n' in out or b'ERROR' in out:
                break
    print('>>', c)
    print(out.decode(errors='replace').strip())
    print()


for c in sys.argv[1:]:
    cmd(c, 240 if c.endswith('=?') else 5)

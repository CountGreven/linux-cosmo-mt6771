#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# ip link add <name> parentdev <parent> type wwan linkid <id>, for iproute2 older than 5.14
import os, socket, struct, sys
IFLA_IFNAME, IFLA_LINKINFO, IFLA_PARENT_DEV_NAME = 3, 18, 56
IFLA_INFO_KIND, IFLA_INFO_DATA, IFLA_WWAN_LINK_ID = 1, 2, 1
def attr(t, payload):
    l = 4 + len(payload)
    return struct.pack('=HH', l, t) + payload + b'\0' * ((4 - l % 4) % 4)
name, parent, link = sys.argv[1], sys.argv[2], int(sys.argv[3])
info = attr(IFLA_INFO_KIND, b'wwan\0') + attr(IFLA_INFO_DATA, attr(IFLA_WWAN_LINK_ID, struct.pack('=I', link)))
body = struct.pack('=BxHiII', socket.AF_UNSPEC, 0, 0, 0, 0)
body += attr(IFLA_IFNAME, name.encode() + b'\0') + attr(IFLA_PARENT_DEV_NAME, parent.encode() + b'\0') + attr(IFLA_LINKINFO, info)
RTM_NEWLINK, NLM_F = 16, 0x1 | 0x4 | 0x400 | 0x200   # REQUEST|ACK|CREATE|EXCL
msg = struct.pack('=IHHII', 16 + len(body), RTM_NEWLINK, NLM_F, 1, 0) + body
s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE)
s.send(msg)
r = s.recv(4096)
err = struct.unpack_from('=i', r, 16)[0]
print('result', err, os.strerror(-err) if err else 'ok')

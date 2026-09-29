"""Right USB-C port protocol with the CoDi STM32, as Android's AmoledisonThread does it.

CMD 144 (right OTG status) is sent on a right sink attach (2, 1) and detach (3, 0): the STM32
switches the right port's D+/D- only after it. CMD 142 from the STM32 is answered 1 s later
with the phone's USB status number, or with a UUG_ON write while the left port is in OTG
(gpu-video-camera-usb.org 8.2, 8.10).
"""
import glob
import logging
import os
import struct
import threading
import time

log = logging.getLogger("codi")

CMD_SYNC_USB_STATUS = 142
CMD_SYNC_RIGHT_USB_OTG_STATUS = 144
# msg_common.msg_ctr starts at 1234 and is never advanced
ANDROID_SEQUENCE = 1234
# codiReset.py waits this long after releasing reset before talking to the STM32
STM32_BOOT_S = 4.0

VENDOR_CHARGE_STATUS = "/proc/AEON_CHARGE_STATUS"
VENDOR_USB_CONTROL = "/proc/AEON_USB_CONTROL"

# PhoneWindowManager / AmoledisonThread matchingUSBStatus
USB_STATUS = {
    "left_charging_and_hdmi": 1,
    "left_charging_and_right_otg": 2,
    "right_charging_and_left_otg": 3,
    "left_or_right_charging": 4,
    "left_otg_and_hdmi": 5,
    "hdmi_only": 6,
    "left_otg_only": 7,
    "right_otg_only": 8,
    "discharge": 9,
}

OTG_NONE, OTG_LEFT, OTG_RIGHT = 0, 2, 3


def frame(cmd, value, seq=ANDROID_SEQUENCE):
    payload = struct.pack(">H", value)
    return b"X!X!" + struct.pack(">III", 16 + len(payload), cmd, seq) + payload


def matching_usb_status(name):
    return USB_STATUS.get((name or "").strip(), 0)


def charge_status(charging, hdmi, otg):
    """The vendor kpd.c CHARGE_STATUS_read string; "" where it prints nothing."""
    if charging:
        if hdmi:
            return "left_charging_and_hdmi"
        return {OTG_RIGHT: "left_charging_and_right_otg",
                OTG_LEFT: "right_charging_and_left_otg",
                OTG_NONE: "left_or_right_charging"}.get(otg, "")
    return {(True, OTG_LEFT): "left_otg_and_hdmi",
            (True, OTG_NONE): "hdmi_only",
            (False, OTG_LEFT): "left_otg_only",
            (False, OTG_RIGHT): "right_otg_only",
            (False, OTG_NONE): "discharge"}.get((bool(hdmi), otg), "")


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


class Sysfs:
    """Mainline stand-ins for aeon_charging_enable, hdmi_plug_in_flag and aeon_otg_enable."""

    def __init__(self, root="/"):
        self.root = root

    def _p(self, path):
        return os.path.join(self.root, path.lstrip("/"))

    def _source_partner(self, port):
        base = "/sys/class/typec/%s" % port
        return (os.path.isdir(self._p(base + "-partner")) and
                "[source]" in _read(self._p(base + "/power_role")))

    def right_sink_attached(self):
        return self._source_partner("port1")

    def charging(self):
        for d in glob.glob(self._p("/sys/class/power_supply/*")):
            if _read(d + "/type") != "Battery" and _read(d + "/online") == "1":
                return True
        return False

    def hdmi(self):
        return any(_read(s) == "connected"
                   for s in glob.glob(self._p("/sys/class/drm/card*-HDMI-A-*/status")))

    def otg(self):
        left = self._source_partner("port0")
        # the fusb301 driver gives an HDMI adapter no USB role; the vendor never sets 3 for one
        right = self.right_sink_attached() and not self.hdmi()
        if left and right:
            # vendor: last writer wins; the mux follows the port that took the data lines
            mux = _read(self._p("/sys/devices/platform/usb-role-mux/active"))
            return OTG_RIGHT if mux == "right" else OTG_LEFT
        return OTG_LEFT if left else OTG_RIGHT if right else OTG_NONE

    def charge_status(self):
        return charge_status(self.charging(), self.hdmi(), self.otg())


def local_status_name(sysfs):
    if os.path.exists(VENDOR_CHARGE_STATUS):
        return _read(VENDOR_CHARGE_STATUS)
    return sysfs.charge_status()


def write_usb_control(value):
    if not os.path.exists(VENDOR_USB_CONTROL):
        # UUG_ON has no mainline user interface; tcpm owns the MT6370 boost
        log.info("cosmo_usb: UUG_ON %s requested, no mainline interface", value)
        return
    with open(VENDOR_USB_CONTROL, "w") as f:
        f.write(value)


class RightUsb:
    def __init__(self, send, sysfs=None, delay=1.0, timer=threading.Timer,
                 status_name=None, usb_control=write_usb_control):
        self.send = send
        self.sysfs = sysfs or Sysfs()
        self.delay = delay
        self.timer = timer
        self.status_name = status_name or (lambda: local_status_name(self.sysfs))
        self.usb_control = usb_control
        self.attached = False

    def _later(self, fn, *args):
        t = self.timer(self.delay, fn, args)
        t.daemon = True
        t.start()

    def _send_144(self, attached):
        for v in (2, 1) if attached else (3, 0):
            log.info("cosmo_usb: -> CMD 144 %d", v)
            self.send(frame(CMD_SYNC_RIGHT_USB_OTG_STATUS, v))

    def poll(self):
        # starts False, so a partner present at codiServer start is sent too
        now = self.sysfs.right_sink_attached()
        if now == self.attached:
            return
        self.attached = now
        self._send_144(now)

    def after_stm32_reset(self):
        # a reset STM32 forgets the right port; detached is its reset state
        t = self.timer(STM32_BOOT_S, self._resend, ())
        t.daemon = True
        t.start()

    def _resend(self):
        self.attached = self.sysfs.right_sink_attached()
        if self.attached:
            self._send_144(True)

    def on_usb_status(self, payload):
        re_status = struct.unpack(">H", payload[:2])[0]
        log.info("cosmo_usb: <- CMD 142 %d", re_status)
        self._later(self._answer, re_status)

    def _answer(self, re_status):
        local = matching_usb_status(self.status_name())
        if re_status == 0:
            log.info("cosmo_usb: -> CMD 142 %d", local)
            self.send(frame(CMD_SYNC_USB_STATUS, local))
        elif re_status == 1 and local in (5, 7):
            self._later(self.usb_control, "4")
        elif re_status == 2 and local == 7:
            self._later(self.usb_control, "5")

    def watch(self, period=1.0):
        while True:
            try:
                self.poll()
            except Exception as e:
                log.error("cosmo_usb: %s", e)
            time.sleep(period)


_right = None


def _serial_send(data):
    import SerialPortManager
    sock = SerialPortManager.socket
    # not while codiUpdate has the port at the upload baud rate
    if sock is not None and sock.baudrate == 115200:
        SerialPortManager.sendCommand(data)


def start():
    global _right
    _right = RightUsb(_serial_send)
    threading.Thread(target=_right.watch, name="cosmo_usb", daemon=True).start()


def on_usb_status(payload):
    if _right is not None:
        _right.on_usb_status(payload)


def after_stm32_reset():
    if _right is not None:
        _right.after_stm32_reset()

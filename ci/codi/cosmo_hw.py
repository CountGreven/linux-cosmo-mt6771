"""Mainline stand-ins for the vendor /proc files codi-app writes and reads.

proc_open() returns the real /proc file when it exists (vendor kernel) and otherwise a small
file-like object that performs the same action through mainline interfaces: GPIO character
device lines for the STM32 control pins, LED class devices for the AW9524 cover LEDs and the
keyboard backlight, and the power_supply battery for the charge level.
"""
import fcntl
import os
import struct
import sys

GPIO_CHIP = "/dev/gpiochip0"
GPIO_STM32_RESET = 77
GPIO_STM32_DL_FW = 80

# vendor aw9524_led_proc: LED id -> sysfs suffix; id 6 is ids 7, 2 and 3 together
LED_IDS = {1: ["1"], 2: ["2"], 3: ["3"], 7: ["7"], 6: ["7", "2", "3"]}
LED_COLORS = {1: "red", 2: "green", 3: "blue"}


def _ioc(nr, size):
    return (3 << 30) | (size << 16) | (0xB4 << 8) | nr


def set_gpio(line, value):
    """Drive one SoC GPIO; the pin keeps the level after the request is released."""
    offs = [line] + [0] * 63
    attrs = struct.pack("<IIQQ", 2, 0, 1 if value else 0, 1) + b"\0" * (240 - 24)
    cfg = struct.pack("<QI20x", 1 << 3, 1) + attrs
    req = bytearray(struct.pack("<64I32s", *offs, b"codi") + cfg +
                    struct.pack("<II20xi", 1, 0, 0))
    chip = os.open(GPIO_CHIP, os.O_RDWR)
    try:
        fcntl.ioctl(chip, _ioc(0x07, 592), req)
        os.close(struct.unpack_from("<i", req, 588)[0])
    finally:
        os.close(chip)


def _write_sysfs(path, value):
    with open(path, "w") as f:
        f.write(str(value))


def set_led(text):
    """Vendor command string: <id><color><level 0-7>, or 4<level 0-5> for the keyboard light."""
    if text[0] == "4":
        _write_sysfs("/sys/class/leds/white:kbd_backlight/brightness", int(text[1]))
        return
    led_id, color, level = int(text[0]), int(text[1]), int(text[2])
    if led_id not in LED_IDS or color not in LED_COLORS:
        return
    step = 8 if color == 1 else 10
    for suffix in LED_IDS[led_id]:
        _write_sysfs("/sys/class/leds/%s:indicator-%s/brightness" % (LED_COLORS[color], suffix),
                     level * step)


def battery_status():
    with open("/sys/class/power_supply/battery/capacity") as f:
        return "0,%d" % int(f.read())


class _Compat:
    def __init__(self, path):
        self.path = path

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def write(self, text):
        text = text.strip()
        if self.path == "/proc/AEON_RESET_STM32":
            set_gpio(GPIO_STM32_RESET, text == "1")
            usb = sys.modules.get("cosmo_usb")
            if text == "0" and usb is not None:
                usb.after_stm32_reset()
        elif self.path == "/proc/AEON_STM32_DL_FW":
            set_gpio(GPIO_STM32_DL_FW, text == "1")
        elif self.path == "/proc/aw9524_led_proc":
            set_led(text)
        return len(text)

    def read(self):
        if self.path == "/proc/battery_status":
            return battery_status()
        return ""


def proc_open(path, mode="r"):
    if os.path.exists(path):
        return open(path, mode)
    return _Compat(path)

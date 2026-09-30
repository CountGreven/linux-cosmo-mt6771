import importlib.util
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import codi_patch  # noqa: E402
import cosmo_usb as cu  # noqa: E402


class Frames(unittest.TestCase):
    def test_cmd144_matches_android(self):
        for v in range(4):
            self.assertEqual(cu.frame(144, v).hex(), (
                             "58215821 00000012 00000090 000004d2 000%x" % v).replace(" ", ""))

    def test_cmd142_reply(self):
        self.assertEqual(cu.frame(142, 8).hex(), "58215821 00000012 0000008e 000004d2 0008".replace(" ", ""))


class Mapping(unittest.TestCase):
    def test_matching_usb_status(self):
        self.assertEqual([cu.matching_usb_status(n) for n in sorted(cu.USB_STATUS, key=cu.USB_STATUS.get)],
                         list(range(1, 10)))
        self.assertEqual(cu.matching_usb_status("discharge\n"), 9)
        self.assertEqual(cu.matching_usb_status(""), 0)
        self.assertEqual(cu.matching_usb_status("bogus"), 0)

    def test_charge_status_table(self):
        L, R, N = cu.OTG_LEFT, cu.OTG_RIGHT, cu.OTG_NONE
        cases = [
            ((1, 1, N), 1), ((1, 1, L), 1), ((1, 1, R), 1),
            ((1, 0, R), 2), ((1, 0, L), 3), ((1, 0, N), 4),
            ((0, 1, L), 5), ((0, 1, N), 6), ((0, 0, L), 7),
            ((0, 0, R), 8), ((0, 0, N), 9),
            ((0, 1, R), 0),  # kpd.c prints nothing
        ]
        for args, want in cases:
            self.assertEqual(cu.matching_usb_status(cu.charge_status(*args)), want, args)


class FakeSys:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def put(self, path, text):
        p = os.path.join(self.root, path.lstrip("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text + "\n")

    def mkdir(self, path):
        os.makedirs(os.path.join(self.root, path.lstrip("/")), exist_ok=True)

    def rmdir(self, path):
        os.rmdir(os.path.join(self.root, path.lstrip("/")))

    def device_link(self, port, marker):
        """Create /sys/class/typec/portN/device -> symlink target containing 'marker'."""
        link = self.root + "/sys/class/typec/port%d/device" % port
        os.makedirs(os.path.dirname(link), exist_ok=True)
        if os.path.lexists(link):
            os.unlink(link)
        os.symlink("/tmp/fake/path/%s" % marker, link)

    def port(self, n, partner, role):
        self.put("/sys/class/typec/port%d/power_role" % n,
                 "[source] sink" if role == "source" else "source [sink]")
        if partner:
            self.mkdir("/sys/class/typec/port%d-partner" % n)


class SysfsState(unittest.TestCase):
    def setUp(self):
        self.fs = FakeSys()
        self.addCleanup(self.fs.tmp.cleanup)
        self.s = cu.Sysfs(self.fs.root)
        self.fs.put("/sys/class/power_supply/battery/type", "Battery")
        self.fs.put("/sys/class/power_supply/battery/online", "1")
        self.fs.put("/sys/class/power_supply/mt6370-charger/type", "USB")
        self.fs.put("/sys/class/power_supply/mt6370-charger/online", "0")
        self.fs.device_link(0, "mt6370-tcpc")
        self.fs.device_link(1, "3-0025")
        self.fs.put("/sys/devices/platform/usb-role-mux/active", "left")

    def status(self):
        return cu.matching_usb_status(self.s.charge_status())

    def test_discharge(self):
        self.fs.port(0, False, "sink")
        self.fs.port(1, False, "sink")
        self.assertEqual(self.status(), 9)

    def test_right_otg_only(self):
        self.fs.port(1, True, "source")
        self.assertTrue(self.s.right_sink_attached())
        self.assertEqual(self.status(), 8)

    def test_right_source_partner_is_not_a_sink(self):
        self.fs.port(1, True, "sink")
        self.assertFalse(self.s.right_sink_attached())

    def test_left_charging_and_right_otg(self):
        self.fs.put("/sys/class/power_supply/mt6370-charger/online", "1")
        self.fs.port(1, True, "source")
        self.assertEqual(self.status(), 2)

    def test_left_otg_only(self):
        self.fs.port(0, True, "source")
        self.assertEqual(self.status(), 7)

    def test_both_otg_follows_mux(self):
        self.fs.port(0, True, "source")
        self.fs.port(1, True, "source")
        self.assertEqual(self.status(), 7)
        self.fs.put("/sys/devices/platform/usb-role-mux/active", "right")
        self.assertEqual(self.status(), 8)

    def test_hdmi_adapter_on_right(self):
        self.fs.port(1, True, "source")
        self.fs.put("/sys/class/drm/card0-HDMI-A-1/status", "connected")
        self.assertEqual(self.status(), 6)
        self.fs.port(0, True, "source")
        self.assertEqual(self.status(), 5)


class ImmediateTimer:
    calls = []

    def __init__(self, delay, fn, args):
        self.delay, self.fn, self.args = delay, fn, args

    def start(self):
        ImmediateTimer.calls.append(self.delay)
        self.fn(*self.args)


class Protocol(unittest.TestCase):
    def setUp(self):
        self.fs = FakeSys()
        self.addCleanup(self.fs.tmp.cleanup)
        self.sent = []
        self.ctl = []
        self.status = "discharge"
        ImmediateTimer.calls = []
        self.fs.device_link(0, "mt6370-tcpc")
        self.fs.device_link(1, "3-0025")
        self.r = cu.RightUsb(self.sent.append, cu.Sysfs(self.fs.root), timer=ImmediateTimer,
                             status_name=lambda: self.status, usb_control=self.ctl.append)

    def values(self):
        return [(int(f[11]), int.from_bytes(f[16:], "big")) for f in self.sent]

    def test_attach_detach(self):
        self.fs.port(1, False, "sink")
        self.r.poll()
        self.assertEqual(self.sent, [])
        self.fs.port(1, True, "source")
        self.r.poll()
        self.r.poll()
        self.assertEqual(self.values(), [(144, 2), (144, 1)])
        self.fs.rmdir("/sys/class/typec/port1-partner")
        self.r.poll()
        self.assertEqual(self.values(), [(144, 2), (144, 1), (144, 3), (144, 0)])

    def test_present_at_start_is_sent(self):
        self.fs.port(1, True, "source")
        self.r.poll()
        self.assertEqual(self.values(), [(144, 2), (144, 1)])

    def test_resend_after_stm32_reset(self):
        self.fs.port(1, True, "source")
        self.r.poll()
        self.sent.clear()
        self.r.after_stm32_reset()
        self.assertEqual(self.values(), [(144, 2), (144, 1)])
        self.assertEqual(ImmediateTimer.calls, [cu.STM32_BOOT_S])
        self.r.poll()
        self.assertEqual(len(self.sent), 2)

    def test_no_resend_when_detached(self):
        self.fs.port(1, False, "sink")
        self.r.after_stm32_reset()
        self.assertEqual(self.sent, [])

    def test_reset_release_through_cosmo_hw(self):
        import cosmo_hw
        with mock.patch.object(cosmo_hw, "set_gpio") as gpio, \
                mock.patch.object(cu, "_right") as right, \
                mock.patch.object(cosmo_hw.os.path, "exists", return_value=False):
            with cosmo_hw.proc_open("/proc/AEON_RESET_STM32", "w") as f:
                f.write("1")
            right.after_stm32_reset.assert_not_called()
            with cosmo_hw.proc_open("/proc/AEON_RESET_STM32", "w") as f:
                f.write("0")
            right.after_stm32_reset.assert_called_once_with()
            self.assertEqual([c[0] for c in gpio.call_args_list],
                             [(cosmo_hw.GPIO_STM32_RESET, True), (cosmo_hw.GPIO_STM32_RESET, False)])

    def test_142_ask_answers_status_after_1s(self):
        self.status = "right_otg_only"
        self.r.on_usb_status(b"\x00\x00")
        self.assertEqual(self.values(), [(142, 8)])
        self.assertEqual(ImmediateTimer.calls, [1.0])

    def test_142_uug_writes(self):
        for re, status, want in [(1, "left_otg_only", ["4"]), (1, "left_otg_and_hdmi", ["4"]),
                                 (1, "discharge", []), (2, "left_otg_only", ["5"]),
                                 (2, "left_otg_and_hdmi", []), (3, "left_otg_only", [])]:
            self.ctl.clear()
            ImmediateTimer.calls = []
            self.status = status
            self.r.on_usb_status(bytes([0, re]))
            self.assertEqual(self.ctl, want, (re, status))
            self.assertEqual(ImmediateTimer.calls, [1.0] * (1 + len(want)))
        self.assertEqual(self.sent, [])


SRC = os.environ.get("CODI_SRC", "/storage/kernel/build/claude-tmp/codi-src/"
                     "Cosmo-CoDiOS_codi-app/codi-app")


class Patch(unittest.TestCase):
    def test_anchor_missing_fails(self):
        with self.assertRaises(ValueError):
            codi_patch.patch("codiServer.py", "nothing here\n")

    @unittest.skipUnless(os.path.isdir(SRC), "codi-app source not present")
    def test_upstream_sources(self):
        for name in codi_patch.PATCHES:
            with open(os.path.join(SRC, name)) as f:
                out = codi_patch.patch(name, f.read())
            compile(out, name, "exec")
            self.assertIn("cosmo_usb", out)



class PortChipResolution(unittest.TestCase):
    """Verify port resolution by chip type rather than hardcoded port numbers."""

    def setUp(self):
        self.fs = FakeSys()
        self.addCleanup(self.fs.tmp.cleanup)
        self.s = cu.Sysfs(self.fs.root)
        self.fs.put("/sys/class/power_supply/battery/type", "Battery")
        self.fs.put("/sys/class/power_supply/battery/online", "0")

    def test_swapped_ports(self):
        """port0=FUSB301 and port1=mt6370 (swapped) still identifies right port."""
        self.fs.device_link(0, "3-0025")   # FUSB301 on port0 (swapped order)
        self.fs.device_link(1, "mt6370-tcpc")  # mt6370 on port1
        self.fs.port(0, True, "source")    # FUSB301 in source role
        # new code finds it by chip even though it is on port0 not port1
        self.assertTrue(self.s.right_sink_attached())

    def test_normal_order(self):
        """port0=mt6370, port1=FUSB301 (normal) identifies right port."""
        self.fs.device_link(0, "mt6370-tcpc")
        self.fs.device_link(1, "3-0025")
        self.fs.port(1, True, "source")
        self.assertTrue(self.s.right_sink_attached())

    def test_fusb_missing(self):
        """FUSB301 port missing -> no right partner."""
        self.fs.device_link(0, "mt6370-tcpc")
        # no device_link for port1 -- FUSB301 absent
        self.assertFalse(self.s.right_sink_attached())



class HookHandover(unittest.TestCase):
    def load(self):
        spec = importlib.util.spec_from_file_location(
            "right_usb_otg", os.path.join(os.path.dirname(HERE), "right-usb-otg.py"))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        return m

    def run_main(self, holders):
        m = self.load()
        with mock.patch.object(m, "holders", return_value=holders), \
                mock.patch.object(sys, "argv", ["right-usb-otg.py", "1"]), \
                mock.patch.object(m.os, "open", side_effect=AssertionError("opened tty")), \
                redirect_stdout(io.StringIO()), mock.patch("sys.stderr", io.StringIO()):
            return m.main()

    def test_codiserver_holds_tty(self):
        self.assertEqual(self.run_main(["812 python3 /usr/lib/codi/codiServer.py"]), 0)

    def test_other_holder_still_fails(self):
        self.assertEqual(self.run_main(["900 minicom -D /dev/ttyS1"]), 2)


if __name__ == "__main__":
    unittest.main()

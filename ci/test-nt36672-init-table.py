#!/usr/bin/env python3
"""Unit tests for ci/nt36672-init-table.py, the vendor-table-to-C translator.

    python3 ci/test-nt36672-init-table.py

The translator is what stands between the vendor's 4.4 LCM table and the mainline driver, so its own
behaviour is pinned here: which preprocessor branch it reads, how it chooses DCS against generic packets
(the vendor's rule, not ours), how delays come out, and that a single changed byte in the driver fails the
check.
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("nt36672_init_table", HERE / "nt36672-init-table.py")
tbl = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = tbl        # dataclasses look their module up here
spec.loader.exec_module(tbl)

SAMPLE = """
#define REGFLAG_DELAY       0xFFFC
static struct LCM_setting_table lcm_suspend_setting[] = {
	{0x28,0,{}},
	{REGFLAG_DELAY, 50, {}},
	{0x10,0,{}},
	{REGFLAG_DELAY, 120, {}},

	{REGFLAG_END_OF_TABLE, 0x00, {}}
};

static struct LCM_setting_table init_setting[] = {
	#if 1
	{0xFF,1,{0x20}},   // page select
	/* {0x99,1,{0x99}}, commented out */
	{0x01,1,{0x33}},
	{0xB0,4,{0x00,0xAA,0x00,0xB6}},
	{0x36, 1, {0x03}},
	{0x11, 1, {0x00}},
	{REGFLAG_DELAY, 120, {}},
	{REGFLAG_END_OF_TABLE, 0x00, {}}
#else
	{0xEE,1,{0xEE}},
	{REGFLAG_END_OF_TABLE, 0x00, {}}
	#endif
};
"""


class Parse(unittest.TestCase):
    def test_reads_the_live_preprocessor_branch_only(self):
        rows = tbl.extract_table(SAMPLE, "init_setting")
        self.assertEqual([r.cmd for r in rows], [0xFF, 0x01, 0xB0, 0x36, 0x11, tbl.DELAY])
        self.assertNotIn(0xEE, [r.cmd for r in rows])
        self.assertNotIn(0x99, [r.cmd for r in rows])

    def test_delay_rows_carry_milliseconds_in_count(self):
        rows = tbl.extract_table(SAMPLE, "lcm_suspend_setting")
        self.assertEqual([(r.cmd, r.count) for r in rows],
                         [(0x28, 0), (tbl.DELAY, 50), (0x10, 0), (tbl.DELAY, 120)])

    def test_count_must_match_the_parameters(self):
        bad = SAMPLE.replace("{0xB0,4,{0x00,0xAA,0x00,0xB6}}", "{0xB0,5,{0x00,0xAA,0x00,0xB6}}")
        with self.assertRaises(tbl.TableError):
            tbl.extract_table(bad, "init_setting")

    def test_rows_after_end_of_table_are_an_error(self):
        bad = SAMPLE.replace("{REGFLAG_DELAY, 120, {}},\n\t{REGFLAG_END_OF_TABLE, 0x00, {}}\n#else",
                             "{REGFLAG_END_OF_TABLE, 0x00, {}},\n\t{0x29,0,{}}\n#else")
        with self.assertRaises(tbl.TableError):
            tbl.extract_table(bad, "init_setting")

    def test_unknown_conditionals_are_refused(self):
        bad = SAMPLE.replace("#if 1", "#ifdef BUILD_LK")
        with self.assertRaises(tbl.TableError):
            tbl.extract_table(bad, "init_setting")

    def test_missing_table_is_an_error(self):
        with self.assertRaises(tbl.TableError):
            tbl.extract_table(SAMPLE, "no_such_table")


class Emit(unittest.TestCase):
    def setUp(self):
        self.init = tbl.emit_function("f_init", tbl.extract_table(SAMPLE, "init_setting"))
        self.off = tbl.emit_function("f_off", tbl.extract_table(SAMPLE, "lcm_suspend_setting"))

    def test_below_0xb0_is_dcs_and_from_0xb0_generic_as_the_vendor_sends_it(self):
        # ddp_dsi.c DSI_set_cmdq_V2: cmd < 0xB0 -> DCS packets, otherwise generic packets
        self.assertIn("\tmipi_dsi_dcs_write_seq_multi(ctx, 0x01, 0x33);\n", self.init)
        self.assertIn("\tmipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x20);\n", self.init)
        self.assertIn("\tmipi_dsi_generic_write_seq_multi(ctx, 0xb0, 0x00, 0xaa, 0x00, 0xb6);\n",
                      self.init)

    def test_parameterless_standard_dcs_uses_the_helpers(self):
        self.assertIn("\tmipi_dsi_dcs_set_display_off_multi(ctx);\n", self.off)
        self.assertIn("\tmipi_dsi_dcs_enter_sleep_mode_multi(ctx);\n", self.off)

    def test_sleep_out_with_a_parameter_stays_a_raw_write(self):
        # the vendor sends 0x11 with one parameter (a 0x15 packet), which the helper would not
        self.assertIn("\tmipi_dsi_dcs_write_seq_multi(ctx, 0x11, 0x00);\n", self.init)

    def test_delays(self):
        self.assertIn("\tmipi_dsi_msleep(ctx, 120);\n", self.init)
        self.assertIn("\tmipi_dsi_msleep(ctx, 50);\n", self.off)

    def test_shape(self):
        self.assertTrue(self.init.startswith(
            "static void f_init(struct mipi_dsi_multi_context *ctx)\n{\n"))
        self.assertTrue(self.init.endswith("\n}\n"))

    def test_long_rows_wrap_within_100_columns(self):
        rows = [tbl.Row(0xB1, 16, list(range(0x40, 0x50)))]
        text = tbl.emit_function("f", rows)
        for line in text.splitlines():
            self.assertLessEqual(len(line.expandtabs(8)), 100, line)
        body = " ".join(l.strip() for l in text.splitlines()[2:-1])
        self.assertIn(", ".join(f"0x{b:02x}" for b in range(0x40, 0x50)), body)


class Check(unittest.TestCase):
    def test_check_passes_on_identical_and_fails_on_one_byte(self):
        rows = tbl.extract_table(SAMPLE, "init_setting")
        func = tbl.emit_function("f_init", rows)
        with tempfile.TemporaryDirectory() as d:
            drv = Path(d) / "drv.c"
            drv.write_text("/* header */\n\n" + func + "\nstatic int other(void)\n{\n\treturn 0;\n}\n")
            self.assertEqual(tbl.compare(drv.read_text(), {"f_init": func}), [])
            drv.write_text(drv.read_text().replace("0x33", "0x34"))
            self.assertNotEqual(tbl.compare(drv.read_text(), {"f_init": func}), [])

    def test_check_fails_when_the_function_is_missing(self):
        self.assertNotEqual(tbl.compare("int x;\n", {"f_init": "static void f_init(void)\n{\n}\n"}), [])


class RealVendorTable(unittest.TestCase):
    """The vendored copy of the Cosmo's LCM driver, as the driver is generated from it."""

    def test_vendored_copy_is_the_recorded_one(self):
        self.assertEqual(tbl.sha256(tbl.VENDOR_COPY), tbl.VENDOR_SHA256)

    def test_the_real_tables(self):
        src = tbl.VENDOR_COPY.read_text()
        init = tbl.extract_table(src, "init_setting")
        off = tbl.extract_table(src, "lcm_suspend_setting")
        cmds = [r for r in init if r.cmd != tbl.DELAY]
        self.assertEqual(init[-4:], [tbl.Row(0x11, 1, [0x00]), tbl.Row(tbl.DELAY, 120, []),
                                     tbl.Row(0x29, 1, [0x00]), tbl.Row(tbl.DELAY, 10, [])])
        self.assertEqual(init[0], tbl.Row(0xFF, 1, [0x20]))
        self.assertEqual(len(cmds), len(init) - 2)
        self.assertEqual(off, [tbl.Row(0x28, 0, []), tbl.Row(tbl.DELAY, 50, []),
                               tbl.Row(0x10, 0, []), tbl.Row(tbl.DELAY, 120, [])])


if __name__ == "__main__":
    unittest.main(verbosity=2)

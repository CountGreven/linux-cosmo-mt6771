#!/usr/bin/env python3
"""Battery capacity and current logger for Planet Cosmo (mt6771).

Logs periodic measurements from sysfs power-supply class to CSV.
Usage: battery-log.py [--interval SECONDS] [--out FILE]
Use --summary [FILE] to print aggregate statistics from a log file.
Supports BATLOG_SYSFS env var to override the sysfs base path."""

import argparse
import csv
import os
import signal
import sys
import time

DEFAULT_INTERVAL = 60
DEFAULT_OUT = "/var/log/battery-log.csv"
BATLOG_SYSFS = os.environ.get("BATLOG_SYSFS", "/sys/class/power_supply")

BATTERY_ATTRS = ("capacity", "voltage_now", "current_now",
                 "charge_counter", "temp", "status")
CHARGER_ATTRS = ("online", "charge_behaviour")


def read_value(path, attr):
    """Read a single sysfs attribute; return empty string on failure."""
    try:
        with open(os.path.join(path, attr), "r") as f:
            return f.read().strip()
    except (IOError, OSError):
        return ""


def read_all():
    """Read battery and charger attributes, returning a list of values."""
    vals = []
    bat_path = os.path.join(BATLOG_SYSFS, "battery")
    chg_path = os.path.join(BATLOG_SYSFS, "mt6370-charger")
    for attr in BATTERY_ATTRS:
        vals.append(read_value(bat_path, attr))
    for attr in CHARGER_ATTRS:
        vals.append(read_value(chg_path, attr))
    return vals


def get_uptime():
    """Return system uptime in seconds as a float."""
    with open("/proc/uptime", "r") as f:
        return float(f.read().split()[0])


HEADER = ["iso_time", "uptime_sec"] + list(BATTERY_ATTRS) + list(CHARGER_ATTRS)


def main():
    parser = argparse.ArgumentParser(
        description="Battery capacity and current logger"
    )
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL,
                        help="Sample interval in seconds (default: %(default)s)")
    parser.add_argument("--out", type=str, default=DEFAULT_OUT,
                        help="Output CSV file path (default: %(default)s)")
    parser.add_argument("--summary", metavar="FILE", nargs="?", const=None,
                        help="Print summary statistics from an existing CSV log")
    args = parser.parse_args()

    if args.summary is not None:
        print_summary(args.summary)
        return

    running = [True]

    def handle_signal(signum, frame):
        running[0] = False

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    file_new = not os.path.isfile(args.out)
    row_count = 0

    with open(args.out, "a", newline="") as csvfile:
        writer = csv.writer(csvfile)
        if file_new:
            writer.writerow(HEADER)
            csvfile.flush()

        while running[0]:
            ts = time.time()
            iso = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ts))
            uptime = get_uptime()
            vals = read_all()
            row = [iso, "%.1f" % uptime] + vals
            writer.writerow(row)
            csvfile.flush()
            row_count += 1
            target = ts + args.interval
            while running[0] and time.time() < target:
                time.sleep(0.1)


def print_summary(logfile):
    """Print summary statistics from a battery log CSV file."""
    if logfile is None:
        logfile = DEFAULT_OUT

    rows = []
    with open(logfile, "r") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            print("ERROR: empty CSV file", file=sys.stderr)
            sys.exit(1)

        for row in reader:
            rows.append(row)

    if not rows:
        print("No data rows found.")
        return

    idx_iso = header.index("iso_time")
    idx_uptime = header.index("uptime_sec")
    idx_capacity = header.index("capacity")
    idx_voltage = header.index("voltage_now")
    idx_current = header.index("current_now")
    idx_time_idx = idx_uptime

    has_charge_counter = "charge_counter" in header
    cc_idx = header.index("charge_counter") if has_charge_counter else None

    first_time = rows[0][idx_iso]
    last_time = rows[-1][idx_iso]

    def safe_float(row, col):
        try:
            return float(row[col])
        except (ValueError, IndexError):
            return None

    cap_first = safe_float(rows[0], idx_capacity)
    cap_last = safe_float(rows[-1], idx_capacity)
    if cap_first is not None and cap_last is not None:
        cap_change = "%.1f" % (cap_last - cap_first)
    else:
        cap_change = None

    currents = []
    total_energy = 0.0
    prev_uptime = None

    for row in rows:
        curr = safe_float(row, idx_current)
        volt = safe_float(row, idx_voltage)
        up = safe_float(row, idx_time_idx)

        if curr is not None and volt is not None and up is not None:
            currents.append(curr)
            if prev_uptime is not None:
                dt_h = (up - prev_uptime) / 3600.0
                total_energy += (volt / 1e6) * (curr / 1e6) * dt_h * 1000

        prev_uptime = up

    if currents:
        avg_current = "%.1f" % (sum(currents) / len(currents))
    else:
        avg_current = None

    energy_est = "%.2f" % total_energy if total_energy > 0 else None

    charge_counter_change = None
    if has_charge_counter:
        cc_first = safe_float(rows[0], cc_idx)
        cc_last = safe_float(rows[-1], cc_idx)
        if cc_first is not None and cc_last is not None:
            charge_counter_change = "%.1f" % (cc_last - cc_first)

    print("=== Battery Log Summary ===")
    print("First reading : %s" % first_time)
    print("Last reading  : %s" % last_time)
    if cap_change is not None:
        print("Capacity delta: %s%%" % cap_change)
    else:
        print("Capacity delta: N/A")
    if avg_current is not None:
        print("Avg current   : %smA" % avg_current)
    if energy_est is not None:
        print("Energy est.   : %s mWh" % energy_est)
    if charge_counter_change is not None:
        print("Charge ctr delta: %s uAh" % charge_counter_change)


if __name__ == "__main__":
    main()

"""Acceptance test for the uBMC IPMI-over-LAN extras: SEL, SDR/sensors, FRU
and chassis boot options.

usage: ipmi-extras-test.py [host] [user] [password] [--trace]
"""
import sys
import traceback

args = [a for a in sys.argv[1:] if not a.startswith("--")]
FLAGS = {a for a in sys.argv[1:] if a.startswith("--")}

HOST = args[0] if len(args) > 0 else "192.168.31.72"
USER = args[1] if len(args) > 1 else "root"
PASSWORD = args[2] if len(args) > 2 else "51L1C0M201B"

FAILS = []


def section(title):
    print("\n=== %s ===" % title)


def run(label, fn):
    try:
        got = fn()
    except Exception as exc:
        print("  %-26s FAIL %s: %s" % (label, type(exc).__name__, exc))
        if "--trace" in FLAGS:
            traceback.print_exc()
        FAILS.append(label)
        return None
    return got


def main():
    from pyghmi.ipmi.command import Command

    print("=== pyghmi -> %s (user %s) ===" % (HOST, USER))
    bmc = Command(bmc=HOST, userid=USER, password=PASSWORD)
    print("  session established")

    # ---------------------------------------------------------------- sensors
    section("SDR / sensors  (ipmitool sensor, chassis status -v)")
    descs = run("get_sensor_descriptions", lambda: list(bmc.get_sensor_descriptions())) or []
    print("  %d sensor(s) in the SDR repository" % len(descs))
    names = sorted(d.get("name", str(i)) for i, d in enumerate(descs))
    for d in descs[:8]:
        print("    %-14s type=%-2s units=%-3s" % (d.get("name"), d.get("type"), d.get("units")))
    if len(descs) > 8:
        print("    ... %d more" % (len(descs) - 8))

    data = run("get_sensor_data", lambda: list(bmc.get_sensor_data())) or []
    shown = 0
    for rec in data:
        val = getattr(rec, "value", None)
        if val is None:
            continue
        print("    %-14s %8s %s" % (getattr(rec, "name", "?"), val,
                                    getattr(rec, "units", "") or ""))
        shown += 1
        if shown >= 10:
            break
    if not data:
        FAILS.append("no sensor readings")

    # ------------------------------------------------------------------- FRU
    section("FRU  (ipmitool fru)")
    inv = run("get_inventory", lambda: dict(bmc.get_inventory())) or {}
    for comp, info in inv.items():
        print("  [%s]" % comp)
        for key in sorted(info):
            if key.endswith("_extra"):
                continue
            print("    %-28s %s" % (key, info[key]))
    if not any(inv.get(c, {}).get("Serial Number") for c in inv):
        FAILS.append("no FRU serial number")

    # ------------------------------------------------------------------- SEL
    section("SEL  (ipmitool sel list / elist)")
    log = run("get_event_log", lambda: list(bmc.get_event_log()))
    if log is not None:
        print("  %d event record(s)" % len(log))
        for ev in log[:4]:
            print("    %s  %s %s" % (ev.get("timestamp"), ev.get("sensor_type", ""),
                                     ev.get("description", "")))
        for ev in log[-2:]:
            print("    %s  %s %s" % (ev.get("timestamp"), ev.get("sensor_type", ""),
                                     ev.get("description", "")))
        if len(log) < 100:
            FAILS.append("SEL record count looks low (%d)" % len(log))

    # --------------------------------------------------------- reading factors
    # Get Sensor Reading Factors (0x04 0x23) is what ipmitool's analog sensors
    # path uses before reading a threshold sensor, so it must answer neatly.
    section("Get Sensor Reading Factors  (0x04 0x23)")
    for sid in (1, 2, 12):
        rf = run("reading factors sensor %d" % sid,
                 lambda sid=sid: bmc.raw_command(netfn=0x04, command=0x23,
                                                 data=(sid,))["data"].hex())
        if rf is not None:
            print("    sensor %-3d %s" % (sid, rf))

    # ----------------------------------------------------------- boot options
    section("Chassis boot options  (ipmitool chassis bootdev)")
    bd = run("get_bootdev", bmc.get_bootdev)
    print("    current: %r" % (bd,))
    run("set_bootdev('optical')", lambda: bmc.set_bootdev("optical", persist=False))
    bd2 = run("get_bootdev after set", bmc.get_bootdev)
    print("    after optical: %r" % (bd2,))
    run("set_bootdev('default')", lambda: bmc.set_bootdev("default"))
    bd3 = run("get_bootdev after reset", bmc.get_bootdev)
    print("    after default: %r" % (bd3,))
    if not bd3 or bd3.get("bootdev") != "default":
        FAILS.append("bootdev did not return to default")

    try:
        bmc.ipmi_session.logout()
    except Exception:
        pass

    print()
    if FAILS:
        print("FAILED: %s" % ", ".join(FAILS))
        return 1
    print("all extras checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())






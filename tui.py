#!/usr/bin/python3
"""bc250-metrics-fix dashboard: live SMU readings, actions run through
bc250-metrics-fix.sh. Standard library only (curses)."""
import curses
import glob
import os
import platform
import re
import subprocess
import sys
import time

NAME = "bc250-metrics-fix"
MOD = "bc250_metrics_fix"
HWMON_NAME = "bc250"
TELEMETRY = f"/sys/module/{MOD}/telemetry"
HERE = os.path.dirname(os.path.realpath(__file__))
SCRIPT = os.environ.get("BC250_SCRIPT", os.path.join(HERE, "bc250-metrics-fix.sh"))
ETC = f"/etc/{NAME}"
OPTIONS_CONF = f"/etc/modprobe.d/{NAME}-options.conf"
MODPROBE_CONF = f"/etc/modprobe.d/{NAME}.conf"
REFRESH_MS = 1000

# Bar scales
CPU_MHZ_MAX = 3500
TEMP_MAX = 100.0
CORE_W_MAX = 8.0
SOCKET_W_MAX = 220.0


def version():
    try:
        with open(SCRIPT) as f:
            for line in f:
                if line.startswith("VERSION="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return "?"


VERSION = version()

# ---------------------------------------------------------------------------
# Data


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def find_hwmon():
    for h in glob.glob("/sys/class/hwmon/hwmon*"):
        if read(os.path.join(h, "name")) == HWMON_NAME:
            return h
    return None


def telemetry_dir():
    """Older builds kept these files on the hwmon device only."""
    if os.path.isfile(os.path.join(TELEMETRY, "summary")):
        return TELEMETRY
    return find_hwmon()


def hwmon_where():
    """Name of the bc250 hwmon device ("hwmon5"), or None when there is none."""
    h = read(os.path.join(TELEMETRY, "hwmon"))
    if h is None:
        h = find_hwmon()
        h = os.path.basename(h) if h else None
    return None if h in (None, "none") else h


def physical_cores():
    ids = set()
    for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/topology/core_id"):
        pkg = read(os.path.join(os.path.dirname(p), "physical_package_id"), "0")
        ids.add((pkg, read(p)))
    return len(ids)


def setting(key):
    line = ""
    try:
        with open(OPTIONS_CONF) as f:
            line = next((l for l in f if l.startswith(f"options {MOD} ")), "")
    except OSError:
        pass
    if key == "sclk":
        return "stock" if "sclk_max=2000" in line else "extended"
    return "on" if "hwmon=1" in line else "off"


def system_state():
    krel = platform.release()
    built = sorted(os.path.basename(os.path.dirname(p))
                   for p in glob.glob(f"{ETC}/*/{MOD}.ko"))
    loaded = os.path.isdir(f"/sys/module/{MOD}")
    return {
        "kernel": krel,
        "amdgpu": os.path.isdir("/sys/module/amdgpu"),
        "loaded": loaded,
        "version": read(f"/sys/module/{MOD}/version", "?") if loaded else None,
        "built": built,
        "built_running": krel in built,
        "installed": os.path.exists(MODPROBE_CONF),
        "sclk": setting("sclk"),
        "hwmon": setting("hwmon"),
        "sclk_max": int(read(f"/sys/module/{MOD}/parameters/sclk_max", "2230") or 2230),
    }


NUM = r"([0-9]+(?:\.[0-9]+)?)"


def parse_summary(text):
    """Parse the telemetry 'summary' attribute (one consistent SMU snapshot)."""
    m = {"cores": [], "l3": []}
    for line in text.splitlines():
        r = re.match(rf"core (\d+):\s+{NUM}MHz\s+{NUM}W\s+{NUM}C\s+{NUM}", line)
        if r:
            m["cores"].append(tuple(float(x) for x in r.groups()[1:]))
            continue
        r = re.match(rf"L3 #(\d+):\s+{NUM}MHz\s+{NUM}C", line)
        if r:
            m["l3"].append((float(r.group(2)), float(r.group(3))))
            continue
        r = re.match(rf"(gfx|soc):\s+{NUM}MHz\s+{NUM}C", line)
        if r:
            m[r.group(1)] = (float(r.group(2)), float(r.group(3)))
            continue
        r = re.match(rf"(vclk|dclk|mem):\s+{NUM}MHz", line)
        if r:
            m[r.group(1)] = float(r.group(2))
            continue
        r = re.match(rf"edge:\s+{NUM}C", line)
        if r:
            m["edge"] = float(r.group(1))
            continue
        r = re.match(rf"pd (cpu|gpu):\s+{NUM}mV\s+{NUM}A\s+{NUM}W", line)
        if r:
            m[r.group(1)] = tuple(float(x) for x in r.groups()[1:])
            continue
        r = re.match(rf"pd socket:\s+{NUM}W", line)
        if r:
            m["socket"] = float(r.group(1))
            continue
        r = re.match(r"throttle:\s+(0x[0-9a-fA-F]+)", line)
        if r:
            m["throttle"] = int(r.group(1), 16)
        r = re.match(r"layout:\s+(\S+)", line)
        if r:
            m["layout"] = r.group(1)
    return m


def metrics():
    d = telemetry_dir()
    if not d:
        return None
    text = read(os.path.join(d, "summary"))
    if not text:
        return None
    m = parse_summary(text)
    m["busy"] = int(read(os.path.join(d, "gpu_busy_percent"), "0") or 0)
    m["state"] = read(os.path.join(d, "state"), "?")
    m["hwmon"] = hwmon_where()
    # 8 cores on SMU firmware without the 8-core metrics patch
    m["unpatched"] = m.get("layout") == "6-core" and physical_cores() >= 8
    return m


# ---------------------------------------------------------------------------
# Drawing

C_TITLE, C_OK, C_WARN, C_BAD, C_DIM, C_ACC, C_TRACK, C_KEY = range(1, 9)

# Level palettes. Colour means level, the same way everywhere:
#   clocks          cool blue scale, brighter as the clock rises
#   power, load     heat scale: green, yellow, orange, red
#   temperatures    the heat scale on fixed thresholds
# xterm-256 indices, with a basic 8-colour fallback.
FREQ_256, FREQ_8 = (67, 33, 45), (curses.COLOR_BLUE, curses.COLOR_BLUE, curses.COLOR_CYAN)
HEAT_256 = (114, 221, 208, 196)
HEAT_8 = (curses.COLOR_GREEN, curses.COLOR_YELLOW, curses.COLOR_YELLOW, curses.COLOR_RED)
FREQ_STEPS = (0.5, 0.85)
HEAT_STEPS = (0.35, 0.65, 0.85)
TEMP_STEPS = (60.0, 75.0, 85.0)
P_FREQ, P_HEAT = 10, 20          # first pair number of each palette


def init_colors():
    curses.start_color()
    try:
        curses.use_default_colors()
        bg = -1
    except curses.error:
        bg = curses.COLOR_BLACK
    rich = curses.COLORS >= 256
    # Slate accent, distinct from the level palettes.
    acc = 103 if rich else curses.COLOR_WHITE
    bar = 60 if rich else curses.COLOR_WHITE
    dim = 245 if rich else curses.COLOR_WHITE
    heat = HEAT_256 if rich else HEAT_8
    curses.init_pair(C_TITLE, 255 if rich else curses.COLOR_BLACK, bar)
    curses.init_pair(C_OK, heat[0], bg)
    curses.init_pair(C_WARN, heat[1], bg)
    curses.init_pair(C_BAD, heat[3], bg)
    curses.init_pair(C_DIM, dim, bg)
    curses.init_pair(C_ACC, acc, bg)
    curses.init_pair(C_TRACK, 238 if rich else curses.COLOR_BLACK, bg)
    curses.init_pair(C_KEY, curses.COLOR_BLACK, curses.COLOR_WHITE)
    for i, c in enumerate(FREQ_256 if rich else FREQ_8):
        curses.init_pair(P_FREQ + i, c, bg)
    for i, c in enumerate(heat):
        curses.init_pair(P_HEAT + i, c, bg)


def _step(value, steps):
    return sum(value >= s for s in steps)


def freq_attr(frac):
    return curses.color_pair(P_FREQ + _step(frac, FREQ_STEPS))


def heat_attr(frac):
    return curses.color_pair(P_HEAT + _step(frac, HEAT_STEPS))


def temp_attr(celsius):
    return curses.color_pair(P_HEAT + _step(celsius, TEMP_STEPS))


def put(win, y, x, text, attr=0):
    h, w = win.getmaxyx()
    if y < 0 or y >= h or x >= w:
        return
    try:
        win.addnstr(y, x, text, max(0, w - x - 1), attr)
    except curses.error:
        pass


def bar(win, y, x, width, frac, attr):
    """Heavy rule for the value, light for the rest. Box-drawing rules, not
    block elements: some fonts draw U+2588 taller than a line."""
    if width <= 0:
        return
    frac = min(max(frac, 0.0), 1.0)
    full = int(round(frac * width))
    if frac > 0 and full == 0:
        full = 1
    put(win, y, x, "━" * full, attr | curses.A_BOLD)
    track = curses.color_pair(C_TRACK)
    if curses.COLORS < 256:
        track |= curses.A_DIM
    put(win, y, x + full, "─" * (width - full), track)


def box(win, y, x, h, w, title):
    attr = curses.color_pair(C_DIM)
    put(win, y, x, "╭" + "─" * (w - 2) + "╮", attr)
    for i in range(1, h - 1):
        put(win, y + i, x, "│", attr)
        put(win, y + i, x + w - 1, "│", attr)
    put(win, y + h - 1, x, "╰" + "─" * (w - 2) + "╯", attr)
    put(win, y, x + 2, f" {title} ", curses.color_pair(C_ACC) | curses.A_BOLD)


def segments(win, y, x, parts):
    """Draw [(text, attr), ...] one after another."""
    for text, attr in parts:
        put(win, y, x, text, attr)
        x += len(text)


def draw_header(win, st, w):
    title = f" BC-250 Bazzite Metrics Fix {VERSION} "
    put(win, 0, 0, " " * (w - 1), curses.color_pair(C_TITLE))
    put(win, 0, 0, title, curses.color_pair(C_TITLE) | curses.A_BOLD)
    put(win, 0, len(title) + 1, f"kernel {st['kernel']}", curses.color_pair(C_TITLE))
    stamp = time.strftime("%H:%M:%S")
    put(win, 0, w - len(stamp) - 2, stamp, curses.color_pair(C_TITLE))

    dim = curses.color_pair(C_DIM)
    if st["loaded"]:
        module = (f"v{st['version']}", curses.color_pair(C_OK))
    elif st["installed"]:
        module = ("installed, not loaded", curses.color_pair(C_WARN))
    else:
        module = ("not installed", curses.color_pair(C_WARN))
    sclk = "350-2230" if st["sclk"] == "extended" else "1000-2000"
    segments(win, 1, 1, [
        ("amdgpu ", dim),
        ("loaded" if st["amdgpu"] else "NOT loaded",
         curses.color_pair(C_OK if st["amdgpu"] else C_BAD)),
        ("   module ", dim), module,
        (f"   sclk {sclk} MHz   hwmon {st['hwmon']}", dim),
    ])


def draw_cores(win, y, x, w, m):
    """Per-core rows plus one L3 line; returns the box height."""
    rows = len(m["cores"]) + 4
    box(win, y, x, rows, w, f"CPU  {m.get('layout', '?')}")
    if m.get("unpatched"):
        put(win, y, x + 2, " 8 cores, SMU metrics patch off: readings wrong, enable it in BIOS ",
            curses.color_pair(C_BAD) | curses.A_BOLD)
    cx = x + 2
    bw = max(4, (w - 4 - 30) // 3)
    px = cx + 8 + bw + 1            # power column
    tx = px + 7 + bw + 1            # temperature column
    c0x = tx + 7 + bw + 1           # C0 column
    dim = curses.color_pair(C_DIM)
    for col, label in ((cx, "#"), (cx + 3, "MHz"), (px + 1, "power"), (tx + 2, "temp"), (c0x + 1, "C0")):
        put(win, y + 1, col, label, dim)
    for i, (mhz, watt, temp, c0) in enumerate(m["cores"]):
        yy = y + 2 + i
        put(win, yy, cx, f"{i}")
        fa, pa, ta = freq_attr(mhz / CPU_MHZ_MAX), heat_attr(watt / CORE_W_MAX), temp_attr(temp)
        put(win, yy, cx + 2, f"{mhz:5.0f}", fa)
        bar(win, yy, cx + 8, bw, mhz / CPU_MHZ_MAX, fa)
        put(win, yy, px, f"{watt:5.2f}W", pa)
        bar(win, yy, px + 7, bw, watt / CORE_W_MAX, pa)
        put(win, yy, tx, f"{temp:5.1f}°", ta)
        bar(win, yy, tx + 7, bw, temp / TEMP_MAX, ta)
        put(win, yy, c0x, f"{c0:3.0f}%")
    parts = []
    for i, (mhz, temp) in enumerate(m["l3"]):
        parts += [(f"L3 #{i} ", dim), (f"{mhz:.0f} MHz  ", freq_attr(mhz / CPU_MHZ_MAX)),
                  (f"{temp:.1f}°", temp_attr(temp)),
                  ("      ", 0)]
    segments(win, y + 2 + len(m["cores"]), cx, parts)
    return rows


def draw_gpu(win, y, x, w, m, st):
    box(win, y, x, 8, w, "GPU / SoC")
    lx, vx, bx = x + 2, x + 12, x + 22
    bw = w - 24
    gfx_mhz, gfx_t = m.get("gfx", (0, 0))
    soc_mhz, soc_t = m.get("soc", (0, 0))
    put(win, y + 1, lx, "busy")
    ba = heat_attr(m["busy"] / 100)
    put(win, y + 1, vx, f"{m['busy']:5d} %", ba)
    bar(win, y + 1, bx, bw, m["busy"] / 100, ba)
    gfrac = gfx_mhz / max(st["sclk_max"], 1)
    put(win, y + 2, lx, "gfx clock")
    put(win, y + 2, vx, f"{gfx_mhz:5.0f} MHz", freq_attr(gfrac))
    bar(win, y + 2, bx, bw, gfrac, freq_attr(gfrac))
    for i, (label, t) in enumerate((("gfx temp", gfx_t), ("soc temp", soc_t),
                                    ("edge temp", m.get("edge", 0)))):
        put(win, y + 3 + i, lx, label)
        put(win, y + 3 + i, vx, f"{t:5.1f} °C", temp_attr(t))
        bar(win, y + 3 + i, bx, bw, t / TEMP_MAX, temp_attr(t))
    put(win, y + 6, lx, f"socclk {soc_mhz:.0f}  mem {m.get('mem', 0):.0f} MHz", curses.color_pair(C_DIM))


def draw_power(win, y, x, w, m):
    box(win, y, x, 8, w, "Power")
    lx = x + 2
    for i, key in enumerate(("cpu", "gpu")):
        mv, amp, watt = m.get(key, (0, 0, 0))
        put(win, y + 1 + i, lx, f"{key}   {mv:4.0f} mV {amp:6.2f} A {watt:6.2f} W")
    sock = m.get("socket", 0)
    put(win, y + 3, lx, "socket")
    sa = heat_attr(sock / SOCKET_W_MAX)
    put(win, y + 3, lx + 7, f"{sock:6.1f} W", sa | curses.A_BOLD)
    bar(win, y + 3, lx + 16, w - 20, sock / SOCKET_W_MAX, sa)
    th = m.get("throttle", 0)
    put(win, y + 4, lx, "throttle")
    put(win, y + 4, lx + 9, f"0x{th:x}" + ("" if th == 0 else "  THROTTLING"),
        curses.color_pair(C_OK if th == 0 else C_BAD) | (0 if th == 0 else curses.A_BOLD))
    dim = curses.color_pair(C_DIM)
    put(win, y + 5, lx, f"vclk {m.get('vclk', 0):.0f}  dclk {m.get('dclk', 0):.0f} MHz", dim)
    put(win, y + 6, lx, f"{m['state']}, {m['hwmon'] or 'no hwmon'}", dim)


KEYS = [("I", "install"), ("T", "self-test"), ("S", "settings"),
        ("B", "build"), ("U", "uninstall"), ("Q", "quit")]


def draw_footer(win, h, w):
    put(win, h - 1, 0, " " * (w - 1))
    x = 1
    for k, label in KEYS:
        put(win, h - 1, x, f" {k} ", curses.color_pair(C_KEY) | curses.A_BOLD)
        put(win, h - 1, x + 3, f" {label}")
        x += len(label) + 6


def draw_not_loaded(win, y, w, st):
    lines = []
    if not st["amdgpu"]:
        lines.append(("amdgpu is not loaded, so there is nothing to read.", C_BAD))
        lines.append(("If an earlier install left rd.driver.blacklist=amdgpu, press I:", C_DIM))
        lines.append(("the installer removes it and asks for one reboot.", C_DIM))
    elif not st["installed"]:
        lines.append((f"{MOD} is not installed.", C_WARN))
        lines.append(("Press I to build it for your kernels and load it (about 10 seconds).", C_DIM))
    elif not st["built_running"]:
        lines.append((f"Nothing is built for the running kernel {st['kernel']}.", C_WARN))
        lines.append(("This happens after a kernel update. Press I to rebuild.", C_DIM))
    else:
        lines.append((f"{MOD} is installed but not loaded.", C_WARN))
        lines.append(("Press I to reinstall and load it, or see: journalctl -k -g bc250", C_DIM))
    box(win, y, 1, len(lines) + 2, w - 2, "Status")
    for i, (text, color) in enumerate(lines):
        put(win, y + 1 + i, 3, text, curses.color_pair(color))


MIN_W, MIN_H = 80, 24


def draw(win):
    win.erase()
    h, w = win.getmaxyx()
    if h < MIN_H or w < MIN_W:
        put(win, 0, 0, f"Terminal too small ({w}x{h}); need at least {MIN_W}x{MIN_H}.",
            curses.color_pair(C_WARN))
        put(win, 1, 0, "Resize, or press Q to quit.")
        win.refresh()
        return
    st = system_state()
    draw_header(win, st, w)
    m = metrics() if st["loaded"] else None
    if not m or not m["cores"]:
        draw_not_loaded(win, 3, w, st)
    else:
        rows = draw_cores(win, 2, 1, w - 2, m)
        y = 2 + rows
        half = (w - 3) // 2
        draw_gpu(win, y, 1, half, m, st)
        draw_power(win, y, 2 + half, w - 3 - half, m)
    draw_footer(win, h, w)
    win.refresh()


# ---------------------------------------------------------------------------
# Dialogs and actions


def dialog(win, title, lines, options):
    """Centered modal; returns the index of the chosen option or None."""
    h, w = win.getmaxyx()
    bw = min(w - 4, max(len(title) + 8, max((len(l) for l in lines), default=0) + 6,
                        sum(len(o) + 4 for o in options) + 4))
    bh = len(lines) + 5
    y, x = (h - bh) // 2, (w - bw) // 2
    sel = 0
    win.timeout(-1)
    try:
        while True:
            for i in range(bh):
                put(win, y + i, x, " " * bw)
            box(win, y, x, bh, bw, title)
            for i, l in enumerate(lines):
                put(win, y + 2 + i, x + 3, l)
            ox = x + 3
            for i, o in enumerate(options):
                attr = curses.color_pair(C_TITLE) | curses.A_BOLD if i == sel else 0
                put(win, y + bh - 2, ox, f" {o} ", attr)
                ox += len(o) + 4
            win.refresh()
            k = win.getch()
            if k in (curses.KEY_LEFT, curses.KEY_UP, ord("h"), ord("k")):
                sel = (sel - 1) % len(options)
            elif k in (curses.KEY_RIGHT, curses.KEY_DOWN, ord("\t"), ord("l"), ord("j")):
                sel = (sel + 1) % len(options)
            elif k in (curses.KEY_ENTER, 10, 13, ord(" ")):
                return sel
            elif k in (27, ord("q"), ord("Q")):
                return None
            elif ord("1") <= k < ord("1") + len(options):
                return k - ord("1")
    finally:
        win.timeout(REFRESH_MS)


def run_in_terminal(win, args, root=False):
    """Leave curses, run the script with visible output, wait for a key."""
    curses.def_prog_mode()
    curses.endwin()
    cmd = [SCRIPT] + args
    if root and os.geteuid() != 0:
        cmd = ["sudo"] + cmd
    print()
    try:
        rc = subprocess.call(cmd)
    except KeyboardInterrupt:
        rc = 130
    print()
    print(f"\033[2m[exit {rc}] Press Enter to return to the dashboard\033[0m", end="", flush=True)
    try:
        input()
    except (EOFError, KeyboardInterrupt):
        pass
    curses.reset_prog_mode()
    win.clear()
    return rc


def action_settings(win):
    sclk, hwmon = setting("sclk"), setting("hwmon")
    i = dialog(win, "Settings", [
        f"GFX clock range accepted via pp_od_clk_voltage:  {sclk}",
        "  extended = 350-2230 MHz, stock = 1000-2000 MHz",
        f"bc250 hwmon device:  {hwmon}",
        "  off = not registered (default), on = every reading in sensors",
    ], ["GFX clock range", "hwmon", "Cancel"])
    if i == 0:
        j = dialog(win, "GFX clock range", ["Range accepted through pp_od_clk_voltage"],
                   ["extended 350-2230", "stock 1000-2000", "Cancel"])
        if j in (0, 1):
            run_in_terminal(win, ["--set", "sclk=" + ("extended", "stock")[j]], root=True)
    elif i == 1:
        j = dialog(win, "bc250 hwmon device", [
            f"off: no hwmon device; the dashboard reads {TELEMETRY} (default)",
            "on:  every reading as a sensor, for sensors, KDE System Monitor,",
            "     CoolerControl",
            "amdgpu's own hwmon, gpu_metrics and amdgpu_top are fixed either way.",
        ], ["off", "on", "Cancel"])
        if j in (0, 1):
            run_in_terminal(win, ["--set", "hwmon=" + ("off", "on")[j]], root=True)


def main(win):
    curses.curs_set(0)
    init_colors()
    win.keypad(True)
    win.timeout(REFRESH_MS)
    while True:
        draw(win)
        k = win.getch()
        if k in (ord("q"), ord("Q"), 27):
            return
        if k in (ord("i"), ord("I")):
            run_in_terminal(win, ["--install"], root=True)
        elif k in (ord("t"), ord("T")):
            run_in_terminal(win, ["--selftest"])
        elif k in (ord("s"), ord("S")):
            action_settings(win)
        elif k in (ord("b"), ord("B")):
            run_in_terminal(win, ["--build"])
        elif k in (ord("u"), ord("U")):
            if dialog(win, "Uninstall", [f"Unload {MOD} and remove all of its files?",
                                         "amdgpu is not modified; no reboot is needed."],
                      ["Uninstall", "Cancel"]) == 0:
                run_in_terminal(win, ["--uninstall"], root=True)


if __name__ == "__main__":
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        sys.exit(f"{NAME}: the dashboard needs a terminal; see {SCRIPT} --help")
    os.environ.setdefault("ESCDELAY", "25")
    try:
        curses.wrapper(main)
    except KeyboardInterrupt:
        pass

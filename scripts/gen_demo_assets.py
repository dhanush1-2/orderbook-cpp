#!/usr/bin/env python3
"""Run the real demos and render their output as README images.

Nothing here is mocked: each image is the captured output of a command run by this
script. Rendering uses Pillow because the TUI needs a pty and no recorder is assumed.

    ./scripts/gen_demo_assets.py        # after building with OB_BUILD_BENCH and OB_BUILD_TOOLS
"""
import fcntl, json, os, pty, re, select, signal, struct, subprocess, tempfile, termios, time
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT = Path("docs/images")
BG, FG, DIM, PROMPT = (22, 24, 29), (222, 226, 232), (140, 148, 160), (120, 200, 255)
RED, GREEN = (240, 113, 120), (127, 217, 98)
FONT = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 26)
CW, LH, PAD = FONT.getlength("M"), 34, 28
SGR = re.compile(r"\x1b\[(\d*)m")


def spans(line):
    """Split a line with SGR colors into (text, color) pairs."""
    color, pos, out = FG, 0, []
    for m in SGR.finditer(line):
        out.append((line[pos:m.start()], color))
        color = {"31": RED, "32": GREEN}.get(m.group(1), FG)
        pos = m.end()
    out.append((line[pos:], color))
    return out


def width_of(line):
    return len(SGR.sub("", line))


def render(lines, cols=None, rows=None):
    cols = cols or max(width_of(l) for l in lines)
    rows = rows or len(lines)
    img = Image.new("RGB", (int(cols * CW) + 2 * PAD, rows * LH + 2 * PAD + 36), BG)
    d = ImageDraw.Draw(img)
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse((PAD + i * 26, 16, PAD + i * 26 + 14, 30), fill=c)
    for y, line in enumerate(lines):
        x = PAD
        base = PROMPT if line.startswith("$ ") else None
        for text, color in spans(line):
            d.text((x, PAD + 36 + y * LH), text, font=FONT, fill=base or color)
            x += len(text) * CW
    return img


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def shot(name, shown, lines):
    render([f"$ {shown}", *lines]).save(OUT / name)
    print("wrote", OUT / name)


def tests():
    out = run(["ctest", "--test-dir", "build"]).splitlines()
    picks = [l for l in out if "Test #" in l]
    tail = [l for l in out if "tests passed" in l or "Total Test time" in l]
    short = [re.sub(r"\.{3,}\s*", " .. ", l.strip()) for l in picks[:4] + ["..."] + picks[-4:]]
    shot("demo-tests.png", "ctest --test-dir build", short + [""] + tail)


def throughput():
    out = run(["build/bench/ob_bench_throughput", "--ops", "2000000"])
    rows = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
    table = [f"{'scenario':<20}{'ns/op':>8}{'M ops/s':>10}{'allocations':>13}"]
    for r in rows:
        table.append(f"{r['scenario']:<20}{r['ns_per_op']:>8.2f}"
                     f"{r['ops_per_sec'] / 1e6:>10.1f}{r['allocations']:>13}")
    shot("demo-throughput.png", "ob_bench_throughput --ops 2000000  # JSON, tabulated", table)


def wire():
    with tempfile.TemporaryDirectory() as t:
        f = f"{t}/orders.bin"
        run(["build/tools/ob_wire_gen", "--scenario", "mixed_realistic", "--ops", "200000", "--out", f])
        out = run(["build/tools/ob_wire_ingest", "--in", f, "--buffer", "1000"]).splitlines()
    shot("demo-wire.png", "ob_wire_ingest --in orders.bin --buffer 1000", out)


def tui():
    pid, fd = pty.fork()
    if pid == 0:
        os.execv("build/tools/ob_replay", ["ob_replay", "--scenario", "mixed_realistic",
                                           "--ops", "200000", "--rate", "5000", "--tui"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 90, 0, 0))
    buf, start = b"", time.time()
    while time.time() - start < 8:
        if select.select([fd], [], [], 0.2)[0]:
            try:
                buf += os.read(fd, 65536)
            except OSError:
                break
    os.kill(pid, signal.SIGINT)
    os.waitpid(pid, 0)

    frames = []
    for raw in buf.decode(errors="replace").split("\x1b[H")[1:]:
        text = re.sub(r"\x1b\[(K|J|\?\d+[hl])", "", raw).replace("\r", "")
        lines = text.rstrip("\n").split("\n")
        if "ctrl-c" in lines[-1]:
            frames.append(lines)
    frames = frames[len(frames) // 4:]  # let the book fill first
    cols = max(width_of(l) for f in frames for l in f)
    rows = max(len(f) for f in frames)
    step = max(1, len(frames) // 40)
    images = [render(f, cols, rows) for f in frames[::step]]
    images[-1].save(OUT / "demo-tui.png")
    small = [i.resize((i.width * 2 // 3, i.height * 2 // 3)).convert("P", palette=Image.ADAPTIVE)
             for i in images]
    small[0].save(OUT / "demo-tui.gif", save_all=True, append_images=small[1:],
                  duration=150, loop=0, optimize=True)
    print("wrote TUI images from", len(frames), "captured frames")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    tests(); throughput(); wire(); tui()

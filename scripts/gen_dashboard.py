#!/usr/bin/env python3
"""Generate docs/dashboard.html from measured results in bench/results/.

Generated, never hand-edited, so the dashboard cannot drift from the runs that
produced it. Re-run after every benchmark run.

Palette: dataviz reference slots 1-2 (blue/orange), validated in both light and
dark modes - all six checks PASS.
"""
import json
import pathlib
import platform
import re
import subprocess
import sys

ORDER = ["rest_only", "cross_shallow", "cross_deep",
         "cancel_heavy", "mixed_realistic", "worst_case_sweep"]
HEADLINE = "mixed_realistic"


def read(name):
    p = pathlib.Path("bench/results") / name
    if not p.exists():
        sys.exit(f"missing {p}; run the benchmarks first")
    return {json.loads(l)["scenario"]: json.loads(l)
            for l in p.read_text().splitlines() if l.strip()}


def bar(frac, series, label, title):
    """Horizontal bar. 4px rounded data-end anchored to the baseline; the track
    provides the 2px surface gap between adjacent bars."""
    pct = max(0.4, min(100.0, frac * 100.0))
    return (f'<div class="track" title="{title}">'
            f'<div class="fill s{series}" style="width:{pct:.2f}%"></div>'
            f'<span class="vlabel">{label}</span></div>')


def main():
    lat, thr, ref = read("latency.jsonl"), read("throughput.jsonl"), read("reference_throughput.jsonl")
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    # Parse the authoritative summary line. Counting "Test  #" undercounts badly:
    # ctest pads the number, so three-digit tests get ONE space and 209 reads as 90.
    ctest_out = subprocess.run(["ctest", "--test-dir", "build", "-N"],
                               capture_output=True, text=True).stdout
    m = re.search(r"Total Tests:\s*(\d+)", ctest_out)
    tests = int(m.group(1)) if m else 0

    rows = []
    for k in ORDER:
        t, l = thr[k], lat[k]
        rows.append({
            "name": k, "nspo": t["ns_per_op"], "ops": t["ops_per_sec"],
            "p99": l["service_ns"]["p99"], "p999": l["service_ns"]["p99_9"],
            "speedup": t["ops_per_sec"] / ref[k]["ops_per_sec"],
            "subres": l["frac_below_clock_resolution"] * 100.0,
            "allocs": t["allocations"],
        })
    by = {r["name"]: r for r in rows}
    clock = lat[HEADLINE]["clock_resolution_ns"]
    peak = max(rows, key=lambda r: r["ops"])

    max_ns = max(r["nspo"] for r in rows)
    # SEPARATE scales. p99 spans 116-491 ns while p99.9 reaches 5,200; sharing one
    # axis flattens five of six scenarios into stubs. The method's rule is explicit:
    # two measures of different scale get two charts, never one squashed one.
    max_p99 = max(r["p99"] for r in rows)
    max_p999 = max(r["p999"] for r in rows)
    # The headline scenario's speedup is an asymptotic outlier (see the callout);
    # scaling the axis to it would flatten the other five into nothing.
    comparable = [r for r in rows if r["name"] != HEADLINE]
    max_speed = max(r["speedup"] for r in comparable)

    def nm(s):
        return s.replace("_", " ")

    ns_bars = "".join(
        f'<div class="row"><span class="rl">{nm(r["name"])}</span>'
        + bar(r["nspo"] / max_ns, 1, f'{r["nspo"]:.1f} ns',
              f'{nm(r["name"])}: {r["nspo"]:.1f} ns per operation, '
              f'{r["ops"]:,.0f} ops/sec') + "</div>"
        for r in rows)

    p99_bars = "".join(
        f'<div class="row"><span class="rl">{nm(r["name"])}</span>'
        + bar(r["p99"] / max_p99, 1, f'{r["p99"]:.0f} ns',
              f'{nm(r["name"])} p99: {r["p99"]:.0f} ns') + "</div>"
        for r in rows)
    p999_bars = "".join(
        f'<div class="row"><span class="rl">{nm(r["name"])}</span>'
        + bar(r["p999"] / max_p999, 2, f'{r["p999"]:,.0f} ns',
              f'{nm(r["name"])} p99.9: {r["p999"]:,.0f} ns') + "</div>"
        for r in rows)

    sp_bars = "".join(
        f'<div class="row"><span class="rl">{nm(r["name"])}</span>'
        + bar(r["speedup"] / max_speed, 1, f'{r["speedup"]:.1f}x',
              f'{nm(r["name"])}: {r["speedup"]:.1f}x the reference engine') + "</div>"
        for r in comparable)

    table = "".join(
        f"<tr><td>{nm(r['name'])}</td><td>{r['nspo']:.1f}</td><td>{r['ops']:,.0f}</td>"
        f"<td>{r['p99']:.0f}</td><td>{r['p999']:.0f}</td><td>{r['speedup']:.1f}x</td>"
        f"<td>{r['subres']:.0f}%</td><td>{r['allocs']}</td></tr>"
        for r in rows)

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Matching Engine Benchmarks</title>
<style>
  :root {{
    color-scheme: light;
    --surface-0: #f4f4f2; --surface-1: #fcfcfb; --border: #dedcd6;
    --text-primary: #0b0b0b; --text-secondary: #52514e; --text-muted: #77756e;
    --series-1: #2a78d6; --series-2: #eb6834; --track: #e8e7e2;
  }}
  @media (prefers-color-scheme: dark) {{
    :root:not([data-theme="light"]) {{
      color-scheme: dark;
      --surface-0: #111110; --surface-1: #1a1a19; --border: #35342f;
      --text-primary: #ffffff; --text-secondary: #c3c2b7; --text-muted: #8f8d83;
      --series-1: #3987e5; --series-2: #d95926; --track: #26251f;
    }}
  }}
  :root[data-theme="dark"] {{
    color-scheme: dark;
    --surface-0: #111110; --surface-1: #1a1a19; --border: #35342f;
    --text-primary: #ffffff; --text-secondary: #c3c2b7; --text-muted: #8f8d83;
    --series-1: #3987e5; --series-2: #d95926; --track: #26251f;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; padding: 28px 16px 40px; background: var(--surface-0);
    color: var(--text-primary);
    font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Inter, sans-serif;
  }}
  .wrap {{ max-width: 1120px; margin: 0 auto; }}
  h1 {{ font-size: 21px; margin: 0 0 4px; letter-spacing: -0.01em; }}
  .sub {{ color: var(--text-secondary); font-size: 13px; margin: 0 0 22px; }}
  .sub code {{ font-size: 12px; color: var(--text-muted); }}
  .tiles {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
            gap: 12px; margin-bottom: 22px; }}
  .tile {{ background: var(--surface-1); border: 1px solid var(--border);
           border-radius: 10px; padding: 14px 16px; }}
  .tile .k {{ font-size: 11px; text-transform: uppercase; letter-spacing: .07em;
              color: var(--text-muted); margin-bottom: 7px; }}
  .tile .v {{ font-size: 26px; font-weight: 600; letter-spacing: -0.02em;
              font-variant-numeric: tabular-nums; }}
  .tile .u {{ font-size: 12px; color: var(--text-secondary); margin-left: 3px;
              font-weight: 400; }}
  .grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }}
  @media (max-width: 760px) {{ .grid {{ grid-template-columns: 1fr; }} }}
  .card {{ background: var(--surface-1); border: 1px solid var(--border);
           border-radius: 10px; padding: 16px 18px 18px; }}
  .card.span {{ grid-column: 1 / -1; }}
  .card h2 {{ font-size: 13px; margin: 0 0 2px; letter-spacing: -0.005em; }}
  .card .note {{ font-size: 12px; color: var(--text-secondary); margin: 0 0 14px; }}
  .legend {{ display: flex; gap: 14px; margin: 0 0 12px; font-size: 12px;
             color: var(--text-secondary); }}
  .legend i {{ display: inline-block; width: 9px; height: 9px; border-radius: 2px;
               margin-right: 5px; vertical-align: baseline; }}
  .row, .row2 {{ display: grid; grid-template-columns: 116px 1fr; gap: 10px;
                 align-items: center; margin-bottom: 7px; }}
  .rl {{ font-size: 12px; color: var(--text-secondary); text-align: right;
         white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }}
  .pair {{ display: grid; gap: 2px; }}  /* 2px surface gap between adjacent bars */
  .track {{ position: relative; background: var(--track); border-radius: 4px;
            height: 15px; }}
  .fill {{ height: 100%; border-radius: 0 4px 4px 0; }}  /* rounded data-end */
  .fill.s1 {{ background: var(--series-1); }}
  .fill.s2 {{ background: var(--series-2); }}
  .vlabel {{ position: absolute; left: calc(100% + 7px); top: 0; line-height: 15px;
             font-size: 11px; color: var(--text-secondary);
             font-variant-numeric: tabular-nums; white-space: nowrap; }}
  .callout {{ border-left: 2px solid var(--series-2); padding: 9px 0 9px 12px;
              margin-top: 14px; font-size: 12px; color: var(--text-secondary); }}
  .callout b {{ color: var(--text-primary); font-weight: 600; }}
  table {{ width: 100%; border-collapse: collapse; font-size: 12px;
           font-variant-numeric: tabular-nums; }}
  th, td {{ text-align: right; padding: 5px 8px;
            border-bottom: 1px solid var(--border); }}
  th:first-child, td:first-child {{ text-align: left; }}
  th {{ color: var(--text-muted); font-weight: 500; font-size: 11px;
        text-transform: uppercase; letter-spacing: .05em; }}
  .foot {{ margin-top: 20px; font-size: 12px; color: var(--text-muted); }}
</style>
</head>
<body>
<div class="wrap">
  <h1>Order book &amp; matching engine &mdash; measured</h1>
  <p class="sub">{platform.processor() or platform.machine()} &middot; Apple clang, C++20, <code>-O3</code>
     &middot; commit <code>{commit}</code> &middot; generated by <code>scripts/gen_dashboard.py</code></p>

  <div class="tiles">
    <div class="tile"><div class="k">Headline &mdash; mixed realistic</div>
      <div class="v">{by[HEADLINE]['nspo']:.1f}<span class="u">ns / op</span></div></div>
    <div class="tile"><div class="k">Peak throughput &mdash; {nm(peak['name'])}</div>
      <div class="v">{peak['ops']/1e6:.1f}<span class="u">M ops / s</span></div></div>
    <div class="tile"><div class="k">Hot-path allocations</div>
      <div class="v">0<span class="u">asserted</span></div></div>
    <div class="tile"><div class="k">Tests passing</div>
      <div class="v">{tests}<span class="u">+ 10<sup>7</sup> differential</span></div></div>
  </div>

  <div class="grid">
    <div class="card">
      <h2>Per-operation cost</h2>
      <p class="note">Batched timing, no per-operation timestamps &mdash; quantization-free.
         This is the figure to trust.</p>
      {ns_bars}
    </div>

    <div class="card">
      <h2>Speedup over the reference engine</h2>
      <p class="note">Same harness, same workload. <code>std::map</code> + <code>std::list</code> baseline.</p>
      {sp_bars}
      <div class="callout"><b>mixed realistic reaches {by[HEADLINE]['speedup']:.0f}x</b> and is left off this
        chart on purpose. It is the only scenario with meaningful FOK volume, where the
        two engines differ <i>asymptotically</i>: the reference sums individual orders
        (O(orders)), the fast engine sums level totals (O(levels)). Plotting it would
        flatten the other five and credit the flat ladder for an algorithmic win.</div>
    </div>

    <div class="card">
      <h2>Latency &mdash; p99</h2>
      <p class="note">Service time per operation. Own scale: p99 spans {max_p99:.0f} ns.</p>
      {p99_bars}
    </div>

    <div class="card">
      <h2>Latency &mdash; p99.9</h2>
      <p class="note">Own scale, {max_p999:,.0f} ns. Separate chart on purpose &mdash; sharing an
         axis with p99 would flatten five of six into stubs.</p>
      {p999_bars}
    </div>

    <div class="card span">
      <div class="callout">The clock's own resolution is <b>{clock:.0f} ns</b> and the median
        operation is faster than that, so {min(r['subres'] for r in rows):.0f}&ndash;{max(r['subres'] for r in rows):.0f}% of samples fall below the
        floor. <b>Every p50 is quantization, not signal</b>, and is reported as such rather
        than printed as a number. These percentiles show the shape of the tail; the
        per-operation panel is the cost. <b>worst case sweep</b> reaching {by['worst_case_sweep']['p999']:,.0f} ns at
        p99.9 is the 400-level book sweep doing exactly what it is built to do.</div>
    </div>

    <div class="card span">
      <h2>All measurements</h2>
      <p class="note">The table view: every figure above, plus what is not plotted.</p>
      <table>
        <thead><tr><th>scenario</th><th>ns/op</th><th>ops/sec</th><th>p99</th>
          <th>p99.9</th><th>vs ref</th><th>sub-resolution</th><th>allocs</th></tr></thead>
        <tbody>{table}</tbody>
      </table>
    </div>
  </div>

  <p class="foot">Method, caveats and the optimization log (including the candidate that was
     measured and rejected) are in <code>docs/METHODOLOGY.md</code>,
     <code>docs/BENCHMARKS.md</code> and <code>docs/OPTIMIZATION-LOG.md</code>.</p>
</div>
</body>
</html>
"""
    out = pathlib.Path("docs/dashboard.html")
    out.write_text(html)
    print(f"wrote {out} ({len(html):,} bytes) from {len(rows)} measured scenarios")

    if "--png" in sys.argv:
        render_png(out)


def render_png(html_path):
    """Render the dashboard to docs/images/dashboard.png via headless Chrome.

    The height is MEASURED, not guessed: render tall, scan up for the last
    non-background row, then re-render at exactly that height plus padding. A
    guessed height leaves dead space or clips the table.
    """
    chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if not pathlib.Path(chrome).exists():
        print("skipping PNG: Google Chrome not found", file=sys.stderr)
        return
    try:
        from PIL import Image
    except ImportError:
        print("skipping PNG: Pillow not installed (pip install Pillow)", file=sys.stderr)
        return

    url = "file://" + str(html_path.resolve()).replace(" ", "%20")
    png = pathlib.Path("docs/images/dashboard.png")
    png.parent.mkdir(parents=True, exist_ok=True)

    def shot(height, path, scale):
        subprocess.run([chrome, "--headless=new", "--disable-gpu", "--no-sandbox",
                        "--hide-scrollbars", "--virtual-time-budget=3000",
                        f"--window-size=1200,{height}",
                        f"--force-device-scale-factor={scale}",
                        f"--screenshot={path}", url], capture_output=True)

    probe = "/tmp/ob_dashboard_probe.png"
    shot(2400, probe, 1)
    im = Image.open(probe).convert("RGB")
    w, h = im.size
    bg, px, last = im.getpixel((5, h - 5)), im.load(), 0
    for y in range(h - 1, -1, -1):
        if any(px[x, y] != bg for x in range(0, w, 7)):
            last = y
            break
    shot(last + 28, png, 2)
    final = Image.open(png)
    print(f"wrote {png} {final.size[0]}x{final.size[1]} "
          f"({final.size[0] // 2}x{final.size[1] // 2} logical, 2x retina)")


if __name__ == "__main__":
    sys.exit(main())

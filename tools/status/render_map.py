#!/usr/bin/env python3
"""Makes the project map from tools/status/project_map.json.

One file holds the map; everything that shows it is made from that file, so
they always agree:

- docs/status/project-map.svg: the diagram, as GitHub shows it in the README;
- the part of README.md between the project-map markers: the last change,
  that diagram, the table of parts and what comes next;
- with --html PATH, the page published as the project map artifact.

    python3 tools/status/render_map.py            # Write the SVG and README part.
    python3 tools/status/render_map.py --check    # Fail if they are not up to date.
    python3 tools/status/render_map.py --html map.html

Only the standard library is used.
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "tools/status/project_map.json"
SVG = ROOT / "docs/status/project-map.svg"
README = ROOT / "README.md"
SVG_IN_README = "docs/status/project-map.svg"
START = ("<!-- project-map:start: made by tools/status/render_map.py from "
         "tools/status/project_map.json; change that file, not this part -->")
END = "<!-- project-map:end -->"

# Light and dark colours of each status, the same on GitHub and on the page.
COLOURS = {
    "light": {"bg": "#f7f4ee", "surface": "#fffdf8", "fg": "#2a2620", "muted": "#6d6458",
              "line": "#ddd4c4", "accent": "#9a6b1f",
              "done": ("#2f7d4f", "#e2f1e6"), "part": ("#9a6b00", "#fbefcf"),
              "block": ("#b2452f", "#f8e1da"), "none": ("#6b6b74", "#ebeaee")},
    "dark": {"bg": "#17150f", "surface": "#201d16", "fg": "#ece5d8", "muted": "#a89e8e",
             "line": "#3a3428", "accent": "#d9a54a",
             "done": ("#7fd19c", "#1d3325"), "part": ("#e8c25e", "#3a3016"),
             "block": ("#f08f78", "#3d2119"), "none": ("#b3b3bd", "#2b2a30")},
}
STATUSES = ("done", "part", "block", "none")
MARKS = {"done": "🟢", "part": "🟡", "block": "🔴", "none": "⚪"}
LEGEND_HEIGHT = 44  # Above the diagram, in the SVG for GitHub.


class MapError(Exception):
    pass


def load(path: Path = DATA) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    for key in ("title", "artifact", "last_change", "statuses", "diagram", "parts", "next"):
        if key not in data:
            raise MapError(f"{path.name}: '{key}' is missing")
    for status in STATUSES:
        if status not in data["statuses"]:
            raise MapError(f"{path.name}: the status '{status}' has no name")
    things = [("box " + b["label"], b["status"]) for b in data["diagram"]["boxes"]]
    things += [("part " + p["part"], p["status"]) for p in data["parts"]]
    for name, status in things:
        if status not in STATUSES:
            raise MapError(f"{path.name}: {name} has the status '{status}', not one of {', '.join(STATUSES)}")
    dt.date.fromisoformat(data["last_change"]["date"])
    return data


def long_date(iso: str) -> str:
    day = dt.date.fromisoformat(iso)
    return f"{day.day} {day.strftime('%B')} {day.year}"


def e(text: str) -> str:
    return html.escape(text, quote=True)


def svg_style(c: dict) -> str:
    rules = [
        f".surface{{fill:{c['surface']};stroke:{c['line']}}}",
        f"text{{fill:{c['fg']}}}",
        f".sub{{fill:{c['muted']}}}",
        f".group rect{{stroke:{c['line']}}}",
        f".group text{{fill:{c['accent']}}}",
        f".edge{{stroke:{c['muted']}}}",
        f".arrow{{fill:{c['muted']}}}",
        f".edge.stop{{stroke:{c['block'][0]}}}",
    ]
    for status in STATUSES:
        line, fill = c[status]
        rules.append(f".{status} rect{{fill:{fill};stroke:{line}}}")
        rules.append(f".{status} circle{{fill:{line}}}")
    return "".join(rules)


def diagram_body(data: dict) -> str:
    """The groups, boxes and lines."""
    d = data["diagram"]
    out = []
    for g in d["groups"]:
        out.append(f'<g class="group"><rect x="{g["x"]}" y="{g["y"]}" width="{g["w"]}" '
                   f'height="{g["h"]}" rx="10"></rect><text x="{g["x"] + 16}" y="{g["y"] + 22}">'
                   f'{e(g["label"])}</text></g>')
    for b in d["boxes"]:
        texts = [f'<text x="{b["x"] + 15}" y="{b["label_y"]}">{e(b["label"])}</text>']
        texts += [f'<text class="sub" x="{b["x"] + 15}" y="{line["y"]}">{e(line["text"])}</text>'
                  for line in b["lines"]]
        out.append(f'<g class="box {b["status"]}"><rect x="{b["x"]}" y="{b["y"]}" '
                   f'width="{b["w"]}" height="{b["h"]}" rx="8"></rect>{"".join(texts)}</g>')
    for edge in d["edges"]:
        out.append(f'<path class="edge{" stop" if edge.get("stop") else ""}" d="{edge["d"]}"></path>')
    return "\n".join(out)



def github_svg(data: dict) -> str:
    d = data["diagram"]
    width, height = d["width"], d["height"] + LEGEND_HEIGHT
    legend, x = [], 12
    for status in STATUSES:
        name = data["statuses"][status]
        legend.append(f'<g class="{status}"><circle cx="{x + 6}" cy="22" r="6"></circle>'
                      f'<text x="{x + 18}" y="27">{e(name)}</text></g>')
        x += 18 + int(len(name) * 7.6) + 26
    title = f"{data['title']}: each part and its status"
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
        f'height="{height}" role="img" aria-label="{e(title)}">\n'
        f"<title>{e(title)}</title>\n"
        "<style>"
        "text{font:13px -apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif}"
        ".sub{font-size:11px}.group text{font-weight:600;font-size:12px;letter-spacing:.06em;text-transform:uppercase}"
        ".group rect{fill:none;stroke-dasharray:5 4}.box rect{stroke-width:1.5}"
        ".edge{stroke-width:1.4;fill:none;marker-end:url(#arrow)}.edge.stop{stroke-dasharray:4 3}"
        + svg_style(COLOURS["light"])
        + "@media (prefers-color-scheme:dark){" + svg_style(COLOURS["dark"]) + "}"
        "</style>\n"
        '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
        'orient="auto-start-reverse"><path class="arrow" d="M0,0 L10,5 L0,10 z"></path></marker></defs>\n'
        f'<rect class="surface" x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="10"></rect>\n'
        + "\n".join(legend) + "\n"
        + f'<g transform="translate(0 {LEGEND_HEIGHT})">\n' + diagram_body(data) + "\n</g>\n</svg>\n"
    )


def md_cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def readme_part(data: dict) -> str:
    statuses = data["statuses"]
    rows = [f"| `{md_cell(p['part'])}` | {MARKS[p['status']]} {statuses[p['status']]} | {md_cell(p['now'])} |"
            for p in data["parts"]]
    steps = [f"{n}. {step}" for n, step in enumerate(data["next"], 1)]
    change = data["last_change"]
    return "\n".join([
        START,
        f"**Last change** ({long_date(change['date'])}): {change['what']}",
        "",
        f"![{data['title']}: each part and its status]({SVG_IN_README})",
        "",
        "| Part | Status | Now |",
        "| --- | --- | --- |",
        *rows,
        "",
        "**Next:**",
        "",
        *steps,
        "",
        f"The same, as a page of its own: [the project map]({data['artifact']}).",
        END,
    ])


def with_readme_part(readme: str, part: str) -> str:
    start, end = readme.find("<!-- project-map:start"), readme.find(END)
    if start < 0 or end < start:
        raise MapError("README.md has no project-map markers")
    return readme[:start] + part + readme[end + len(END):]


def page(data: dict) -> str:
    light, dark = COLOURS["light"], COLOURS["dark"]

    def variables(c: dict) -> str:
        names = [f"--bg: {c['bg']}; --surface: {c['surface']}; --fg: {c['fg']}; --muted: {c['muted']}; "
                 f"--line: {c['line']}; --accent: {c['accent']};"]
        for status in STATUSES:
            names.append(f"--{status}: {c[status][0]}; --{status}-bg: {c[status][1]};")
        return " ".join(names)

    legend = "\n".join(f'      <span class="pill {s}">{e(data["statuses"][s])}</span>' for s in STATUSES)
    rows = "\n".join(
        f'          <tr><td>{e(p["part"])}</td><td><span class="pill {p["status"]}">'
        f'{e(short(data["statuses"][p["status"]]))}</span></td><td>{e(p["now"])}</td></tr>'
        for p in data["parts"])
    steps = "\n".join(f"      <li>{e(step)}</li>" for step in data["next"])
    d = data["diagram"]
    change = data["last_change"]
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{e(data["title"])}</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Fraunces:opsz,wght@9..144,600&family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
  /* Made by tools/status/render_map.py from tools/status/project_map.json. */
  :root {{ {variables(light)}
    --display: "Fraunces", Georgia, serif; --body: "IBM Plex Sans", system-ui, sans-serif;
    --mono: "IBM Plex Mono", ui-monospace, monospace; color-scheme: light; }}
  @media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ {variables(dark)} color-scheme: dark; }} }}
  :root[data-theme="dark"] {{ {variables(dark)} color-scheme: dark; }}
  body {{ margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.55 var(--body);
    padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); padding-block: 28px 48px; }}
  main {{ max-width: 1060px; margin: 0 auto; display: grid; gap: 28px; }}
  header {{ display: grid; gap: 6px; }}
  h1 {{ font: 600 2.1rem/1.15 var(--display); margin: 0; text-wrap: balance; }}
  h2 {{ font: 600 1.3rem/1.2 var(--display); margin: 0 0 10px; }}
  a {{ color: var(--accent); }}
  .meta {{ color: var(--muted); font-size: .9rem; }}
  .meta b {{ color: var(--fg); font-weight: 600; }}
  .legend {{ display: flex; flex-wrap: wrap; gap: 8px 14px; font-size: .85rem; }}
  .pill {{ display: inline-flex; align-items: center; gap: 6px; padding: 1px 9px; border-radius: 999px; font-size: .78rem;
    font-weight: 600; white-space: nowrap; border: 1px solid currentColor; }}
  .pill::before {{ content: ""; width: 7px; height: 7px; border-radius: 50%; background: currentColor; }}
  .pill.done {{ color: var(--done); background: var(--done-bg); }}
  .pill.part {{ color: var(--part); background: var(--part-bg); }}
  .pill.block {{ color: var(--block); background: var(--block-bg); }}
  .pill.none {{ color: var(--none); background: var(--none-bg); }}
  .map {{ overflow-x: auto; background: var(--surface); border: 1px solid var(--line); border-radius: 10px; padding: 18px 12px; }}
  .map svg {{ display: block; min-width: 860px; width: 100%; height: auto; font-family: var(--body); }}
  .map .box rect {{ stroke-width: 1.5; }}
  .map text {{ fill: var(--fg); font-size: 13px; }}
  .map .sub {{ fill: var(--muted); font-size: 11px; }}
  .map .group rect {{ fill: none; stroke: var(--line); stroke-dasharray: 5 4; }}
  .map .group text {{ fill: var(--accent); font: 600 12px var(--body); letter-spacing: .06em; text-transform: uppercase; }}
  .map .edge {{ stroke: var(--muted); stroke-width: 1.4; fill: none; marker-end: url(#arrow); }}
  .map .edge.stop {{ stroke: var(--block); stroke-dasharray: 4 3; }}
  .map .arrow {{ fill: var(--muted); }}
  .map .done rect {{ fill: var(--done-bg); stroke: var(--done); }}
  .map .part rect {{ fill: var(--part-bg); stroke: var(--part); }}
  .map .block rect {{ fill: var(--block-bg); stroke: var(--block); }}
  .map .none rect {{ fill: var(--none-bg); stroke: var(--none); }}
  .ledger {{ width: 100%; border-collapse: collapse; font-size: .92rem; }}
  .ledger th {{ text-align: left; font-size: .75rem; letter-spacing: .07em; text-transform: uppercase; color: var(--muted);
    border-bottom: 1px solid var(--line); padding: 6px 10px 6px 0; }}
  .ledger td {{ border-bottom: 1px solid var(--line); padding: 9px 10px 9px 0; vertical-align: top; }}
  .ledger td:first-child {{ font-family: var(--mono); font-size: .85rem; white-space: nowrap; }}
  .ledger td:nth-child(2) {{ white-space: nowrap; }}
  .table-wrap {{ overflow-x: auto; }}
  .next {{ display: grid; gap: 8px; margin: 0; padding-left: 1.2em; }}
  .next li::marker {{ color: var(--accent); font-weight: 600; }}
  footer {{ color: var(--muted); font-size: .85rem; }}
  @media (max-width: 640px) {{ .ledger td:first-child {{ white-space: normal; }} }}
</style>
</head>
<body>
<main>
  <header>
    <h1>{e(data["title"])}</h1>
    <div class="meta">Last change: <b>{e(change["what"])}</b>, {long_date(change["date"])}</div>
    <div class="legend" aria-label="Status key">
{legend}
    </div>
  </header>

  <section>
    <h2>How the parts connect</h2>
    <div class="map">
      <svg viewBox="0 0 {d["width"]} {d["height"]}" role="img" aria-label="Diagram of Bright Dawn's parts and their status">
        <defs>
          <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
            <path class="arrow" d="M0,0 L10,5 L0,10 z"></path>
          </marker>
        </defs>
{diagram_body(data)}
      </svg>
    </div>
  </section>

  <section>
    <h2>The parts</h2>
    <div class="table-wrap">
      <table class="ledger">
        <thead><tr><th>Part</th><th>Status</th><th>Now</th></tr></thead>
        <tbody>
{rows}
        </tbody>
      </table>
    </div>
  </section>

  <section>
    <h2>Next</h2>
    <ol class="next">
{steps}
    </ol>
  </section>

  <footer>The same map is in the repository's README, made from the same file
  (<code>tools/status/project_map.json</code>), so the two always agree.</footer>
</main>
</body>
</html>
"""


def short(name: str) -> str:
    """'Partly works' is 'Partly' in the table, as the column says what works."""
    return "Partly" if name == "Partly works" else name


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the SVG or README part is not up to date")
    parser.add_argument("--html", type=Path, help="also write the artifact page here")
    args = parser.parse_args(argv)
    try:
        data = load()
        svg = github_svg(data)
        readme_old = README.read_text(encoding="utf-8")
        readme_new = with_readme_part(readme_old, readme_part(data))
    except (MapError, ValueError, KeyError) as error:
        print(f"render_map: {error}", file=sys.stderr)
        return 2
    if args.check:
        stale = []
        if not SVG.exists() or SVG.read_text(encoding="utf-8") != svg:
            stale.append(str(SVG.relative_to(ROOT)))
        if readme_new != readme_old:
            stale.append("README.md")
        if stale:
            print("render_map: not made from tools/status/project_map.json: " + ", ".join(stale)
                  + "\nRun: python3 tools/status/render_map.py", file=sys.stderr)
            return 1
        print("render_map: the README and its diagram match tools/status/project_map.json")
    else:
        SVG.parent.mkdir(parents=True, exist_ok=True)
        SVG.write_text(svg, encoding="utf-8")
        README.write_text(readme_new, encoding="utf-8")
    if args.html:
        args.html.write_text(page(data), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

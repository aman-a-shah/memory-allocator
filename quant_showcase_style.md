# Quant Showcase — Design System Spec

A portable, copy-paste design specification for producing an **editorial research-paper style
PDF** for quantitative / research projects. Hand this whole file to an AI along with a project's
data and it should produce a PDF **visually identical** to the reference (the Statistical
Arbitrage research note in `site/statistical-arbitrage.pdf`).

> **How to use this with an AI:** "Build a research-note PDF for <project>. Follow
> `QUANT_SHOWCASE_STYLE.md` exactly — same tokens, fonts, layout, chart system, print rules, and
> accessibility rules. Generate all figures from real project output (write an `export_data.py`
> that dumps a `data.json`); never mock numbers." Then describe the project's math, metrics, and
> the figures you want.

**The PDF is rendered from an HTML/CSS source** — that is the trick that buys typeset math, vector
charts, real fonts, and crisp color. You author a single-page HTML document and let headless
Chrome print it to PDF (`--print-to-pdf`). So the deliverable is **5 source files** in a `site/`
folder plus the generated PDF:

| File | Role |
|---|---|
| `index.html` | document structure + content + KaTeX math |
| `styles.css` | the design system **+ a `@media print` block** (the print block is what makes the PDF good) |
| `charts.js` | hand-rolled dependency-free SVG charts + data binding + (screen-only) motion |
| `export_data.py` | runs the real project, writes `data.json` (no mocked numbers) |
| `build_pdf.sh` | regenerates data, serves the folder, prints `index.html` → PDF |
| → `data.json`, `<project>.pdf` | generated artifacts |

Charts are **hand-rolled dependency-free SVG**. Math is **KaTeX from CDN**. There is no bundler.

> **Why HTML→PDF and not a website?** The on-screen page still works in a browser (useful for fast
> iteration), but the canonical output is the PDF. Everything below applies to both; the
> PDF-specific rules live in **§11**.

---

## 0. Adapt this to your project

**This spec is self-contained — you do not need the reference repo.** Copy the **verbatim** blocks
(color tokens §2, the chart engine §6.1, the print block §11.1, the build script §11.4) exactly as
written. **Author** only the project-specific blocks: the page content, the per-chart renderers
(§6.2), and `export_data.py`. The look comes from the verbatim parts; only the domain content changes.

Map whatever your project produces onto the existing components:

| Your project has… | → put it in |
|---|---|
| 3–4 headline results (a key ratio, an error, a throughput) | abstract-stats strip (§5) |
| a method/pipeline with ordered stages | numbered sections + pipeline diagram (§5) |
| governing equations or a derivation | KaTeX display + inline math (§7) |
| fitted parameters / diagnostics | parameter grid (§5) |
| time series, distributions, scatters, comparisons | chart renderers (§6) |
| tabular results (per-run, per-fold, a metrics report) | data tables (§5; they flow across pages §11) |
| one "money" number (speed, scale, accuracy) | throughput callout (§5) |
| how it's built + the exact repro command | colophon (§5) |

Keep the **narrative shape** identical: hook/abstract → a numbered walk through the method →
results → colophon. A good default section spine is `01 Setup/Data → 02 Method → 03 Validation →
04 Results`; rename to fit the domain, but keep it a real sequence (§1).

---

## 1. Voice & register

- **Register:** brand (the page *is* the artifact). It reads like a beautifully typeset research
  paper / Distill.pub article, **not** a SaaS landing page.
- **Three voice words:** precise, scholarly, quietly confident. The rigor is the pitch; never oversell.
- **Core principle — show the evidence:** every claim sits next to the equation or figure that
  proves it. Every number is generated from a real run, not invented.
- **Narrative shape:** a hook/abstract, then a numbered sequence that walks the project's pipeline
  end to end (e.g. `01 Discovery → 02 Model → 03 Validation → 04 Execution → 05 Results`).

---

## 2. Color tokens (OKLCH — copy verbatim)

Near-white **paper** (cool cast, never cream), a near-black **ink ramp**, a single **crimson**
accent, and a muted **slate-blue** as the only second data-series hue. Drop this `:root` in as-is.

```css
:root {
  /* paper / surfaces — true near-white, faint cool cast (never cream) */
  --paper:      oklch(0.989 0.0015 250);
  --paper-2:    oklch(0.971 0.003 252);   /* figure / panel background */
  --paper-3:    oklch(0.955 0.004 252);   /* row hover, in-sample shade */

  /* ink ramp */
  --ink:        oklch(0.235 0.012 264);   /* headings / near-black     (16.2:1) */
  --body:       oklch(0.330 0.012 264);   /* body text                 (11.8:1) */
  --muted:      oklch(0.475 0.012 264);   /* captions / secondary      (6.5:1)  */
  --faint:      oklch(0.550 0.012 264);   /* axis ticks                (4.7:1)  */

  --rule:       oklch(0.885 0.004 258);   /* hairlines                 */
  --rule-soft:  oklch(0.930 0.003 258);   /* lighter hairlines / grid  */

  /* accent — crimson (the ONLY brand color) */
  --accent:     oklch(0.505 0.182 21);    /* lines, dots, markers      (6.3:1)  */
  --accent-ink: oklch(0.430 0.176 21);    /* crimson text/links        (8.6:1)  */
  --accent-12:  oklch(0.505 0.182 21 / 0.12);  /* fills */
  --accent-22:  oklch(0.505 0.182 21 / 0.22);  /* selection */

  /* second data series — muted slate-blue (charts only) */
  --data2:      oklch(0.520 0.085 248);   /* second line / bars        (5.3:1)  */
  --data2-12:   oklch(0.520 0.085 248 / 0.14);

  /* metrics */
  --maxw: 50rem;   /* prose measure ≈ 70ch */
  --figw: 64rem;   /* figures/tables break wider */
  --gap: clamp(1rem, 2.5vw, 2rem);

  --serif: "Source Serif 4", Georgia, "Times New Roman", serif;
  --mono: "JetBrains Mono", ui-monospace, "SF Mono", Menlo, monospace;
  --ease: cubic-bezier(0.22, 1, 0.36, 1);  /* ease-out-quint */
}
```

**Color rules**
- Crimson is the only accent. Use it for links, key inline numbers, section numbers, chart
  lines/markers, and exactly one emphasized pipeline stage. Never introduce a second brand hue.
- `--data2` (slate-blue) appears **only inside charts** as the second series (e.g. frictionless vs
  realistic, Bonferroni vs BH, histogram bars). Never in prose or UI chrome.
- Gains/losses do **not** get green/red. Drawdowns and "losses" use crimson; this is a one-accent
  system on purpose.
- Body bg must stay a true cool near-white. **Never** drift to cream/sand/beige (the AI default).

---

## 3. Typography

Load from Google Fonts (both chosen deliberately *off* the over-used reflex list):

```html
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Source+Serif+4:ital,opsz,wght@0,8..60,300..700;1,8..60,400..600&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
```

- **Source Serif 4** — all prose and headings. `font-optical-sizing: auto`. This is the scholarly
  voice; one family carrying display→body via weight/optical contrast.
- **JetBrains Mono** — every number, metric, axis label, code, technical tag, and section number.
  Always with `font-feature-settings: "tnum" 1` (tabular figures) and `letter-spacing: -0.01em`.
- **Pairing logic:** serif + mono is a true contrast-axis pairing (not two similar sans). Mono is
  *functional* here (it's quant data), never costume.

**Type scale** (fluid `clamp()`, ≥1.25 steps):

```css
body  { font: 380 clamp(1.02rem, 0.97rem + 0.3vw, 1.18rem)/1.62 var(--serif); }
h1    { font-weight: 600; font-size: clamp(2.9rem, 1.8rem + 5.4vw, 5.6rem); letter-spacing: -0.025em; }
.lede { font-weight: 360; font-size: clamp(1.18rem, 1.06rem + 0.6vw, 1.5rem); line-height: 1.5; color: var(--ink); }
h2    { font-weight: 600; font-size: clamp(1.7rem, 1.3rem + 1.6vw, 2.5rem); letter-spacing: -0.018em; }
h3    { font-weight: 600; font-size: 1.18rem; }
```

- `text-wrap: balance` on h1–h3; `text-wrap: pretty` on `p`.
- Headings `--ink`; body `--body`; captions `--muted`. `strong` → weight 640 + `--ink`.
- **Never** put a non-breaking space between hero words (it forces overflow on mobile).
- Display heading ceiling ≤ 6rem; letter-spacing floor ≥ -0.04em.

---

## 4. Layout system

- Two widths only: **prose measure** `--maxw` (50rem) for text/equations, **figure width**
  `--figw` (64rem) for charts and tables. Both centered.
- A `.wrap` container: `width: min(100% - 2.5rem, var(--figw)); margin-inline: auto;`.
- Inside a `.prose` block, constrain text children to `--maxw` and center them; let `figure`,
  `.data-table`, and `.param-grid` span the full `--figw`. This produces the signature rhythm:
  narrow column of reading, wide figures breaking out.

```css
.wrap { width: min(100% - 2.5rem, var(--figw)); margin-inline: auto; }
.prose > p, .prose > h2, .prose > h3, .prose .section-head,
.prose > ul, .prose .equation { max-width: var(--maxw); margin-inline: auto; }
```

- **Section dividers:** a single 1px `--rule` hairline between sections (`section + section`).
  No cards, no boxes, no shadows for separation. Generous vertical padding
  `clamp(2.6rem, 6vw, 4.6rem)`.
- **Spacing:** vary it for rhythm — tight groupings (label→value) inside generous section gaps.
- Flexbox for 1D, Grid for 2D. Hairline-separated grids over bordered cards.

---

## 5. Signature components (recipes)

### Masthead
`dateline` (mono, one crimson word) → big `h1` → `lede` paragraph → **abstract-stats strip** →
**hero chart** → **TOC**. No gradient, no hero-metric cards.

### Abstract-stats strip (NOT metric cards)
A `<dl>` of 4 headline numbers, separated by hairlines, bordered top and bottom. This replaces the
banned "big-number card grid".
```css
.abstract-stats { display: grid; grid-template-columns: repeat(4, 1fr); border-block: 1px solid var(--rule); }
.abstract-stats > div { padding: 1rem 1.1rem; border-left: 1px solid var(--rule-soft); }
.abstract-stats > div:first-child { border-left: 0; padding-left: 0; }
.abstract-stats dt { color: var(--muted); font-size: 0.74rem; }
.abstract-stats dd { font: 500 clamp(1.2rem,1rem + 0.9vw,1.7rem) var(--mono); color: var(--ink); }
```

### Section heading
A crimson mono section number set baseline-aligned beside the `h2`. **Only number sections when
they form a genuine sequence** (a real pipeline) — never as decorative `01/02/03` scaffolding.
```html
<div class="section-head"><span class="mono section-no">01</span><h2>Discovering pairs</h2></div>
```

### Figure + caption
Every chart lives in a `<figure>` with a `<figcaption>` that starts with a crimson mono `Fig. N`
/ `Tbl. N` tag and **states what the figure proves**.
```css
.chart { width: 100%; background: var(--paper-2); border: 1px solid var(--rule); border-radius: 2px; }
figcaption { font-size: 0.86rem; color: var(--muted); margin-top: 0.85rem; max-width: 54ch; }
.fignum { color: var(--accent-ink); font-family: var(--mono); font-size: 0.78rem; }
```

### Pipeline diagram
A row of hairline-bordered `.stage` blocks (mono number + label + sub-label) joined by mono `→`
glyphs; exactly **one** stage gets the crimson border (`box-shadow: inset 0 0 0 1px var(--accent-12)`).
Stacks to full-width below 720px.

### Parameter grid
4-up hairline-separated grid for fitted parameters/diagnostics. Greek/symbol keys in **italic
serif crimson**; values in mono `--ink`; description in tiny `--muted`.

### Data tables
Right-aligned numeric columns in mono with tabular figures; first column left-aligned; `--ink`
bottom border under the head, `--rule-soft` between rows, `--paper-3` row hover. Status shown by a
filled crimson dot (`row-pass`) vs a hollow ring (`row-fail`) — **shape, not color alone**.

### Throughput / hero-number callout
One oversized mono number (`clamp(2.4rem, 1.6rem + 3.6vw, 4rem)`) beside a short `--muted`
paragraph, separated from prose by a top hairline. Use sparingly (one per page).

### Colophon
Closes the page: how it's built + a `<pre class="repro">` showing the exact reproduce command,
rendered as inverted (`--ink` bg, `--paper` text).

---

## 6. Chart system (hand-rolled SVG, no libraries)

Charts are generated in `charts.js` as **SVG strings** and injected via `innerHTML`. Rationale:
crisp, dependency-free, full control, matches the "explicit/auditable" ethos.

**Conventions**
- One render function per chart `(node, w, h, data) → svgMarkup`; tables get `(node, data)`.
- Size to the container: read `clientWidth`/`clientHeight`, render `viewBox="0 0 w h"` with
  `preserveAspectRatio="none"`, `width/height 100%`. Re-render on a debounced `resize`.
- Use **log scales** when a distribution spikes (e.g. return histograms: a tall zero-spike + thin
  tail → log y so both read). Label tiny financial quantities in **basis points**, not `0.00%`.
- **Never label a near-constant series with its absolute value.** An equity curve sitting at
  ~1,000,000 throughout makes every y-tick read `1M`; a normalized curve hovering at 1.00 makes
  every tick read `1.00`. Re-express the axis as **cumulative return** (% or bps **from the
  starting value**) so the ticks actually differ. This is the single most common chart bug here.
- Kill negative-zero (`-0.000`) in every formatter.
- Lines: `--ink` primary, `--accent` for the emphasized/result series, `--data2` dashed for the
  comparison series. Markers/critical lines: crimson. Bars: slate. Grid: `--rule-soft`.
- Always include axis labels and a tiny mono legend when there are 2 series. Encode series by
  **dash + label**, never color alone.

**Chart CSS classes** (style these once; renderers reference them):
```css
.chart svg { display: block; width: 100%; height: 100%; }
.c-grid { stroke: var(--rule-soft); } .c-axis { stroke: var(--rule); }
.c-tick { fill: var(--faint); font: 10px var(--mono); }
.c-line { fill: none; stroke: var(--ink); stroke-width: 1.6; stroke-linejoin: round; stroke-linecap: round; }
.c-line-accent { stroke: var(--accent); } .c-line-data2 { stroke: var(--data2); } .c-line-dash { stroke-dasharray: 4 4; }
.c-area-accent { fill: var(--accent-12); }  .c-band { stroke: var(--faint); stroke-dasharray: 3 4; fill: none; }
.c-dot { fill: var(--ink); } .c-dot-accent { fill: var(--accent); } .c-bar { fill: var(--data2); }
.c-marker { stroke: var(--accent); stroke-width: 1.4; } .c-marker-label { fill: var(--accent-ink); font: 10px var(--mono); }
.c-legend { font: 11px var(--mono); }
```

### 6.1 The `charts.js` engine — copy verbatim

This is the project-agnostic core: formatters (with negative-zero handling), scale/tick/SVG
helpers, the data-binding resolver, and all the plumbing (fetch → bind → render → typeset math →
motion → re-render on resize/`beforeprint`). Paste it as-is; you only fill in `R` and `T` (§6.2).

```js
(() => {
  "use strict";
  const REDUCED = matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* formatters — nz() strips negative zero ("-0.00" -> "0.00") */
  const nz = (s) => (parseFloat(s) === 0 ? s.replace("-", "") : s);
  const fmt = {
    int: (v) => Math.round(v).toLocaleString("en-US"),
    num1: (v) => nz(v.toFixed(1)), num2: (v) => nz(v.toFixed(2)),
    num3: (v) => nz(v.toFixed(3)), num4: (v) => nz(v.toFixed(4)),
    pct2: (v) => nz((v * 100).toFixed(2)) + "%", pct3: (v) => nz((v * 100).toFixed(3)) + "%",
    compact: (v) => new Intl.NumberFormat("en-US", { notation: "compact", maximumFractionDigits: 1 }).format(v),
    money: (v) => "$" + Math.round(v).toLocaleString("en-US"),
    sci: (v) => v === 0 ? "0" : (Math.abs(v) >= 1e-3 && Math.abs(v) < 1e4 ? String(Number(v.toPrecision(3))) : v.toExponential(1)),
  };

  /* scale + svg-string helpers */
  const lin = (d0, d1, r0, r1) => (v) => d1 === d0 ? (r0 + r1) / 2 : r0 + ((v - d0) / (d1 - d0)) * (r1 - r0);
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const ext = (a) => [Math.min(...a), Math.max(...a)];
  function niceTicks(min, max, count) {
    if (min === max) return [min];
    const step0 = (max - min) / count, mag = Math.pow(10, Math.floor(Math.log10(step0))), n = step0 / mag;
    const step = (n >= 5 ? 5 : n >= 2 ? 2 : 1) * mag, start = Math.ceil(min / step) * step, out = [];
    for (let t = start; t <= max + step * 1e-6; t += step) out.push(Math.round(t / step) * step);
    return out;
  }
  const line = (x1, y1, x2, y2, c, ex = "") => `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" class="${c}"${ex}/>`;
  const text = (x, y, c, s, ex = "") => `<text x="${x}" y="${y}" class="${c}"${ex}>${s}</text>`;
  const path = (d, c, ex = "") => `<path d="${d}" class="${c}"${ex}/>`;
  const toPath = (xs, ys) => xs.map((x, i) => `${i ? "L" : "M"}${x.toFixed(2)} ${ys[i].toFixed(2)}`).join(" ");
  const svgOpen = (w, h) => `<svg viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" width="100%" height="100%" role="img">`;
  function gridAndAxes(m, w, h, xd, yd, xfmt, yfmt, { xticks = 5, yticks = 5, yLabels = true } = {}) {
    const x = lin(xd[0], xd[1], m.l, w - m.r), y = lin(yd[0], yd[1], h - m.b, m.t); let s = "";
    for (const t of niceTicks(yd[0], yd[1], yticks)) { const yy = y(t); s += line(m.l, yy, w - m.r, yy, "c-grid"); if (yLabels) s += text(m.l - 6, yy + 3, "c-tick c-tick-y", yfmt(t)); }
    if (xticks) for (const t of niceTicks(xd[0], xd[1], xticks)) s += text(x(t), h - m.b + 14, "c-tick c-tick-x", xfmt(t));
    s += line(m.l, h - m.b, w - m.r, h - m.b, "c-axis"); return { s, x, y };
  }

  /* === per-project registries — define your renderers HERE, inside this IIFE,
     so all the helpers above are in scope (see §6.2 for the contract) === */
  const R = {};   // chart key -> (node, w, h, data) => svgString
  const T = {};   // table key -> (node, data) => void (sets node.innerHTML)
  // R.curve = (node, w, h, data) => { ... return svgOpen(w,h) + ... + "</svg>"; };
  // T.metrics = (node, data) => { node.innerHTML = "<table>...</table>"; };

  /* === plumbing — copy verbatim === */
  let DATA = null;
  const resolve = (o, p) => p.split(".").reduce((x, k) => x == null ? undefined : x[k], o);
  function renderOne(node) {
    const k = node.dataset.chart;
    if (T[k]) return T[k](node, DATA);
    if (!R[k]) return;
    const w = Math.max(320, Math.round(node.clientWidth)), h = Math.max(120, Math.round(node.clientHeight));
    node.innerHTML = R[k](node, w, h, DATA);
  }
  const renderAll = () => document.querySelectorAll("[data-chart]").forEach(renderOne);
  function bindStats() {
    document.querySelectorAll("[data-stat]").forEach((n) => {
      const v = resolve(DATA, n.dataset.stat); if (v == null) return;
      const f = n.dataset.fmt; n.textContent = f && fmt[f] ? fmt[f](v) : v;
    });
  }
  function setupMotion() {
    if (REDUCED) return;
    const ts = document.querySelectorAll("figure,.param-grid,.throughput,.pipeline,.abstract-stats,.toc,.hero-spark");
    ts.forEach((t) => t.classList.add("reveal"));
    const io = new IntersectionObserver((es) => es.forEach((e) => { if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); } }), { threshold: 0.12, rootMargin: "0px 0px -8% 0px" });
    ts.forEach((t) => io.observe(t));
  }
  function typesetMath() { if (typeof renderMathInElement !== "function") { setTimeout(typesetMath, 80); return; } renderMathInElement(document.body, { throwOnError: false }); }
  function progress() {
    const bar = document.getElementById("progress-bar"); if (!bar) return;
    const on = () => { const sc = document.documentElement.scrollTop, mx = document.documentElement.scrollHeight - innerHeight; bar.style.transform = `scaleX(${mx > 0 ? clamp(sc / mx, 0, 1) : 0})`; };
    addEventListener("scroll", on, { passive: true }); on();
  }
  let rt; addEventListener("resize", () => { clearTimeout(rt); rt = setTimeout(renderAll, 150); }, { passive: true });
  addEventListener("beforeprint", renderAll);   // crisp charts at print heights (§11.2)
  async function init() {
    try { DATA = await (await fetch("data.json", { cache: "no-cache" })).json(); }
    catch (e) { document.querySelectorAll("[data-chart]").forEach((n) => (n.textContent = "data unavailable — run export_data.py")); return; }
    bindStats(); renderAll(); typesetMath(); setupMotion(); progress();
  }
  document.readyState === "loading" ? addEventListener("DOMContentLoaded", init) : init();
})();
```

### 6.2 Writing a chart renderer (the contract)

Each chart lives on a container `<div class="chart" data-chart="KEY">`; you add `R.KEY`. Each table
lives on `<div class="data-table" data-chart="KEY">` inside a `<figure class="table-figure">`; you add
`T.KEY`. A renderer returns an SVG **string** built from the helpers; never reach outside its data.

```js
// time series, 1–2 lines. <div class="chart" data-chart="curve">
R.curve = (node, w, h, data) => {
  const s = data.curve;                                  // { x:[], y:[], y2?:[] }
  const m = { t: 16, r: 16, b: 28, l: 48 };
  const X = ext(s.x), Y = ext([].concat(s.y, s.y2 || []));
  const { s: base, x, y } = gridAndAxes(m, w, h, X, Y, fmt.num2, fmt.num2, {});
  let out = svgOpen(w, h) + base;
  if (s.y2) out += path(toPath(s.x.map(x), s.y2.map(y)), "c-line c-line-data2 c-line-dash");
  out += path(toPath(s.x.map(x), s.y.map(y)), "c-line c-line-accent");
  if (s.y2) out += text(w - m.r - 4, m.t + 4, "c-legend", "— baseline", ` text-anchor="end" fill="var(--data2)"`);
  return out + "</svg>";
};
```

Apply the §6 conventions in every renderer: log y for spiky distributions; cumulative-return axes
for near-constant series; basis points for tiny quantities; `nz()` on every label; series encoded
by dash + legend, not color alone.

**The catalog of figures worth building** (mix and match to your domain): a scatter with a fit
line; a time series with threshold bands and a shaded in-sample region; a sigmoid/curve with marked
critical values; sorted points against one or two frontier lines; a two-line "with vs. without"
comparison; a value + secondary-panel stack (e.g. equity + drawdown); a log-scale distribution with
percentile markers.

---

## 7. Math (KaTeX)

Typeset all equations — never paraphrase math in prose or screenshot it.

```html
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.css" crossorigin>
<script defer src="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.js" crossorigin></script>
<script defer src="https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/contrib/auto-render.min.js" crossorigin></script>
```
- **Do NOT add `integrity`/SRI hashes** unless you verify them — a wrong hash silently blocks the
  script and all math renders as raw LaTeX.
- Render after the DOM/data are ready, using KaTeX's **default delimiters** (handles both inline
  `\(…\)` and display `\[…\]`): `renderMathInElement(document.body, { throwOnError: false })`.
- Display equations get a centered `.equation` paragraph with `overflow-x: auto`.
- Never embed an HTML element inside a math span (`\mathtt{<span>…}` won't render); put live values
  in prose next to the equation instead.
- `.katex { color: var(--ink); }`.

---

## 8. Motion (screen only — never gates printed content)

Motion is purely for the on-screen reading experience; the **PDF is static** and §11 turns all of
this off. The critical rule: content is **visible by default** so it always prints. Motion is added
only when JS is on (set `<html class="no-js">` then
`document.documentElement.classList.replace('no-js','js')` inline in `<head>`), and reveals must
*enhance* an already-visible default — never gate visibility on a class that a print/headless
render won't trigger.

- **Reveal on scroll:** `IntersectionObserver` (threshold 0.12, `rootMargin: "0px 0px -8% 0px"`)
  adds `.in` to figures/grids/callouts; CSS transitions `opacity` + `translateY(14px)` with
  `--ease`, 0.7s. Stagger is fine; one identical entrance on everything is not.
- **Hero line-draw:** animate the masthead chart's main path via `stroke-dasharray`/`dashoffset`
  to 0 on load (~1.1s).
- **Reading progress bar:** a 2px crimson bar at the top, `scaleX` driven by scroll.
- Ease-out exponential curves only (`var(--ease)`). No bounce/elastic. Don't animate layout props.
- **Reduced motion is mandatory:**
  ```css
  @media (prefers-reduced-motion: reduce) {
    .js .reveal, .js .reveal.in { opacity: 1; transform: none; transition: none; }
    .progress span { display: none; }
    html { scroll-behavior: auto; }
  }
  ```

---

## 9. Accessibility (WCAG AA — non-negotiable)

- Body text ≥4.5:1, large text ≥3:1. The tokens above are pre-verified: body 11.8, muted 6.5,
  faint 4.7, accent-ink 8.6, accent 6.3, data2 5.3, ink 16.2. If you change a token, re-check.
- Never gray-on-tinted light body text "for elegance".
- Charts/tables encode meaning with **shape + label**, not color alone (dashed vs solid, filled vs
  hollow dot, text legend).
- Semantic HTML: `header/main/section/figure/figcaption/dl/table`, real headings, `aria-label`s on
  stat strips and decorative diagrams marked `aria-hidden`.

---

## 10. Data pipeline (`export_data.py` → `data.json`)

- A Python script runs the **real** project and serializes everything the figures need into one
  `data.json`. The site `fetch`es it. No numbers are hand-written into the HTML.
- **Downsample** long curves to ~400–700 points for the browser; keep full-resolution stats
  (drawdown, percentiles) computed server-side.
- Bind prose numbers with `data-stat="path.to.value" data-fmt="num2|pct2|int|compact|money|sci"`
  and a small resolver in `charts.js`, so every inline number stays in sync with the data.
- The page must be served over HTTP (not `file://`) because of `fetch` — this matters at PDF-build
  time too (see §11). The build script serves the folder on a local port, then prints.

---

## 11. PDF / print output (the canonical deliverable)

The PDF is produced by printing the HTML through headless Chrome. Two pieces make it good: a
`@media print` block in `styles.css`, and a `build_pdf.sh` that drives Chrome.

### 11.1 `@page` and the print stylesheet

```css
@page { size: Letter; margin: 15mm 16mm 16mm; }   /* or A4 */

@media print {
  /* 1. PRESERVE COLOR — without this Chrome drops backgrounds/tints */
  *, *::before, *::after { -webkit-print-color-adjust: exact; print-color-adjust: exact; }

  /* 2. KILL MOTION & INTERACTIVE CHROME so nothing prints blank or stray */
  .js .reveal, .js .reveal.in { opacity: 1 !important; transform: none !important; transition: none !important; }
  .js .c-line.draw { stroke-dasharray: none !important; stroke-dashoffset: 0 !important; }
  .progress, .site-footer { display: none !important; }
  a { color: var(--ink); text-decoration: none; }

  /* 3. RE-ASSERT WIDE LAYOUT — the page content box (~7in) is < 720px, so the
        mobile media queries would otherwise fold your grids to 1–2 columns */
  .wrap { width: 100%; }
  .abstract-stats, .param-grid { grid-template-columns: repeat(4, 1fr); }
  .pipeline { flex-wrap: nowrap; } .pipeline .stage { flex: 1 1 0; min-width: 0; } .pipeline .flow { display: block; }

  /* 4. PAGINATION — keep CHARTS/equations/callouts whole, but let long TABLES
        FLOW across pages (header repeats, rows stay intact) so they fill the
        current page instead of bumping wholesale and stranding a gap */
  figure:not(.table-figure), .param-grid, .pipeline, .throughput,
  .equation, .repro { break-inside: avoid; }
  tr { break-inside: avoid; } thead { display: table-header-group; }
  .table-figure { break-inside: auto; } .table-figure figcaption { break-before: avoid; }
  h1, h2, h3, .section-head { break-after: avoid; } .section-head + p { break-before: avoid; }
  .colophon { break-inside: avoid; }   /* short closing note stays whole */

  /* 5. TABLES fit the page with natural AUTO layout once wide text columns are
        abbreviated (e.g. "PAIR2_Y/PAIR2_X" -> stem "PAIR2" in the renderer).
        No fixed layout, no wrapping — everything sits on one line. */
  .data-table { overflow: visible; }
  table { font-size: 8.7pt; width: 100%; }
  thead th, tbody td { white-space: nowrap; padding-inline: 0.55rem; }

  /* 6. DENSITY + per-chart print heights (charts re-render at these on
        beforeprint, so no scaling distortion — see 11.2/charts.js) */
  body { font-size: 10pt; line-height: 1.48; }
  .masthead { padding: 0 0 1rem; } .masthead h1 { font-size: 2.7rem; }
  section { padding-block: 1.05rem; } .prose p { margin-bottom: 0.7rem; }
  figure { margin: 0.9rem auto; } figcaption { margin-top: 0.5rem; } .equation { margin: 0.9rem auto !important; }
  p { orphans: 2; widows: 2; }
  /* Give every chart container an id (e.g. #fig-curve) and a print height.
     Start them all ~220px, then tune each ±20px while watching the per-page
     gap (§11.3). Charts re-render at these via the beforeprint hook above. */
  .chart { height: 220px; }
  #fig-hero { height: 165px; }          /* the masthead sparkline */
  /* #fig-foo { height: 248px; }  #fig-bar { height: 200px; }  ...per chart... */
}
```

In `charts.js`, re-render charts when print layout activates, so they draw at the
print heights above (not scaled from the screen render):

```js
addEventListener("beforeprint", renderAll);   // renderAll re-reads each container's clientHeight
```

### 11.2 Gotchas (each was a real bug)

- **Color adjust is mandatory.** Without `print-color-adjust: exact`, the paper tint, crimson
  lines, and slate bars vanish to white in the PDF.
- **The page is narrower than your mobile breakpoint.** Letter/A4 content width ≈ 7in ≈ 670–700px,
  which is below a 720px `max-width` query — so your responsive rules fire in print and collapse
  the layout unless you re-assert the wide grids inside `@media print`.
- **`overflow-x: auto` does nothing on paper.** A wide table just clips off the page edge. The fix
  is to make the content fit: abbreviate the wide text column in the data layer (pair stems, not
  full names) so natural auto-layout fits on one line. Don't reach for `table-layout: fixed` +
  wrapping; and **never** `overflow-wrap: anywhere` (it shatters `PAIR2_Y/PAIR2_X` mid-token).
- **Re-render charts on `beforeprint`.** Charts size to their on-screen container at JS time and
  `preserveAspectRatio="none"` would otherwise stretch them to the print box. An
  `addEventListener("beforeprint", renderAll)` redraws each chart at its print height — crisp, and
  it frees you to tune print chart heights independently of the screen sizes.
- **Reveal/opacity must be forced visible in print** (`opacity:1 !important`) or figures that never
  scrolled into view render blank.

### 11.3 Eliminating page gaps (the layout-quality pass)

A single-column, figure-heavy document breaks awkwardly by default: a tall figure hits
`break-inside: avoid`, doesn't fit the remaining space, bumps to the next page, and **strands a
half-empty page**. Fixing this is iterative and measurable — don't eyeball it.

**Measure objectively.** Render every page to an image (PyMuPDF `get_pixmap`) and compute the
bottom whitespace: the fraction below the last row containing a non-white pixel. Anything over
~20% on a non-final page is a gap to chase. (Reference result after tuning: 8 pages, every page
≤19%, average ~12%, no empty pages — down from 13 pages with 35–50% gaps.)

**The five levers, in order of impact:**

1. **Let tables flow** (§11.1 item 4). The biggest single win: a long table wrapped in an
   avoid-break `figure` bumps wholesale. Allow `.table-figure` to break (rows intact, header
   repeats) and it fills the current page, continuing on the next.
2. **Fill, don't shrink, when a figure sits at a page bottom.** Counterintuitive: if a chart is the
   last thing on a page and the *next* block bumped, making that chart **taller** pushes its
   caption down and shrinks the gap. Making it shorter *widens* the gap. (We took the phantom chart
   from 184px → 252px to fill its page.)
3. **Compact trailing blocks so they fit instead of bumping.** Short callouts and the colophon
   bump and strand a near-empty final page. Shrink their spacing / heading size / copy by ~30–50px
   so they pack onto the preceding page. (Colophon `h2` → 1.55rem, one-line fine print, tighter
   `repro` — that collapsed a lone 9th page into page 8.)
4. **Global density.** 10pt body, `line-height` ~1.48, section padding ~1rem, tighter figure and
   equation margins. Packs ~1 extra block per page across the document.
5. **Per-chart height tuning.** Give each chart its own print height and nudge ±20px while watching
   the per-page gap measurement. It's whack-a-mole, but 4–5 iterations converges.

Some residual gap is unavoidable in single-column with half-page figures; aim for ≤~18% on
interior pages and a clean (whole) closing block, not zero.

### 11.4 `build_pdf.sh`

```bash
# regenerate data → serve (fetch needs HTTP) → headless print to PDF
PYTHONPATH=src python3 site/export_data.py
python3 -m http.server 8013 --directory site & SRV=$!; sleep 1
"<chrome>" --headless --disable-gpu --no-pdf-header-footer \
  --virtual-time-budget=12000 --run-all-compositor-stages-before-draw \
  --window-size=760,1100 \
  --print-to-pdf="site/<project>.pdf" "http://localhost:8013/index.html"
kill $SRV
```
- `--virtual-time-budget` lets the async `fetch` + KaTeX typeset + SVG build finish before capture
  (raise it if math/charts come out raw/blank).
- `--no-pdf-header-footer` drops the browser's date/URL chrome.
- `--window-size` width (~760) sets the on-screen render width so charts are sized for the page.
- Verify with any PDF→image renderer (e.g. PyMuPDF `get_pixmap`) and eyeball every page.

---

## 12. Build checklist

- [ ] 5 source files in `site/`: `index.html`, `styles.css`, `charts.js`, `export_data.py`,
      `build_pdf.sh` → producing `data.json` + `<project>.pdf`.
- [ ] `:root` tokens copied verbatim; cool near-white bg (not cream); single crimson accent.
- [ ] Source Serif 4 + JetBrains Mono loaded; numbers in mono with tabular figures.
- [ ] Prose at `--maxw`, figures/tables at `--figw`; hairline section dividers; no cards/shadows.
- [ ] Masthead = dateline + h1 + lede + abstract-stats strip + hero chart + TOC.
- [ ] Sections numbered **only** if a real sequence; each `h2` paired with a crimson mono number.
- [ ] Every figure in a `<figure>` with a `Fig. N` caption that states what it proves.
- [ ] Charts hand-rolled SVG; `--ink`/`--accent`/`--data2` series; log scale + bps where apt;
      near-constant series shown as cumulative return; 2-series charts have a dashed+labeled legend.
- [ ] KaTeX from CDN **without** bogus SRI; default delimiters; equations typeset, not paraphrased.
- [ ] `@media print` block present: color-adjust exact, motion off, wide grids re-asserted, tables
      flow across pages (auto layout; wide text columns abbreviated to fit one line), charts/colophon
      kept whole, `beforeprint` re-render hook, per-chart print heights.
- [ ] All numbers from a real run via `export_data.py`; nothing mocked.
- [ ] **PDF rendered and every page eyeballed:** no clipped tables, no `1M`/`1.00`-collapsed axes,
      no blank reveals, no near-empty orphan pages, colors intact.

---

## 13. Anti-patterns (reject on sight)

- Cream / sand / beige body background; warm-tinted near-white. Stay cool-neutral.
- A second brand color, or green/red gain-loss coloring. One crimson accent only.
- SaaS hero with gradient + big-metric cards + identical icon-card grid.
- Neon-green terminal or navy-and-gold "fintech" clichés.
- Tiny uppercase tracked eyebrows above every section; numbered markers used decoratively.
- Side-stripe `border-left` accents on callouts; gradient text; decorative glassmorphism.
- Charts as decoration. Every figure carries information the reader needs.
- Mocked numbers, screenshot equations, or paraphrased math.
- y-axes that collapse to one repeated label (`1M`, `1.00`) on a near-constant series.
- Shipping the PDF without opening it: clipped tables, dropped colors, blank reveal-gated figures.
- Non-breaking spaces in the hero heading; SRI hashes you didn't verify.

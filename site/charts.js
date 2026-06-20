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
    if (!isFinite(min) || !isFinite(max) || min === max) return [min || 0];
    const step0 = (max - min) / count, mag = Math.pow(10, Math.floor(Math.log10(step0))), n = step0 / mag;
    const step = (n >= 5 ? 5 : n >= 2 ? 2 : 1) * mag, start = Math.ceil(min / step) * step, out = [];
    if (!(step > 0)) return [min, max];   // guard: never loop on a zero/NaN step
    for (let t = start; t <= max + step * 1e-6 && out.length < 1000; t += step) out.push(Math.round(t / step) * step);
    return out;
  }
  const line = (x1, y1, x2, y2, c, ex = "") => `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" class="${c}"${ex}/>`;
  const text = (x, y, c, s, ex = "") => `<text x="${x}" y="${y}" class="${c}"${ex}>${s}</text>`;
  const path = (d, c, ex = "") => `<path d="${d}" class="${c}"${ex}/>`;
  const rect = (x, y, w, h, c, ex = "") => `<rect x="${x}" y="${y}" width="${w}" height="${h}" class="${c}"${ex}/>`;
  const circle = (x, y, r, c, ex = "") => `<circle cx="${x}" cy="${y}" r="${r}" class="${c}"${ex}/>`;
  const toPath = (xs, ys) => xs.map((x, i) => `${i ? "L" : "M"}${x.toFixed(2)} ${ys[i].toFixed(2)}`).join(" ");
  const svgOpen = (w, h) => `<svg viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" width="100%" height="100%" role="img">`;
  function gridAndAxes(m, w, h, xd, yd, xfmt, yfmt, { xticks = 5, yticks = 5, yLabels = true } = {}) {
    const x = lin(xd[0], xd[1], m.l, w - m.r), y = lin(yd[0], yd[1], h - m.b, m.t); let s = "";
    for (const t of niceTicks(yd[0], yd[1], yticks)) { const yy = y(t); s += line(m.l, yy, w - m.r, yy, "c-grid"); if (yLabels) s += text(m.l - 6, yy + 3, "c-tick c-tick-y", yfmt(t)); }
    if (xticks) for (const t of niceTicks(xd[0], xd[1], xticks)) s += text(x(t), h - m.b + 14, "c-tick c-tick-x", xfmt(t));
    s += line(m.l, h - m.b, w - m.r, h - m.b, "c-axis"); return { s, x, y };
  }

  /* === per-project registries ============================================ */
  const R = {};   // chart key -> (node, w, h, data) => svgString
  const T = {};   // table key -> (node, data) => void

  /* extra local helpers for this document's renderers */
  const log10 = Math.log10;
  const legend = (x, y, items) => items.map((it, i) =>
    `<line x1="${x}" y1="${y + i * 14 - 3}" x2="${x + 16}" y2="${y + i * 14 - 3}" class="c-line ${it.cls}"${it.dash ? ' stroke-dasharray="4 4"' : ""}/>` +
    text(x + 22, y + i * 14, "c-legend", it.label)).join("");
  const nsFmt = (v) => v >= 1000 ? fmt.compact(v) : Math.round(v) + "";

  /* ---- Hero: latency distribution (determinism) ------------------------- */
  R.latency_hist = (node, w, h, data) => {
    const d = data.latency_hist, pct = data.latency_pct;
    const edges = d.edges_ns, nb = d.custom.length;
    const centers = [];
    for (let i = 0; i < nb; i++) centers.push(Math.sqrt(edges[i] * edges[i + 1]));
    const m = { t: 18, r: 16, b: 34, l: 46 };
    const xd = [log10(edges[0]), log10(edges[nb])];
    const maxc = Math.max(...d.custom, ...d.sys);
    const yd = [0, log10(maxc + 1)];
    const x = (v) => lin(xd[0], xd[1], m.l, w - m.r)(log10(v));
    const y = (c) => lin(yd[0], yd[1], h - m.b, m.t)(log10(c + 1));
    let out = svgOpen(w, h);
    // y grid at decades
    for (let p = 0; p <= yd[1]; p++) { const yy = y(Math.pow(10, p) - 1); out += line(m.l, yy, w - m.r, yy, "c-grid"); out += text(m.l - 6, yy + 3, "c-tick c-tick-y", fmt.compact(Math.pow(10, p))); }
    // x ticks at decades of ns
    for (let p = Math.ceil(xd[0]); p <= xd[1]; p++) { const xv = Math.pow(10, p); out += text(x(xv), h - m.b + 14, "c-tick c-tick-x", nsFmt(xv) + "ns"); }
    // step areas: custom (accent) then sys (slate dashed line)
    const stepPath = (counts, baseline) => {
      let dd = `M${x(edges[0]).toFixed(2)} ${y(0).toFixed(2)}`;
      for (let i = 0; i < nb; i++) { dd += `L${x(edges[i]).toFixed(2)} ${y(counts[i]).toFixed(2)}L${x(edges[i + 1]).toFixed(2)} ${y(counts[i]).toFixed(2)}`; }
      dd += `L${x(edges[nb]).toFixed(2)} ${y(0).toFixed(2)}`;
      return dd;
    };
    out += path(stepPath(d.sys), "c-area-data2");
    out += path(stepPath(d.custom), "c-area-accent");
    // outlines (use center polyline)
    out += path(toPath(centers.map(x), d.sys.map(y)), "c-line c-line-data2 c-line-dash c-line-thin");
    out += path(toPath(centers.map(x), d.custom.map(y)), "c-line c-line-accent");
    // p99.9 markers
    const mark = (v, cls, lbl, dy) => {
      if (!v || v < edges[0]) return "";
      const xx = x(v);
      return line(xx, m.t, xx, h - m.b, cls + " c-line-dash") +
        text(xx + 3, m.t + dy, "c-marker-label", lbl);
    };
    out += mark(pct.custom_ns[3], "c-marker", "p99.9 " + nsFmt(pct.custom_ns[3]) + "ns", 10);
    out += text(w - m.r, h - m.b + 28, "c-axis-label", "allocation latency (log)", ' text-anchor="end"');
    out += legend(m.l + 8, m.t + 6, [{ cls: "c-line-accent", label: "memalloc" }, { cls: "c-line-data2", dash: 1, label: "system malloc" }]);
    return out + "</svg>";
  };

  /* ---- Throughput by scenario (grouped bars) ---------------------------- */
  R.throughput = (node, w, h, data) => {
    const d = data.throughput;
    const labels = ["Slab / fixed", "TLSF / var", "HFT mixed"];
    const n = d.custom_mops.length;
    const m = { t: 22, r: 16, b: 34, l: 46 };
    const maxv = Math.max(...d.custom_mops, ...d.sys_mops) * 1.16;
    const { s: base, y } = gridAndAxes(m, w, h, [0, 1], [0, maxv], () => "", fmt.int, { xticks: 0, yticks: 5 });
    let out = svgOpen(w, h) + base;
    const groupW = (w - m.l - m.r) / n, barW = groupW * 0.3, y0 = h - m.b;
    for (let i = 0; i < n; i++) {
      const cx = m.l + groupW * i + groupW / 2;
      const cVal = d.custom_mops[i], sVal = d.sys_mops[i];
      const cy = y(cVal), sy = y(sVal);
      out += rect(cx - barW - 3, sy, barW, y0 - sy, "c-bar");                 // system (slate)
      out += rect(cx + 3, cy, barW, y0 - cy, "c-bar-accent");                 // custom (crimson)
      out += text(cx + 3 + barW / 2, cy - 5, "c-val-accent", fmt.num1(cVal), ' text-anchor="middle"');
      out += text(cx - barW / 2 - 3, sy - 5, "c-val", fmt.num1(sVal), ' text-anchor="middle"');
      out += text(cx, h - m.b + 14, "c-tick c-tick-x", labels[i]);
      out += text(cx, h - m.b + 26, "c-marker-label", fmt.num1(d.speedup[i]) + "×", ' text-anchor="middle"');
    }
    out += text(m.l - 6, m.t - 8, "c-axis-label", "M ops/s");
    out += legend(w - m.r - 150, m.t - 2, [{ cls: "c-line-accent", label: "memalloc" }, { cls: "c-line-data2", label: "system malloc" }]);
    return out + "</svg>";
  };

  /* ---- O(1): latency vs request size ------------------------------------ */
  R.by_size = (node, w, h, data) => {
    const d = data.by_size;
    const m = { t: 18, r: 16, b: 36, l: 46 };
    const xd = [log10(d.sizes[0]), log10(d.sizes[d.sizes.length - 1])];
    const maxy = Math.max(...d.custom_ns, ...d.sys_ns) * 1.1;
    const yd = [0, maxy];
    const x = (v) => lin(xd[0], xd[1], m.l, w - m.r)(log10(v));
    const y = lin(yd[0], yd[1], h - m.b, m.t);
    let out = svgOpen(w, h);
    for (const t of niceTicks(0, maxy, 5)) { const yy = y(t); out += line(m.l, yy, w - m.r, yy, "c-grid") + text(m.l - 6, yy + 3, "c-tick c-tick-y", fmt.int(t)); }
    for (const s of d.sizes) { const lbl = s >= 1024 ? (s / 1024) + "K" : s + ""; out += text(x(s), h - m.b + 14, "c-tick c-tick-x", lbl); }
    out += line(m.l, h - m.b, w - m.r, h - m.b, "c-axis");
    const xs = d.sizes.map(x);
    out += path(toPath(xs, d.sys_ns.map(y)), "c-line c-line-data2 c-line-dash");
    out += path(toPath(xs, d.custom_ns.map(y)), "c-line c-line-accent");
    d.sizes.forEach((s, i) => { out += circle(x(s), y(d.sys_ns[i]), 2.4, "c-dot-data2"); out += circle(x(s), y(d.custom_ns[i]), 2.6, "c-dot-accent"); });
    out += text((m.l + w - m.r) / 2, h - m.b + 30, "c-axis-label", "request size (bytes, log)", ' text-anchor="middle"');
    out += text(m.l - 6, m.t - 6, "c-axis-label", "ns / alloc–free");
    out += legend(m.l + 10, m.t + 8, [{ cls: "c-line-accent", label: "memalloc (flat = O(1))" }, { cls: "c-line-data2", dash: 1, label: "system malloc" }]);
    return out + "</svg>";
  };

  /* ---- Scalability: throughput vs threads ------------------------------- */
  R.scalability = (node, w, h, data) => {
    const d = data.scalability;
    const m = { t: 18, r: 16, b: 34, l: 48 };
    const th = d.threads;
    const ideal = th.map((t) => d.custom_mops[0] * t);
    const maxy = Math.max(...d.custom_mops, ...d.sys_mops, ...ideal) * 1.06;
    const xd = [th[0], th[th.length - 1]], yd = [0, maxy];
    const x = lin(xd[0], xd[1], m.l, w - m.r), y = lin(yd[0], yd[1], h - m.b, m.t);
    let out = svgOpen(w, h);
    for (const t of niceTicks(0, maxy, 5)) { const yy = y(t); out += line(m.l, yy, w - m.r, yy, "c-grid") + text(m.l - 6, yy + 3, "c-tick c-tick-y", fmt.int(t)); }
    for (const t of th) out += text(x(t), h - m.b + 14, "c-tick c-tick-x", t + "");
    out += line(m.l, h - m.b, w - m.r, h - m.b, "c-axis");
    const xs = th.map(x);
    out += path(toPath(xs, ideal.map(y)), "c-line c-line-thin", ' stroke="var(--accent-22)" stroke-dasharray="2 4"');
    out += path(toPath(xs, d.sys_mops.map(y)), "c-line c-line-data2 c-line-dash");
    out += path(toPath(xs, d.custom_mops.map(y)), "c-line c-line-accent");
    th.forEach((t, i) => { out += circle(x(t), y(d.sys_mops[i]), 2.6, "c-dot-data2"); out += circle(x(t), y(d.custom_mops[i]), 2.8, "c-dot-accent"); });
    out += text((m.l + w - m.r) / 2, h - m.b + 30, "c-axis-label", "threads", ' text-anchor="middle"');
    out += text(m.l - 6, m.t - 6, "c-axis-label", "M ops/s");
    out += legend(m.l + 10, m.t + 8, [{ cls: "c-line-accent", label: "memalloc" }, { cls: "c-line-data2", dash: 1, label: "system malloc" }]);
    out += text(x(th[th.length - 1]) - 4, y(ideal[ideal.length - 1]) + 4, "c-axis-label", "ideal", ' text-anchor="end"');
    return out + "</svg>";
  };

  /* ---- Fragmentation over the workload ---------------------------------- */
  R.fragmentation = (node, w, h, data) => {
    const d = data.fragmentation;
    const m = { t: 18, r: 16, b: 34, l: 46 };
    const xd = ext(d.cycle_k);
    const yd = [0, Math.max(3.4, Math.max(...d.frag_pct) * 1.3)];
    const x = lin(xd[0], xd[1], m.l, w - m.r), y = lin(yd[0], yd[1], h - m.b, m.t);
    let out = svgOpen(w, h);
    for (const t of niceTicks(0, yd[1], 5)) { const yy = y(t); out += line(m.l, yy, w - m.r, yy, "c-grid") + text(m.l - 6, yy + 3, "c-tick c-tick-y", fmt.num1(t) + "%"); }
    for (const t of niceTicks(xd[0], xd[1], 5)) out += text(x(t), h - m.b + 14, "c-tick c-tick-x", fmt.compact(t * 1000));
    out += line(m.l, h - m.b, w - m.r, h - m.b, "c-axis");
    // 3% target line
    const ty = y(3);
    out += line(m.l, ty, w - m.r, ty, "c-marker c-line-dash") + text(w - m.r - 2, ty - 4, "c-marker-label", "3% target", ' text-anchor="end"');
    out += path(toPath(d.cycle_k.map(x), d.frag_pct.map(y)), "c-line c-line-accent");
    out += text((m.l + w - m.r) / 2, h - m.b + 30, "c-axis-label", "operations processed", ' text-anchor="middle"');
    out += text(m.l - 6, m.t - 6, "c-axis-label", "fragmentation");
    return out + "</svg>";
  };

  /* ---- Stability: footprint plateau over 12k cycles --------------------- */
  R.stability = (node, w, h, data) => {
    const d = data.stability;
    const m = { t: 18, r: 16, b: 34, l: 48 };
    const xd = ext(d.cycle);
    const maxmb = Math.max(...d.footprint_mb) * 1.4;
    const yd = [0, maxmb];
    const x = lin(xd[0], xd[1], m.l, w - m.r), y = lin(yd[0], yd[1], h - m.b, m.t);
    let out = svgOpen(w, h);
    for (const t of niceTicks(0, maxmb, 5)) { const yy = y(t); out += line(m.l, yy, w - m.r, yy, "c-grid") + text(m.l - 6, yy + 3, "c-tick c-tick-y", fmt.num1(t)); }
    for (const t of niceTicks(xd[0], xd[1], 6)) out += text(x(t), h - m.b + 14, "c-tick c-tick-x", fmt.compact(t));
    out += line(m.l, h - m.b, w - m.r, h - m.b, "c-axis");
    const linePath = toPath(d.cycle.map(x), d.footprint_mb.map(y));
    const areaPath = linePath + `L${x(xd[1]).toFixed(2)} ${y(0).toFixed(2)}L${x(xd[0]).toFixed(2)} ${y(0).toFixed(2)}Z`;
    out += path(areaPath, "c-area-accent");
    out += path(linePath, "c-line c-line-accent");
    d.cycle.forEach((c, i) => { out += circle(x(c), y(d.footprint_mb[i]), 2.2, "c-dot-accent"); });
    out += text((m.l + w - m.r) / 2, h - m.b + 30, "c-axis-label", "allocate/free cycles", ' text-anchor="middle"');
    out += text(m.l - 6, m.t - 6, "c-axis-label", "resident MB");
    return out + "</svg>";
  };

  /* ---- Tables ----------------------------------------------------------- */
  T.latency_pct = (node, data) => {
    const d = data.latency_pct;
    const cell = (v) => v < 41 ? "&lt;41" : (v >= 1000 ? fmt.compact(v) : Math.round(v) + "");
    let rows = "";
    for (let i = 0; i < d.labels.length; i++) {
      const ratio = d.custom_ns[i] > 0 ? d.sys_ns[i] / d.custom_ns[i] : null;
      rows += `<tr><td>${d.labels[i]}</td><td>${cell(d.custom_ns[i])}</td><td>${cell(d.sys_ns[i])}</td>` +
        `<td class="pos">${ratio && ratio >= 1.05 ? fmt.num1(ratio) + "×" : "—"}</td></tr>`;
    }
    node.innerHTML = `<table><thead><tr><th>Percentile</th><th>memalloc (ns)</th><th>system (ns)</th><th>tighter</th></tr></thead><tbody>${rows}</tbody></table>`;
  };

  T.targets = (node, data) => {
    const h = data.headline;
    const rows = [
      ["Worst-case time complexity", "O(1)", "O(1) — flat across 16 B–64 KB", true],
      ["Throughput vs system malloc", "≥6× (vs glibc)", fmt.num1(h.speedup_slab) + "–" + fmt.num1(h.speedup_tlsf) + "× (vs Apple libc)", false],
      ["Memory fragmentation", "<3%", fmt.num2(h.fragmentation_pct) + "%", h.fragmentation_pct < 3],
      ["Latency determinism (tail)", "tight p99.9",
        "p99.9 " + (h.p999_custom_ns < 41 ? "&lt;41" : Math.round(h.p999_custom_ns)) +
        " ns vs malloc " + (h.p999_sys_ns >= 1000 ? fmt.compact(h.p999_sys_ns) : Math.round(h.p999_sys_ns)) + " ns", true],
      ["Hot-path kernel switches", "0", "0 (user-space arena)", true],
      ["Hot-path lock contention", "0", "lock-free (TSan-clean)", true],
    ];
    let body = "";
    for (const [m, t, meas, pass] of rows)
      body += `<tr class="${pass ? "row-pass" : "row-fail"}"><td>${m}</td><td>${t}</td><td>${meas}</td></tr>`;
    node.innerHTML = `<table><thead><tr><th>PRD target</th><th>Goal</th><th>Measured</th></tr></thead><tbody>${body}</tbody></table>`;
  };

  T.report = (node, data) => {
    const h = data.headline, tp = data.throughput, lp = data.latency_pct,
          sc = data.scalability, st = data.stability;
    const ns = (v) => v < 41 ? "&lt;41 ns" : (v >= 1000 ? fmt.compact(v) + " ns" : Math.round(v) + " ns");
    const mops = (v) => fmt.num1(v) + " M ops/s";
    const fp = st.footprint_mb[st.footprint_mb.length - 1];
    const rows = [
      ["Mixed throughput", mops(tp.custom_mops[2]), "Slab throughput", mops(tp.custom_mops[0])],
      ["TLSF throughput", mops(tp.custom_mops[1]), "Speedup vs malloc (mixed)", fmt.num1(h.speedup_mixed) + "×"],
      ["p50 latency", ns(lp.custom_ns[0]), "p99 latency", ns(lp.custom_ns[2])],
      ["p99.9 latency", ns(lp.custom_ns[3]), "Tail vs malloc (p99.9)", fmt.num1(h.tail_ratio) + "×"],
      ["Fragmentation", fmt.num2(h.fragmentation_pct) + "%", "Resident footprint", fmt.num1(fp) + " MB"],
      ["Throughput, 1 thread", mops(sc.custom_mops[0]), "Throughput, 8 threads", mops(sc.custom_mops[sc.custom_mops.length - 1])],
      ["Scaling, 1 → 8 threads", fmt.num1(sc.custom_mops[sc.custom_mops.length - 1] / sc.custom_mops[0]) + "×", "Events replayed", fmt.compact(h.events)],
    ];
    let body = "";
    for (const [k1, v1, k2, v2] of rows)
      body += `<tr><td>${k1}</td><td>${v1}</td><td>${k2}</td><td>${v2}</td></tr>`;
    node.innerHTML = `<table class="report-table"><tbody>${body}</tbody></table>`;
  };

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
    const ts = document.querySelectorAll("figure,.param-grid,.throughput,.pipeline,.abstract-stats,.toc");
    ts.forEach((t) => t.classList.add("reveal"));
    const io = new IntersectionObserver((es) => es.forEach((e) => { if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); } }), { threshold: 0.12, rootMargin: "0px 0px -8% 0px" });
    ts.forEach((t) => io.observe(t));
  }
  let mathTries = 0;
  function typesetMath() {
    if (typeof renderMathInElement !== "function") {
      if (mathTries++ < 60) setTimeout(typesetMath, 60);   // bounded: never block print
      return;
    }
    // Scope KaTeX to the equation blocks only. Running it over document.body
    // walks every injected chart-SVG text node and is pathologically slow there.
    document.querySelectorAll(".equation").forEach((el) =>
      renderMathInElement(el, { throwOnError: false }));
  }
  function progress() {
    const bar = document.getElementById("progress-bar"); if (!bar) return;
    const on = () => { const sc = document.documentElement.scrollTop, mx = document.documentElement.scrollHeight - innerHeight; bar.style.transform = `scaleX(${mx > 0 ? clamp(sc / mx, 0, 1) : 0})`; };
    addEventListener("scroll", on, { passive: true }); on();
  }
  let rt; addEventListener("resize", () => { clearTimeout(rt); rt = setTimeout(renderAll, 150); }, { passive: true });
  addEventListener("beforeprint", renderAll);
  function paint() {
    bindStats(); renderAll(); if (!window.__NO_MATH__) typesetMath(); setupMotion(); progress();
    window.__RENDER_DONE__ = true;
  }
  async function init() {
    // Prefer inlined data (window.__DATA__): lets the page render synchronously
    // so a headless print captures fully-drawn charts. Fall back to fetch for
    // the canonical on-screen experience.
    if (window.__DATA__) { DATA = window.__DATA__; paint(); return; }
    try { DATA = await (await fetch("data.json", { cache: "no-cache" })).json(); }
    catch (e) { document.querySelectorAll("[data-chart]").forEach((n) => (n.textContent = "data unavailable — run export_data.py")); return; }
    paint();
  }
  // Fallback so a print is never blocked if data/render fails.
  setTimeout(() => { window.__RENDER_DONE__ = true; }, 8000);
  document.readyState === "loading" ? addEventListener("DOMContentLoaded", init) : init();
})();

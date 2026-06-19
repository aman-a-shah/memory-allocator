// Build-time KaTeX pre-render.
//
// Client-side renderMathInElement hangs in this headless-Chrome PDF build (a
// font/layout interaction with the injected chart SVGs), so for the print build
// we render every \[...\] / \(...\) to static KaTeX HTML here and disable the
// client-side typeset. The on-screen index.html keeps live KaTeX. Output:
// print.html, identical to index.html but with math baked in.
//
// Usage: node render_math.cjs <in.html> <out.html>

const fs = require("fs");
const katex = require("./vendor/katex/katex.min.js");

const [inPath, outPath] = process.argv.slice(2);
if (!inPath || !outPath) {
  console.error("usage: node render_math.cjs <in.html> <out.html>");
  process.exit(2);
}

let html = fs.readFileSync(inPath, "utf8");

const render = (tex, display) => {
  try {
    return katex.renderToString(tex.trim(), { displayMode: display, throwOnError: false });
  } catch (e) {
    console.error("[render_math] katex error:", e.message);
    return tex;
  }
};

let n = 0;
// Display math \[ ... \]
html = html.replace(/\\\[([\s\S]*?)\\\]/g, (_, tex) => { n++; return render(tex, true); });
// Inline math \( ... \)
html = html.replace(/\\\(([\s\S]*?)\\\)/g, (_, tex) => { n++; return render(tex, false); });

// Disable the client-side typeset so charts.js doesn't re-run KaTeX.
html = html.replace(
  '<script defer src="charts.js"></script>',
  '<script>window.__NO_MATH__=1</script>\n<script defer src="charts.js"></script>'
);

fs.writeFileSync(outPath, html);
console.error(`[render_math] pre-rendered ${n} equations -> ${outPath}`);

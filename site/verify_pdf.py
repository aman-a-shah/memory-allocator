#!/usr/bin/env python3
"""Measure per-page bottom whitespace in a PDF (QUANT_SHOWCASE_STYLE.md §11.3).

For each page, render to an image and report the fraction of vertical space
below the last row containing a non-white pixel. Interior pages over ~20% are
gaps worth chasing; the final page is expected to be short.
"""
import sys
import fitz  # PyMuPDF


def main(path):
    doc = fitz.open(path)
    n = len(doc)
    print(f"[verify] {path}: {n} page(s)")
    worst = 0.0
    for i, page in enumerate(doc):
        pix = page.get_pixmap(dpi=110)
        w, h, comps = pix.width, pix.height, pix.n
        data = pix.samples
        last_content = 0
        for y in range(h):
            row = data[y * w * comps:(y + 1) * w * comps]
            # any pixel darker than near-white in any channel?
            if any(row[x] < 245 for x in range(0, len(row), comps)):
                last_content = y
        gap = 1.0 - (last_content + 1) / h
        tag = "" if (i == n - 1 or gap <= 0.20) else "  <-- gap"
        if i != n - 1:
            worst = max(worst, gap)
        print(f"  page {i+1:2d}: bottom whitespace {gap*100:5.1f}%{tag}")
    print(f"[verify] worst interior-page gap: {worst*100:.1f}%")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "research-note.pdf")

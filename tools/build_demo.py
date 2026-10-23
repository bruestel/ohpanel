#!/usr/bin/env python3
"""
The web interface as a demo without a panel: a static site that any web server (GitHub Pages)
can serve, the API answered in the browser by demo/mock.js from invented data.

    tools/build_demo.py [out]        # default: build/demo

Writes out/index.html (www/index.html with the mock loaded before its script), the mock and its
data (demo/), the preview's layout (www/layout-eu.json: the demo is an NSPanel EU) and the icon set (the firmware's, which the
interface fetches as /api/v1/assets), and the QR codes of the demo's qr pages, made with the
firmware's library (components/qrcodegen) as POST /api/v1/qr makes them: the mock has no encoder.
"""

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


# what POST /api/v1/qr answers (components/app_web/src/WebDisplay.cpp post_qr), for one text
QR_C = r"""
#include <stdio.h>
#include <string.h>
#include "qrcodegen.h"
int main(int argc, char **argv) {
  const char *e = argv[2];
  enum qrcodegen_Ecc level = !strcmp(e, "low") ? qrcodegen_Ecc_LOW : !strcmp(e, "quartile") ? qrcodegen_Ecc_QUARTILE
                             : !strcmp(e, "high") ? qrcodegen_Ecc_HIGH : qrcodegen_Ecc_MEDIUM;
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  int ok = qrcodegen_encodeText(argv[1], tmp, qr, level, qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, 1);
  if (!ok) ok = qrcodegen_encodeText(argv[1], tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, 0);
  if (!ok) { printf("{\"error\":\"too long for the card\"}"); return 0; }
  int n = qrcodegen_getSize(qr);
  printf("{\"size\":%d,\"modules\":\"", n);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      putchar(qrcodegen_getModule(qr, x, y) ? '1' : '0');
  printf("\"}");
  return 0;
}
"""


def qr_codes(config):
    """{"ecc|text": answer} for every qr page of the configuration."""
    pages = [p for p in config.get("pages", []) if p.get("type") == "qr" and p.get("text")]
    if not pages:
        return {}
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = pathlib.Path(tmp) / "qr.c", pathlib.Path(tmp) / "qr"
        src.write_text(QR_C)
        qrc = ROOT / "components" / "qrcodegen"
        subprocess.run(["cc", "-O1", "-o", str(exe), str(src), str(qrc / "src" / "qrcodegen.c"), "-I", str(qrc / "include")],
                       check=True)
        out = {}
        for p in pages:
            ecc = p.get("ecc", "medium")
            out[ecc + "|" + p["text"]] = json.loads(subprocess.run([str(exe), p["text"], ecc], check=True,
                                                                   capture_output=True, text=True).stdout)
        return out


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "demo"
    out.mkdir(parents=True, exist_ok=True)
    page = (ROOT / "www" / "index.html").read_text(encoding="utf-8")
    head = page.index("<head>") + len("<head>")
    page = page[:head] + '\n<script src="mock.js"></script>' + page[head:]
    page = page.replace("<title>OHpanel</title>", "<title>OHpanel demo</title>", 1)
    (out / "index.html").write_text(page, encoding="utf-8")
    for src, name in [(ROOT / "demo" / "mock.js", "mock.js"), (ROOT / "demo" / "config.json", "config.json"),
                      (ROOT / "demo" / "items.json", "items.json"), (ROOT / "demo" / "api.json", "api.json"),
                      (ROOT / "www" / "layout-eu.json", "layout.json"),
                      (ROOT / "components" / "app_display" / "src" / "icon_set.bin", "icon_set.bin")]:
        shutil.copyfile(src, out / name)
    config = json.loads((ROOT / "demo" / "config.json").read_text(encoding="utf-8"))
    (out / "qr.json").write_text(json.dumps(qr_codes(config)), encoding="utf-8")
    print(f"demo: {out}")


if __name__ == "__main__":
    main()

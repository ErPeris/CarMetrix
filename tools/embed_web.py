#!/usr/bin/env python3
"""
CarMetrix — embed_web.py
Comprime carmetrix/data/index.html (gzip) in un header C PROGMEM:
  carmetrix/src/web_index.h

Eseguire DOPO ogni modifica all'HTML e PRIMA di compilare in Arduino IDE,
così la web UI viene inclusa nel firmware e si aggiorna via OTA.

Uso:  python tools/embed_web.py
"""
import gzip, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "carmetrix", "data", "index.html")
OUT  = os.path.join(ROOT, "carmetrix", "src", "web_index.h")

with open(SRC, "rb") as f:
    raw = f.read()
gz = gzip.compress(raw, 9)

lines = [
    "#pragma once",
    "// ============================================================",
    "//  AUTO-GENERATO da tools/embed_web.py — NON modificare a mano.",
    "//  Rigenerare con:  python tools/embed_web.py",
    "//  (dopo aver modificato carmetrix/data/index.html)",
    "// ============================================================",
    f"// HTML: {len(raw)} byte  ->  gzip: {len(gz)} byte",
    "",
    "const unsigned char INDEX_HTML_GZ[] PROGMEM = {",
]
for i in range(0, len(gz), 16):
    chunk = gz[i:i + 16]
    lines.append("  " + ",".join(f"0x{b:02x}" for b in chunk) + ",")
lines.append("};")
lines.append(f"const unsigned int INDEX_HTML_GZ_LEN = {len(gz)};")
lines.append("")

with open(OUT, "w", newline="\n") as f:
    f.write("\n".join(lines))

print(f"OK: index.html {len(raw)} B  ->  web_index.h (gzip {len(gz)} B)")

"""Evaluate FFmpeg's per-library Makefiles against the configured config.mak and list the C
sources a minimal build needs (what `make` would compile). Reads only; prints paths."""
import re
import sys
from pathlib import Path

src = Path(sys.argv[1])      # FFmpeg source root
build = Path(sys.argv[2])    # configure output dir
cfg = {}
for line in (build / "ffbuild" / "config.mak").read_text().splitlines():
    m = re.match(r"^(!?)([A-Z0-9_]+)=(.*)$", line)
    if m:
        cfg[m.group(2)] = "" if m.group(1) else m.group(3).strip()

def expand(text):
    return re.sub(r"\$\(([A-Z0-9_]+)\)", lambda m: cfg.get(m.group(1), ""), text)

def objs_for(lib):
    files = []
    for mk in [src / lib / "Makefile", src / lib / "x86" / "Makefile"]:
        if not mk.exists():
            continue
        text = mk.read_text().replace("\\\n", " ")
        for line in text.splitlines():
            m = re.match(r"^\s*(OBJS|OBJS-\$\([A-Z0-9_]+\)|SHLIBOBJS[^ ]*|STLIBOBJS[^ ]*)\s*\+?=\s*(.*)$", line)
            if not m:
                continue
            var, rhs = m.group(1), m.group(2)
            if var.startswith("SHLIB"):
                continue
            if "-$(" in var:                      # OBJS-$(CONFIG_X) / STLIBOBJS-$(CONFIG_X)
                key = var[var.index("-$(") + 3:-1]
                if cfg.get(key, "") != "yes":
                    continue
            for o in rhs.split():
                if o.endswith(".o"):
                    rel = o[:-2]
                    base = (src / lib / rel)
                    for ext in (".c",):
                        if base.with_suffix(ext).exists():
                            files.append(f"{lib}/{rel}{ext}")
    return sorted(set(files))

for lib in ("libavutil", "libavcodec"):
    for f in objs_for(lib):
        print(f)

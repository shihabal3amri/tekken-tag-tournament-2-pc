"""Evaluate FFmpeg's per-library Makefiles against the configured config.mak and list the C
sources a minimal build needs (what `make` would compile). Reads only; prints paths."""
import re
import sys
from pathlib import Path

src = Path(sys.argv[1])      # FFmpeg source root
build = Path(sys.argv[2])    # configure output dir
cfg = {}
for line in (build / "ffbuild" / "config.mak").read_text().splitlines():
    m = re.match(r"^(!?[A-Z0-9_]+)=(.*)$", line)
    if m:
        cfg[m.group(1)] = m.group(2).strip()

# --disable-asm makes FFmpeg's ARCH variable "c" even for x86-64. Use the
# configure command recorded in config.mak to reject an accidental Win64 or
# other-host configuration before selecting platform-specific sources.
configuration = cfg.get("FFMPEG_CONFIGURATION", "")
# The native hosts: Linux x86-64, and macOS arm64 and x86-64 (mac/build_ffmpeg.sh, also --disable-asm).
native_hosts = (("--target-os=linux", "--arch=x86_64"),
                ("--target-os=darwin", "--arch=aarch64"),
                ("--target-os=darwin", "--arch=x86_64"))
def has_option(option):
    return re.search(r"(?:^|\s)" + re.escape(option) + r"(?:\s|$)", configuration)
if not any(all(has_option(o) for o in host) for host in native_hosts):
    raise SystemExit("native FFmpeg configuration is not one of: " +
                     ", ".join(" ".join(host) for host in native_hosts))

make_subdirs = []
for key in ("ARCH", "INTRINSICS"):
    name = cfg.get(key, "")
    if not re.fullmatch(r"[A-Za-z0-9_]+", name):
        raise SystemExit(f"native FFmpeg configuration has invalid {key}")
    if name not in make_subdirs:
        make_subdirs.append(name)

required_features = (
    "CONFIG_WMAPRO_DECODER",
    "CONFIG_XMA1_DECODER",
    "CONFIG_XMA2_DECODER",
    "CONFIG_XMA_PARSER",
)
for feature in required_features:
    if cfg.get(feature) != "yes":
        raise SystemExit(f"native FFmpeg configuration is missing {feature}")

def objs_for(lib):
    files = []
    makefiles = [src / lib / "Makefile"] + [src / lib / name / "Makefile"
                                             for name in make_subdirs]
    for mk in makefiles:
        if not mk.exists():
            continue
        text = mk.read_text().replace("\\\n", " ")
        for line in text.splitlines():
            m = re.match(r"^\s*(OBJS|OBJS-\$\(!?[A-Z0-9_]+\)|SHLIBOBJS[^ ]*|STLIBOBJS[^ ]*)\s*\+?=\s*(.*)$", line)
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
                    if not base.with_suffix(".c").exists():
                        raise SystemExit(f"unsupported FFmpeg object without a C source: {lib}/{o} ({mk})")
                    files.append(f"{lib}/{rel}.c")
                elif "$" in o and o != "$(COMPAT_OBJS:%=../compat/%)":
                    raise SystemExit(f"unsupported FFmpeg object expression: {o} ({mk})")
    if lib == "libavutil":
        # Its Makefile also appends $(COMPAT_OBJS:%=../compat/%). These are
        # configure-selected C sources outside libavutil's own directory.
        for obj in cfg.get("COMPAT_OBJS", "").split():
            if not obj.endswith(".o") or not (src / "compat" / obj[:-2]).with_suffix(".c").exists():
                raise SystemExit(f"unsupported FFmpeg compatibility object: {obj}")
            files.append(f"compat/{obj[:-2]}.c")
    return sorted(set(files))

selected = {lib: objs_for(lib) for lib in ("libavutil", "libavcodec")}
required_sources = {"libavcodec/wmaprodec.c", "libavcodec/xma_parser.c"}
missing_sources = required_sources - set(selected["libavcodec"])
if missing_sources:
    raise SystemExit("native FFmpeg source list is missing " +
                     ", ".join(sorted(missing_sources)))
for lib in ("libavutil", "libavcodec"):
    for f in selected[lib]:
        print(f)

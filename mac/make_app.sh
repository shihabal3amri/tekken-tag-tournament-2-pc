#!/usr/bin/env bash
# Build "mac/out/Tekken Tag Tournament 2.app" from the game executable(s).
#   mac/make_app.sh [--binary PATH] [--binary-x86 PATH] [--out PATH] [--no-data] [--no-sign]
#                   [--release] [--identity NAME] [--notarize PROFILE]
#     --binary      the arm64 game executable (default mac/build/game/tekken_recomp_mac)
#     --binary-x86  an x86_64 game executable too: one universal app (each slice with its own dylibs,
#                   joined with lipo; a file both slices share, like Valve's universal libsteam_api, once)
#     --out         the .app to make (default "mac/out/Tekken Tag Tournament 2.app"); replaced each run
#     --no-data     leave the game data out (default: mac/out/game is cloned into Contents/Resources/game;
#                   APFS clones cost no space; a symlink would break the bundle's code signature)
#     --no-sign     skip the signature
#     --release     what players download: no game data (the app imports the player's disc and TU3 on first
#                   launch, apple_import.mm), signed with --identity under the hardened runtime, and every
#                   slice checked against its deployment target (arm64 14.4, x86_64 13.0). Then
#                   mac/out/TTT2-macOS-<version>.zip.
#     --identity    signing identity for --release (default: the first "Developer ID Application")
#     --notarize    notarytool keychain profile: submit the zip, staple the app, zip it again
# Every non-system dylib the executable needs (DXVK d3d11/dxgi, vkd3d-shader, SDL3, OpenSSL, the
# Vulkan loader, recursively) is copied from mac/deps/install-<its arch> into Contents/Frameworks with
# an @rpath install name, the executable gets @executable_path/../Frameworks as its only rpath, and
# MoltenVK (Khronos' universal release, from mac/build_libs.sh; MOLTENVK_DYLIB/MOLTENVK_ICD override)
# goes in with an ICD manifest in Contents/Resources/vulkan/icd.d, where the Vulkan loader looks first.
# Nothing may point into /opt/homebrew, /usr/local or this checkout afterwards: the script checks with
# otool and fails otherwise, so the bundle runs on a Mac without Homebrew.
set -euo pipefail
mac=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$mac")
binary=$mac/build/game/tekken_recomp_mac
binary_x86=
app="$mac/out/Tekken Tag Tournament 2.app"
data=1
sign=1
release=0
identity=
profile=
while [[ $# -gt 0 ]]; do
    case $1 in
        --binary) binary=$2; shift 2 ;;
        --binary-x86) binary_x86=$2; shift 2 ;;
        --out) app=$2; shift 2 ;;
        --no-data) data=0; shift ;;
        --no-sign) sign=0; shift ;;
        --release) release=1; data=0; shift ;;
        --identity) identity=$2; shift 2 ;;
        --notarize) profile=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[[ -x $binary ]] || { echo "no game executable at $binary" >&2; exit 1; }
[[ -z $binary_x86 || -x $binary_x86 ]] || { echo "no x86_64 game executable at $binary_x86" >&2; exit 1; }
[[ -z $profile || $release == 1 ]] || { echo "--notarize needs --release" >&2; exit 2; }
# MoltenVK: Khronos' universal release build from mac/build_libs.sh (either ARCH's copy serves both).
moltenvk_dir=$mac/deps/install-$(lipo -archs "$binary" | cut -d' ' -f1)/moltenvk/lib
moltenvk=${MOLTENVK_DYLIB:-$moltenvk_dir/libMoltenVK.dylib}
moltenvk_icd=${MOLTENVK_ICD:-$moltenvk_dir/MoltenVK_icd.json}
[[ -f $moltenvk && -f $moltenvk_icd ]] || { echo "MoltenVK not found ($moltenvk, $moltenvk_icd): run mac/build_libs.sh" >&2; exit 1; }
version=$(sed -n 's/^set(TTT2_VERSION "\(.*\)")$/\1/p' "$repo/port/CMakeLists.txt")
exe_name=tekken_recomp_mac

# bundle BINARY APP: one slice's .app (executable, its dylibs, MoltenVK, Info.plist, icon).
bundle() {
local binary=$1 app=$2
local contents="$app/Contents"
rm -rf "$app"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/vulkan/icd.d"
cp "$binary" "$contents/MacOS/$exe_name"
chmod 755 "$contents/MacOS/$exe_name"
cp "$moltenvk" "$contents/Frameworks/libMoltenVK.dylib"
chmod 644 "$contents/Frameworks/libMoltenVK.dylib"

# Bundle the dylibs, rewrite install names and rpaths, write the ICD manifest, Info.plist and icon.
python3 -I - "$contents" "$exe_name" "$binary" "$moltenvk_icd" "$repo" <<'PY'
import json, os, plistlib, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

contents, exe_name, original_exe, icd_src, repo = sys.argv[1:6]
contents = Path(contents)
frameworks = contents / "Frameworks"
exe = contents / "MacOS" / exe_name
# This slice's libraries: mac/build_libs.sh, build_vkd3d.sh, build_dxvk.sh for the executable's ARCH.
slice_arch = subprocess.run(["lipo", "-archs", original_exe], check=True, capture_output=True,
                            text=True).stdout.split()[0]
search = sorted(Path(repo, f"mac/deps/install-{slice_arch}").glob("*/lib"))

def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout

def deps(path):
    # A universal file (Valve's libsteam_api.dylib) lists its loads once per architecture,
    # each under a "<file> (architecture X):" header line.
    lines = [l for l in run("otool", "-L", str(path)).splitlines()[1:] if not l.rstrip().endswith(":")]
    out = list(dict.fromkeys(l.strip().split(" (compatibility")[0] for l in lines))
    if path.suffix == ".dylib" and out:      # a dylib lists its own id first
        own = run("otool", "-D", str(path)).splitlines()[1:]
        out = [d for d in out if not own or d != own[0].strip()]
    return out

def rpaths(path):
    text = run("otool", "-l", str(path))
    return re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (.+?) \(offset", text)

def system(dep):
    return dep.startswith("/usr/lib/") or dep.startswith("/System/")

def resolve(dep, source):
    """The file a load command names, for a binary that lived at `source` before bundling."""
    leaf = dep.split("/")[-1]
    origin = Path(source).parent
    if dep.startswith("@loader_path/"):
        return (origin / dep[len("@loader_path/"):]).resolve()
    if dep.startswith("@executable_path/"):
        return (Path(original_exe).parent / dep[len("@executable_path/"):]).resolve()
    if dep.startswith("@rpath/"):
        rest = dep[len("@rpath/"):]
        for rp in rpaths(Path(source)):
            rp = rp.replace("@loader_path", str(origin)).replace("@executable_path", str(Path(original_exe).parent))
            if (Path(rp) / rest).exists():
                return (Path(rp) / rest).resolve()
        for d in search:
            if (d / rest).exists():
                return (d / rest).resolve()
        raise SystemExit(f"cannot resolve {dep} for {source}")
    if Path(dep).exists():
        return Path(dep).resolve()
    raise SystemExit(f"missing dependency {dep} of {source}")

# bundled leaf name -> original file; work list of (bundled path, original path)
bundled = {}
work = [(exe, Path(original_exe).resolve()), (frameworks / "libMoltenVK.dylib", None)]
while work:
    path, source = work.pop()
    for dep in deps(path):
        if system(dep):
            continue
        leaf = dep.split("/")[-1]
        if dep.startswith("@rpath/") and (frameworks / leaf).exists() and leaf in bundled:
            pass
        elif leaf not in bundled:
            if source is None:
                raise SystemExit(f"{path.name} needs {dep}")
            real = resolve(dep, source)
            target = frameworks / leaf
            shutil.copyfile(real, target)
            os.chmod(target, 0o644)
            bundled[leaf] = real
            work.append((target, real))
        if dep != f"@rpath/{leaf}":
            run("install_name_tool", "-change", dep, f"@rpath/{leaf}", str(path))

for lib in frameworks.glob("*.dylib"):
    run("install_name_tool", "-id", f"@rpath/{lib.name}", str(lib))
    for rp in rpaths(lib):
        run("install_name_tool", "-delete_rpath", rp, str(lib))
    run("install_name_tool", "-add_rpath", "@loader_path", str(lib))
for rp in rpaths(exe):
    run("install_name_tool", "-delete_rpath", rp, str(exe))
run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(exe))

# Check: nothing outside the system and the bundle.
bad = []
minos = (11, 0)
for macho in [exe] + sorted(frameworks.glob("*.dylib")):
    for dep in deps(macho):
        if not (system(dep) or dep.startswith("@rpath/")):
            bad.append(f"{macho.name}: {dep}")
    for rp in rpaths(macho):
        if not rp.startswith("@"):
            bad.append(f"{macho.name}: rpath {rp}")
    m = re.search(r"minos (\d+)\.(\d+)", run("otool", "-l", str(macho)))
    if m:
        minos = max(minos, (int(m.group(1)), int(m.group(2))))
if bad:
    raise SystemExit("non-bundled references left:\n  " + "\n  ".join(bad))

# MoltenVK as the bundle's Vulkan driver (path relative to the manifest).
icd = json.loads(Path(icd_src).read_text())
icd["ICD"]["library_path"] = "../../../Frameworks/libMoltenVK.dylib"
(contents / "Resources/vulkan/icd.d/MoltenVK_icd.json").write_text(json.dumps(icd, indent=4) + "\n")

# Icon: the port's own ttt2.ico holds PNGs (16 to 256 px); iconutil takes them as an iconset.
ico = Path(repo, "port/src/setup/ttt2.ico").read_bytes()
pngs = {}
for i in range(struct.unpack("<H", ico[4:6])[0]):
    w, h, _, _, _, _, size, offset = struct.unpack("<BBBBHHII", ico[6 + 16 * i:22 + 16 * i])
    blob = ico[offset:offset + size]
    if blob.startswith(b"\x89PNG"):
        pngs[w or 256] = blob
names = {16: ["icon_16x16"], 32: ["icon_16x16@2x", "icon_32x32"], 64: ["icon_32x32@2x"],
         128: ["icon_128x128"], 256: ["icon_128x128@2x", "icon_256x256"]}
with tempfile.TemporaryDirectory() as tmp:
    iconset = Path(tmp, "ttt2.iconset")
    iconset.mkdir()
    for size, files in names.items():
        for name in files:
            if size in pngs:
                (iconset / f"{name}.png").write_bytes(pngs[size])
    run("iconutil", "-c", "icns", "-o", str(contents / "Resources/ttt2.icns"), str(iconset))

plist = {
    "CFBundleDevelopmentRegion": "en",
    "CFBundleDisplayName": "Tekken Tag Tournament 2",
    "CFBundleName": "TTT2",
    "CFBundleExecutable": exe_name,
    "CFBundleIdentifier": "io.github.shihabal3amri.ttt2",
    "CFBundleIconFile": "ttt2",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "LSApplicationCategoryType": "public.app-category.fighting-games",
    # The highest minos of any Mach-O in this slice (Homebrew bottles carry the macOS they were built on).
    "LSMinimumSystemVersion": f"{minos[0]}.{minos[1]}",
    "NSHighResolutionCapable": True,
    "NSSupportsAutomaticGraphicsSwitching": True,
    "GCSupportsControllerUserInteraction": True,
    "GCSupportedGameControllers": [{"ProfileName": "ExtendedGamepad"}],
    # Launched from Finder or `open`: the bundled MoltenVK only (a Homebrew one would be a second
    # identical adapter), and DXVK's SDL3 window system.
    "LSEnvironment": {"VK_LOADER_SEARCH_ONLY_IN_BUNDLE": "1", "DXVK_WSI_DRIVER": "SDL3"},
}
with open(contents / "Info.plist", "wb") as f:
    plistlib.dump(plist, f)
(contents / "PkgInfo").write_text("APPL????")
print(f"bundled: {', '.join(sorted(bundled))}, libMoltenVK.dylib; minimum macOS {minos[0]}.{minos[1]}")
PY
}

if [[ -z $binary_x86 ]]; then
    bundle "$binary" "$app"
else
    # Each slice bundled on its own, then joined: the executable and every dylib with lipo (a file that is
    # already universal, or identical in both, is taken once), the rest from the arm64 bundle.
    work=$(mktemp -d "${TMPDIR:-/tmp}/ttt2-app.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    bundle "$binary" "$work/arm64.app"
    bundle "$binary_x86" "$work/x86_64.app"
    rm -rf "$app"
    cp -R "$work/arm64.app" "$app"
    for slice in "$work/x86_64.app/Contents/MacOS/$exe_name" "$work/x86_64.app/Contents/Frameworks/"*.dylib; do
        rel=${slice#"$work/x86_64.app/"}
        if [[ ! -e "$app/$rel" ]]; then cp "$slice" "$app/$rel"
        elif cmp -s "$slice" "$app/$rel" || [[ " $(lipo -archs "$app/$rel") " == *" x86_64 "* ]]; then :
        else lipo -create "$work/arm64.app/$rel" "$slice" -output "$app/$rel"
        fi
    done
    (cd "$app" && for f in "Contents/MacOS/$exe_name" Contents/Frameworks/*.dylib; do
        [[ " $(lipo -archs "$f") " == *" arm64 "* && " $(lipo -archs "$f") " == *" x86_64 "* ]] ||
            { echo "$f is not universal" >&2; exit 1; }
    done)
    # Each slice keeps its own floor (os_sync_wait_on_address needs 14.4 on arm64; x86_64 runs on 13).
    arm=$(/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$work/arm64.app/Contents/Info.plist")
    x86=$(/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$work/x86_64.app/Contents/Info.plist")
    plist="$app/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $x86" \
        -c "Add :LSMinimumSystemVersionByArchitecture dict" \
        -c "Add :LSMinimumSystemVersionByArchitecture:arm64 string $arm" \
        -c "Add :LSMinimumSystemVersionByArchitecture:x86_64 string $x86" "$plist"
fi
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $version" "$app/Contents/Info.plist"

if [[ $release == 1 ]]; then
    # Every Mach-O must load on its slice's floor, not on the Mac that built it (Homebrew bottles are built
    # for the newest macOS).
    bad=0
    while IFS= read -r f; do
        for arch in $(lipo -archs "$f"); do
            floor=13.0; [[ $arch == arm64 ]] && floor=14.4
            minos=$(otool -arch "$arch" -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')
            if [[ -n $minos ]] && [[ $(printf '%s\n%s\n' "$floor" "$minos" | sort -V | tail -1) != "$floor" ]]; then
                echo "$(basename "$f") ($arch) needs macOS $minos, above $floor" >&2; bad=1
            fi
        done
    done < <(find "$app/Contents/MacOS" "$app/Contents/Frameworks" -type f)
    [[ $bad == 0 ]] || { echo "rebuild those dependencies with a lower deployment target" >&2; exit 1; }
fi

if [[ $data == 1 ]]; then
    if [[ -f $mac/out/game/tekken.img ]]; then
        cp -Rc "$mac/out/game" "$app/Contents/Resources/game"
        echo "game data cloned from mac/out/game"
    else
        echo "warning: no mac/out/game/tekken.img; the app has no game data" >&2
    fi
fi

if [[ $release == 1 ]]; then
    if [[ -z $identity ]]; then
        identity=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)
        [[ -n $identity ]] || { echo "no Developer ID Application identity in the keychain" >&2; exit 1; }
    fi
    # Inside out, hardened runtime, secure timestamp. The app's one entitlement turns off library validation:
    # libsteam_api loads the Steam app's steamclient.dylib, signed by Valve (mac/entitlements.plist).
    for f in "$app/Contents/Frameworks/"*.dylib; do
        codesign --force --timestamp --options runtime -s "$identity" "$f"
    done
    codesign --force --timestamp --options runtime --entitlements "$mac/entitlements.plist" -s "$identity" "$app"
    codesign --verify --deep --strict "$app"
    zip="$mac/out/TTT2-macOS-$version.zip"
    rm -f "$zip"
    ditto -c -k --keepParent "$app" "$zip"
    if [[ -n $profile ]]; then
        xcrun notarytool submit "$zip" --keychain-profile "$profile" --wait
        xcrun stapler staple "$app"
        rm -f "$zip"
        ditto -c -k --keepParent "$app" "$zip"
        spctl -a -vv -t exec "$app"
    fi
    echo "$zip"
elif [[ $sign == 1 ]]; then
    codesign --force --deep -s - "$app"
    codesign --verify --deep --strict "$app"
fi
echo "$app"

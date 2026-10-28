#!/usr/bin/env bash
# DXVK-native 1.9.2b for macOS (DXVK -> Vulkan -> MoltenVK), with shihab's patch set
# (tools/patches/dxvk-native.json, same order as tools/prepare_dxvk_native.py) and then
# the Mac patches in mac/patches/dxvk-*-macos.patch (sorted by name).
#
#   mac/build_dxvk.sh                      arm64, macOS 14.4
#   ARCH=x86_64 mac/build_dxvk.sh          the Intel slice, macOS 13.0 (native on an Intel Mac,
#                                          cross from arm64)
# Links mac/build_libs.sh's SDL3 and Vulkan loader for the same ARCH.
# Output prefix (default mac/deps/install-$ARCH/dxvk):
#   lib/libdxvk_d3d11.dylib, lib/libdxvk_dxgi.dylib   (install names @rpath/..., rpath @loader_path)
#   include/dxvk/{directx,windows,wsi}                the DXVK-native Windows/DirectX ABI headers
set -euo pipefail

mac_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$mac_root/.." && pwd)
arch=${ARCH:-arm64}
host_arch=$(uname -m)
src=$mac_root/deps/dxvk-native-src
build=$mac_root/build/renderer/dxvk-$arch
prefix=${TTT2_DXVK_PREFIX:-$mac_root/deps/install-$arch/dxvk}
libdir=${TTT2_MAC_LIBDIR:-$mac_root/deps/install-$arch/sdl3/lib}
incdir=${TTT2_MAC_INCDIR:-$mac_root/deps/install-$arch/sdl3/include}
link_dirs=("-L$libdir" "-L$mac_root/deps/install-$arch/vulkan/lib")
default_minos=13.0; [[ $arch == arm64 ]] && default_minos=14.4
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-$default_minos}
jobs=${TTT2_BUILD_JOBS:-$(sysctl -n hw.ncpu)}
manifest=$repo/tools/patches/dxvk-native.json

read -r url tag commit < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["repository"], m["tag"], m["commit"])' "$manifest")
patches=()
while IFS= read -r p; do patches+=("$repo/tools/patches/$p"); done < <(
    python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["patches"]))' "$manifest")
for p in "$mac_root"/patches/dxvk-*-macos.patch; do [[ -e $p ]] && patches+=("$p"); done

# The stamp says which patch set the tree holds; a different set re-creates the tree.
signature=$(cat "$manifest" "${patches[@]}" | shasum -a 256 | cut -d' ' -f1)
stamp=$src/.ttt2-mac-patches
if [[ ! -d $src/.git ]]; then
    git clone --branch "$tag" "$url" "$src"
fi
if [[ ! -f $stamp || $(cat "$stamp") != "$signature" ]]; then
    git -C "$src" checkout -q -f --detach "$commit"
    git -C "$src" reset -q --hard "$commit"
    git -C "$src" clean -q -fdx -e .ttt2-mac-patches
    # shihab's patches go into the index, the Mac patches only into the work tree, so that
    # `git -C mac/deps/dxvk-native-src diff` is always exactly the Mac patches together
    # (git diff -- <files> splits them: the async patch owns dxvk_context/graphics/image/
    # options/pipecompiler/pipemanager and src/dxvk/meson.build, the MoltenVK patch the rest).
    for p in "${patches[@]}"; do
        case $p in
            "$mac_root"/patches/*) index=() ;;
            *) index=(--index) ;;
        esac
        git -C "$src" apply ${index[@]+"${index[@]}"} --ignore-whitespace "$p" || { echo "patch failed: $p" >&2; exit 1; }
    done
    echo "$signature" > "$stamp"
fi

meson_args=(--buildtype release --prefix "$prefix" --libdir lib
    -Denable_tests=false -Denable_d3d9=false -Denable_d3d10=false -Ddxvk_native_wsi=sdl3
    "-Dcpp_args=-isystem$incdir" "-Dcpp_link_args=${link_dirs[*]} -Wl,-rpath,@loader_path")
if [[ $arch != "$host_arch" ]]; then
    cpu_family=$arch; [[ $arch == arm64 ]] && cpu_family=aarch64
    cross=$mac_root/build/renderer/macos-$arch.cross
    mkdir -p "$(dirname "$cross")"
    cat > "$cross" <<EOF
[binaries]
c = ['clang', '-arch', '$arch']
cpp = ['clang++', '-arch', '$arch']
ar = 'ar'
strip = 'strip'
[host_machine]
system = 'darwin'
cpu_family = '$cpu_family'
cpu = '$arch'
endian = 'little'
EOF
    meson_args+=(--cross-file "$cross" -Ddxvk_native_force=true)
fi

if [[ -f $build/meson-private/coredata.dat ]]; then
    meson setup "$build" "$src" --reconfigure "${meson_args[@]}" > "$build.meson.log" 2>&1 \
        || { tail -20 "$build.meson.log"; exit 1; }
else
    mkdir -p "$(dirname "$build")"
    meson setup "$build" "$src" "${meson_args[@]}" > "$build.meson.log" 2>&1 \
        || { tail -20 "$build.meson.log"; exit 1; }
fi
ninja -C "$build" -j "$jobs"
meson install -C "$build" > /dev/null

# Headers the game includes (port/CMakeLists.txt: TTT2_DXVK_NATIVE_INCLUDE and its directx/, windows/).
rm -rf "$prefix/include/dxvk"
mkdir -p "$prefix/include/dxvk"
cp -R "$src/include/native/." "$prefix/include/dxvk/"
printf 'DXVK-native (%s): %s/lib/libdxvk_d3d11.dylib, libdxvk_dxgi.dylib\n' "$arch" "$prefix"

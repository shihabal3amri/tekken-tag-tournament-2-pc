#!/bin/bash
# release.sh <version>: the server's release archives in server/dist/<version>/ - Windows (zip), Linux x86-64 and
# ARM64 (tar.gz), each with the program, HOSTING.md, README.md, PROTOCOL.md, THIRD_PARTY_NOTICES.txt and the admin
# command wrapper (ttt2admin.cmd; on Linux ttt2admin, executable, and the systemd unit), plus SHA256SUMS. Static
# builds (CGO_ENABLED=0), the version stamped into the program (it logs it at start).
# Needs Go (PATH, or tools/go of this project) and Python (tools/pack.py: archives with the right file modes).
set -euo pipefail
VERSION=${1:?usage: release.sh <version>}
cd "$(dirname "$0")/.."
ROOT=$(pwd)
GO=go
command -v go >/dev/null 2>&1 || GO="$ROOT/../tools/go/bin/go"
export CGO_ENABLED=0 GOFLAGS=-trimpath
OUT="$ROOT/dist/$VERSION"
rm -rf "$OUT"
mkdir -p "$OUT"
"$GO" vet ./...
"$GO" test -short ./...
for target in windows/amd64 linux/amd64 linux/arm64; do
  os=${target%/*}
  arch=${target#*/}
  name="ttt2-server-$VERSION-$os-$arch"
  stage="$OUT/$name"
  mkdir -p "$stage"
  exe=ttt2-server
  [ "$os" = windows ] && exe=ttt2-server.exe
  GOOS=$os GOARCH=$arch "$GO" build -ldflags "-s -w -X main.version=$VERSION" -o "$stage/$exe" ./cmd/ttt2-server
  cp HOSTING.md README.md PROTOCOL.md THIRD_PARTY_NOTICES.txt "$stage/"
  if [ "$os" = linux ]; then
    cp deploy/ttt2-server.service "$stage/"
    tr -d '\r' < deploy/ttt2admin > "$stage/ttt2admin" # LF only: a shell script with CR line ends does not run
  else
    sed 's/\r*$/\r/' deploy/ttt2admin.cmd > "$stage/ttt2admin.cmd" # CRLF, as cmd.exe expects
  fi
  kind=tar.gz
  [ "$os" = windows ] && kind=zip
  python tools/pack.py "$stage" "$kind"
  rm -rf "$stage"
done
(cd "$OUT" && sha256sum *.zip *.tar.gz > SHA256SUMS)
cat "$OUT/SHA256SUMS"

#!/bin/sh
# kolma installer: downloads the latest release binary for this platform,
# verifies its checksum, and installs kolma + kolma-tui.
#
#   curl -fsSL https://raw.githubusercontent.com/LangerSword/kolma/master/scripts/install.sh | sh
#
# Environment:
#   KOLMA_VERSION   pin a version (default: latest release)
#   KOLMA_PREFIX    install prefix (default: ~/.local, or /usr/local if writable)
set -eu

REPO="LangerSword/kolma"

os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)
case "$os" in
  linux) os=linux ;;
  darwin) os=darwin ;;
  *) echo "kolma: unsupported OS '$os' (Linux and macOS only)" >&2; exit 1 ;;
esac
case "$arch" in
  x86_64|amd64) arch=amd64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) echo "kolma: unsupported architecture '$arch'" >&2; exit 1 ;;
esac

if [ -n "${KOLMA_VERSION:-}" ]; then
  tag="v${KOLMA_VERSION#v}"
  base="https://github.com/$REPO/releases/download/$tag"
else
  tag=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name"' | head -1 | cut -d'"' -f4)
  [ -n "$tag" ] || { echo "kolma: could not determine the latest release" >&2; exit 1; }
  base="https://github.com/$REPO/releases/download/$tag"
fi

name="kolma-$os-$arch.tar.gz"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "kolma: downloading $name ($tag)"
curl -fsSL "$base/$name" -o "$tmp/$name"
curl -fsSL "$base/checksums.txt" -o "$tmp/checksums.txt"

expected=$(grep " $name\$" "$tmp/checksums.txt" | awk '{print $1}')
[ -n "$expected" ] || { echo "kolma: no checksum for $name" >&2; exit 1; }
if command -v sha256sum >/dev/null 2>&1; then
  actual=$(sha256sum "$tmp/$name" | awk '{print $1}')
else
  actual=$(shasum -a 256 "$tmp/$name" | awk '{print $1}')
fi
[ "$expected" = "$actual" ] || { echo "kolma: CHECKSUM MISMATCH — refusing to install" >&2; exit 1; }

tar -xzf "$tmp/$name" -C "$tmp" --strip-components=1

prefix="${KOLMA_PREFIX:-}"
if [ -z "$prefix" ]; then
  if [ -w /usr/local/bin ] 2>/dev/null; then
    prefix=/usr/local
  else
    prefix="$HOME/.local"
  fi
fi
mkdir -p "$prefix/bin"
install -m 755 "$tmp/kolma" "$prefix/bin/kolma"
install -m 755 "$tmp/kolma-tui" "$prefix/bin/kolma-tui"

echo "kolma: installed $tag to $prefix/bin"
case ":$PATH:" in
  *":$prefix/bin:"*) ;;
  *) echo "kolma: note — $prefix/bin is not in your PATH" ;;
esac
"$prefix/bin/kolma" version

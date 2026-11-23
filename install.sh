#!/bin/sh
# Installs jlang on Linux or macOS:
#
#   curl -fsSL https://jafarniaz.com/jlang/install.sh | sh
#
# It downloads the prebuilt binary for this computer from the latest GitHub
# release, or builds JLang from source if there is none, and puts jlang in
# ~/.local/bin (or $JLANG_INSTALL_DIR). Nothing needs sudo.
set -eu

REPO="jafarnz/JLang"
DIR="${JLANG_INSTALL_DIR:-$HOME/.local/bin}"

say() { printf '%s\n' "$*"; }
fail() { say "jlang install: $*" >&2; exit 1; }

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) fail "this script is for Linux and macOS. On Windows, download jlang-windows-x86_64.zip from https://github.com/$REPO/releases, or run: npx jlang-cli" ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) arch=x86_64 ;;
    arm64 | aarch64) arch=arm64 ;;
    *) arch="" ;;
esac

if command -v curl >/dev/null 2>&1; then
    get() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    get() { wget -q "$1" -O "$2"; }
else
    fail "needs curl or wget"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$DIR"

name="jlang-$os-$arch"
if [ -n "$arch" ] && get "https://github.com/$REPO/releases/latest/download/$name.tar.gz" "$tmp/jlang.tar.gz" 2>/dev/null; then
    say "Downloaded $name."
    tar xzf "$tmp/jlang.tar.gz" -C "$tmp"
    install -m 755 "$tmp/$name/jlang" "$DIR/jlang"
else
    say "No prebuilt jlang for $os $(uname -m); building it from source."
    command -v make >/dev/null 2>&1 || fail "building needs make and a C compiler"
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || fail "building needs a C compiler (cc or gcc)"
    get "https://github.com/$REPO/archive/refs/heads/main.tar.gz" "$tmp/src.tar.gz" || fail "couldn't download the source"
    tar xzf "$tmp/src.tar.gz" -C "$tmp"
    (cd "$tmp"/JLang-main && make >/dev/null) || fail "the build failed"
    install -m 755 "$tmp"/JLang-main/build/jlang "$DIR/jlang"
fi

say "Installed $("$DIR/jlang" version 2>/dev/null || echo jlang) to $DIR/jlang"
case ":$PATH:" in
    *":$DIR:"*) ;;
    *) say "Add $DIR to your PATH to run jlang from anywhere, for example:"
       say "  echo 'export PATH=\"$DIR:\$PATH\"' >> ~/.profile" ;;
esac
say "Try it: jlang new blinky && cd blinky && jlang run main.jlang"

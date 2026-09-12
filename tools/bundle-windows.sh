#!/usr/bin/env bash
# Turn a built receiver into a self-contained tree that runs on a clean Windows
# box with no MSYS2 installed.
#
#   tools/bundle-windows.sh [builddir] [outdir]     # from an MSYS2 UCRT64 shell
#
# The layout mirrors /ucrt64 on purpose: every layer (GLib, GTK, GIO,
# GStreamer) finds its data by walking up from bin/, so `bin` is load-bearing
# and the tree cannot be flattened.
#
#   <out>/bin/                    exe + every transitive DLL
#   <out>/lib/gstreamer-1.0/      plugins, including libgstgtk4.dll
#   <out>/lib/gio/modules/        libgioopenssl.dll — without it wss:// fails
#   <out>/libexec/gstreamer-1.0/  gst-plugin-scanner.exe
#   <out>/share/glib-2.0/schemas/ gschemas.compiled
set -euo pipefail

PREFIX=/ucrt64
BUILD=${1:-build}
OUT=${2:-dist/aircast}

rm -rf "$OUT"
mkdir -p "$OUT"/bin "$OUT"/lib/gstreamer-1.0 "$OUT"/lib/gio/modules \
         "$OUT"/libexec/gstreamer-1.0 "$OUT"/share/glib-2.0/schemas

cp "$BUILD/aircast-receiver.exe" "$OUT/bin/"
# Shipped for the CI smoke test, and for asking a user's machine what it has.
cp "$PREFIX/bin/gst-inspect-1.0.exe" "$OUT/bin/"
cp "$PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner.exe" "$OUT/libexec/gstreamer-1.0/"
cp "$PREFIX"/lib/gio/modules/libgioopenssl.dll "$OUT/lib/gio/modules/"
# ponytail: the whole plugin directory, not a curated list. Prune only if the
# installer turns out too big — measure first, a wrong guess here is a codec
# that works in CI and not on the user's machine.
cp "$PREFIX"/lib/gstreamer-1.0/*.dll "$OUT/lib/gstreamer-1.0/"

# ntldd -R prints three shapes per line:
#   "\tNAME (0xADDR)"            a system DLL, no path — skip
#   "\tNAME => PATH (0xADDR)"    resolved — take PATH when it is ours
#   "\tNAME => not found"        unresolved
#
# "not found" is mostly noise: api-ms-win-* and ext-ms-win-* are API sets, which
# are virtual names the loader resolves through a schema and which have no file
# to find, and the rest are optional system DLLs a given Windows edition may
# simply not have. Reporting them is useful; failing on them would mean this
# script never runs anywhere. The real gate is the smoke test, which loads every
# plugin with MSYS2 off the PATH.
closure () {
  ntldd -R "$1" 2>/dev/null | grep 'not found' \
    | grep -viE '(api|ext)-ms-win-' | sed "s|^|  unresolved under $1: |" >&2 || true

  ntldd -R "$1" 2>/dev/null | tr '\\' '/' \
    | sed -n 's/^[[:space:]]*[^ ]* => \(.*\) (0x[0-9a-fA-F]*)$/\1/p' \
    | grep -i '/ucrt64/'
}

# The exe's import table is not the whole story: plugins, GIO modules and the
# scanner are all LoadLibrary'd at runtime. Walk every one of them, or the
# pipeline dies on a machine that has nothing else installed.
{
  for f in "$OUT"/bin/*.exe "$OUT"/lib/gstreamer-1.0/*.dll \
           "$OUT"/lib/gio/modules/*.dll "$OUT"/libexec/gstreamer-1.0/*.exe; do
    closure "$f"
  done
} | sort -u | while read -r dll; do
  cp -n "$dll" "$OUT/bin/"
done

# The only mandatory data file: GTK reads settings through GSettings, and
# g_settings_new() aborts the process when the schema is missing.
glib-compile-schemas --targetdir="$OUT/share/glib-2.0/schemas" "$PREFIX/share/glib-2.0/schemas"

# Startup optimisation only — GIO loads every valid module in the directory
# when the cache is absent.
gio-querymodules "$OUT/lib/gio/modules" || true

du -sh "$OUT"
echo "bundled $(find "$OUT" -name '*.dll' | wc -l) DLLs into $OUT"

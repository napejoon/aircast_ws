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
#   <out>/share/icons/Adwaita/     the icon theme the toolbar draws from
set -euo pipefail

PREFIX=/ucrt64
BUILD=${1:-build}
OUT=${2:-dist/aircast}

rm -rf "$OUT"
mkdir -p "$OUT"/bin "$OUT"/lib/gstreamer-1.0 "$OUT"/lib/gio/modules \
         "$OUT"/libexec/gstreamer-1.0 "$OUT"/share/glib-2.0/schemas \
         "$OUT"/share/icons

cp "$BUILD/aircast-receiver.exe" "$OUT/bin/"
# Shipped for the CI smoke test, and for asking a user's machine what it has.
cp "$PREFIX/bin/gst-inspect-1.0.exe" "$OUT/bin/"
cp "$PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner.exe" "$OUT/libexec/gstreamer-1.0/"
cp "$PREFIX"/lib/gio/modules/libgioopenssl.dll "$OUT/lib/gio/modules/"
# A curated list, now that there is a measurement to justify one. The old
# comment here said to prune only after measuring, because a wrong guess is a
# codec that works in CI and not on the user's machine. The measurement arrived:
# main.c marks its own startup, and on the reported machine gst_init took 21.28
# of a 22.83 second launch. Everything else together -- the Direct3D probe,
# gtk_init, building and presenting the window -- was 1.55.
#
# That time is the 299 plugin files: after an install every one has a new mtime,
# so GStreamer rebuilds its registry and LoadLibrary's all of them, through a
# Defender that has never seen any of them before. After a reboot the registry
# is still valid but the files are cold and it stats all 299. The receiver names
# fifteen elements and webrtcbin makes about as many again. The rest was aws,
# deepgram, elevenlabs, spotify, decklink, ndi, x265, festival and two hundred
# and fifty others that this program cannot reach.
#
# What keeps this honest is the check below, not this list.
PLUGINS="
  coreelements typefindfunctions app
  webrtc nice dtls srtp sctp
  rtp rtpmanager
  videoparsersbad videoconvertscale matroska
  libav vpx
  d3d11
  gtk4
  opengl
"
for name in $PLUGINS; do
  src="$PREFIX/lib/gstreamer-1.0/libgst$name.dll"
  if [ -f "$src" ]; then
    cp "$src" "$OUT/lib/gstreamer-1.0/"
  else
    echo "no plugin libgst$name.dll in $PREFIX" >&2
    exit 1
  fi
done

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

# Every element this program can ask for by name, and every element webrtcbin
# builds inside itself. The pruning above is a list of files; this is the list
# that matters, and it is checked against the bundle rather than against the
# machine that built it -- GST_PLUGIN_SYSTEM_PATH_1_0 points at the tree we just
# made and GST_PLUGIN_PATH_1_0 is emptied, so a plugin left behind fails here
# and not on a user's desktop.
#
# The names come from receiver/main.c (its pipeline strings and its two
# factory calls) and from webrtcbin's own internals: it makes an rtpbin, a pair
# of nice elements, the DTLS-SRTP encoder and decoder, the jitter buffer,
# retransmission and the demuxers under them.
ELEMENTS="
  webrtcbin rtph264depay rtpvp8depay h264parse
  tee queue identity capsfilter filesink fakesink funnel
  videoconvert matroskamux avdec_h264 vp8dec gtk4paintablesink
  d3d11h264dec
  rtpbin rtpjitterbuffer rtpssrcdemux rtpptdemux rtpstorage
  rtprtxsend rtprtxreceive
  nicesrc nicesink dtlssrtpenc dtlssrtpdec srtpenc srtpdec
  sctpenc sctpdec
"
missing=
for el in $ELEMENTS; do
  GST_PLUGIN_SYSTEM_PATH_1_0="$OUT/lib/gstreamer-1.0" GST_PLUGIN_PATH_1_0= \
    "$OUT/bin/gst-inspect-1.0.exe" "$el" >/dev/null 2>&1 || missing="$missing $el"
done
if [ -n "$missing" ]; then
  echo "the bundle is missing these elements:$missing" >&2
  exit 1
fi
echo "all $(echo $ELEMENTS | wc -w) named elements resolve inside the bundle"

# The only mandatory data file: GTK reads settings through GSettings, and
# g_settings_new() aborts the process when the schema is missing.
glib-compile-schemas --targetdir="$OUT/share/glib-2.0/schemas" "$PREFIX/share/glib-2.0/schemas"

# Startup optimisation only — GIO loads every valid module in the directory
# when the cache is absent.
gio-querymodules "$OUT/lib/gio/modules" || true

# The icon theme. libgtk embeds a private handful of symbolic icons, and the
# toolbar happened to be drawing from those: record, fullscreen and the close
# glyph are all in there. view-restore-symbolic is not -- and that is the one
# the fullscreen button switches to *while* fullscreen, so pressing F on a
# machine with no icon theme installed turned that button blank. The only
# visible way out of fullscreen, drawn as nothing at all.
#
# Shipping the theme rather than picking a fourth name GTK happens to embed:
# that private set is not an API, and the next icon this program wants would
# be the same bug again.
cp -r "$PREFIX/share/icons/Adwaita" "$OUT/share/icons/"
cp -r "$PREFIX/share/icons/hicolor" "$OUT/share/icons/" 2>/dev/null || true

# And an index for it, for the same reason gio-querymodules runs above: with
# no icon-theme.cache GTK walks the theme itself, and Adwaita is thousands of
# small files. On a warm filesystem that walk is nothing; on a cold one it is
# seconds, and startup is exactly when the filesystem is cold.
# gtk4-update-icon-cache is GTK 4's name for it; the unprefixed one is GTK 3's
# and is what a machine with both installed may have instead. An index that
# fails to build is a slower first launch and nothing worse, so neither name
# being present is not an error.
icon_cache () {
  gtk4-update-icon-cache --force --quiet "$1" 2>/dev/null \n    || gtk-update-icon-cache --force --quiet "$1" 2>/dev/null \n    || echo "  no icon-cache tool for $1; first launch will walk it" >&2
}
icon_cache "$OUT/share/icons/Adwaita"
[ -d "$OUT/share/icons/hicolor" ] && icon_cache "$OUT/share/icons/hicolor"

# A bundle that gets this far with no theme is a bundle whose buttons are
# blank, and nothing downstream would notice: the smoke test loads plugins,
# and the MSI size floor is met by the DLLs alone.
test -s "$OUT/share/icons/Adwaita/index.theme" \
  || { echo "no icon theme in the bundle -- the toolbar would draw blank" >&2; exit 1; }

du -sh "$OUT"
echo "bundled $(find "$OUT" -name '*.dll' | wc -l) DLLs into $OUT"

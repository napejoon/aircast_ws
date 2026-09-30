#!/usr/bin/env bash
# Turn a Homebrew build of the receiver into Kagami.app: everything it loads,
# inside it, found through it (receiver/main.c harden_environment, __APPLE__).
#
#   tools/bundle-macos.sh [build dir] [output .app]     # defaults: build dist/Kagami.app
#
# The macOS counterpart of tools/bundle-windows.sh, with the same plugin list
# less Direct3D. Homebrew builds everything with absolute install names into
# its own prefix, so each Mach-O is rewritten to find its libraries in
# Contents/Frameworks, and then re-signed ad hoc: rewriting a load command
# breaks the signature, and Apple Silicon kills a process whose code signature
# does not verify ("Killed: 9", nothing else).
#
# Every executable lives in Contents/MacOS -- kagami, gst-plugin-scanner,
# gst-inspect-1.0 -- so @executable_path/../Frameworks is right for each of
# them and for every plugin any of them loads.
set -euo pipefail

BUILD=${1:-build}
OUT=${2:-dist/Kagami.app}
BREW=$(brew --prefix)
# The gstreamer keg, not $BREW/lib: libnice-gstreamer links its plugin in here.
GST=$(brew --prefix gstreamer)
HERE=$(cd "$(dirname "$0")" && pwd)
C="$OUT/Contents"
M="$C/MacOS"
F="$C/Frameworks"
R="$C/Resources"

rm -rf "$OUT"
mkdir -p "$M" "$F" "$R/lib/gstreamer-1.0" "$R/lib/gio/modules" \
         "$R/share/glib-2.0/schemas" "$R/share/icons"
cp "$BUILD/kagami" "$M/"
cp "$BREW/bin/gst-inspect-1.0" "$M/"
cp "$GST/libexec/gstreamer-1.0/gst-plugin-scanner" "$M/"

# The Windows list (tools/bundle-windows.sh says what each is for), less d3d11.
PLUGINS="
  coreelements typefindfunctions app
  webrtc nice dtls srtp sctp
  rtp rtpmanager
  videoparsersbad videoconvertscale matroska
  libav vpx
  gtk4
  opengl
"
for name in $PLUGINS; do
  src=$(ls "$GST"/lib/gstreamer-1.0/libgst"$name".{dylib,so} 2>/dev/null | head -1 || true)
  [ -n "$src" ] || { echo "no plugin libgst$name in $GST/lib/gstreamer-1.0" >&2; exit 1; }
  cp "$(realpath "$src")" "$R/lib/gstreamer-1.0/"
done

# wss:// is glib-networking's TLS backend, whichever one Homebrew built.
tls=$(ls "$BREW"/lib/gio/modules/libgio{openssl,gnutls}.{so,dylib} 2>/dev/null | head -1 || true)
[ -n "$tls" ] || { echo "no glib-networking TLS module in $BREW/lib/gio/modules" >&2; exit 1; }
cp "$(realpath "$tls")" "$R/lib/gio/modules/"

# The library closure of all of it, copied into Frameworks and every reference
# rewritten. dylibbundler walks the whole tree and rewrites the copies too.
args=()
for f in "$M"/* "$R"/lib/gstreamer-1.0/* "$R"/lib/gio/modules/*; do args+=(-x "$f"); done
# The Rust plugins (gtk4 among them) name their libraries by @rpath, hence the
# search paths.
dylibbundler --overwrite-dir --bundle-deps --no-codesign \
  -s "$BREW/lib" -s "$GST/lib" -s "$GST/lib/gstreamer-1.0" \
  -d "$F" -p @executable_path/../Frameworks/ "${args[@]}" >/dev/null
# dylibbundler rewrites what the plugins load, not what they call themselves:
# each still carries its Homebrew path as its own install name. Nothing loads a
# plugin by that name, but it is the one string that would, and the leak check
# below is only worth having if it can say "none" and mean it.
for f in "$R"/lib/gstreamer-1.0/* "$R"/lib/gio/modules/*; do
  # Only a dylib has an install name; a loadable bundle (MH_BUNDLE) has none.
  if otool -D "$f" | tail -n +2 | grep -q .; then
    install_name_tool -id "@executable_path/../${f#"$C"/}" "$f" 2>/dev/null
  fi
done

# What dylibbundler leaves: the Rust plugins (gtk4paintablesink) reach
# libgstreamer and friends as @rpath/..., resolved through LC_RPATH entries
# that point into Homebrew's Cellar. It warns "MAY NOT CORRECTLY HANDLE" and
# means it: on a Mac with Homebrew the plugin loads Homebrew's libgstreamer
# beside the bundle's, and fails to register; on one without, it fails to load.
# Every @rpath reference becomes the bundle's own copy, and every LC_RPATH that
# leaves the bundle goes.
macho=()
while IFS= read -r -d '' f; do
  file -b "$f" | grep -q Mach-O && macho+=("$f")
done < <(find "$C" -type f -print0)
unresolved=
for f in "${macho[@]}"; do
  id=$(otool -D "$f" | tail -n +2)
  case "$id" in @rpath/*)
    install_name_tool -id "@executable_path/../Frameworks/${id#@rpath/}" "$f" 2>/dev/null ;;
  esac
  for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep '^@rpath/' || true); do
    [ "$dep" = "$id" ] && continue
    name=${dep#@rpath/}
    if [ -e "$F/$name" ]; then
      install_name_tool -change "$dep" "@executable_path/../Frameworks/$name" "$f" 2>/dev/null
    else
      unresolved="$unresolved $dep(${f#"$C"/})"
    fi
  done
  for rp in $(otool -l "$f" | awk '$1=="cmd"{r=($2=="LC_RPATH")} r && $1=="path"{print $2}'); do
    install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null
  done
done
[ -z "$unresolved" ] || { echo "no bundled copy for:$unresolved" >&2; exit 1; }

glib-compile-schemas --targetdir="$R/share/glib-2.0/schemas" "$BREW/share/glib-2.0/schemas"
gio-querymodules "$R/lib/gio/modules" || true
# Dereferenced (-L): a bundle carries files, not links into a Cellar that the
# next Mac does not have, and codesign --strict refuses a link that leaves the
# bundle. Homebrew's hicolor is every other formula's icons by symlink, so only
# its index.theme comes along, for our own icon to sit in.
cp -RL "$BREW/share/icons/Adwaita" "$R/share/icons/"
mkdir -p "$R/share/icons/hicolor/256x256/apps"
cp -L "$BREW/share/icons/hicolor/index.theme" "$R/share/icons/hicolor/"
cp "$HERE/../receiver/icons/kagami-256.png" "$R/share/icons/hicolor/256x256/apps/kagami.png"
for theme in Adwaita hicolor; do
  gtk4-update-icon-cache --force --quiet "$R/share/icons/$theme" 2>/dev/null || true
done
cp "$HERE/../installer/kagami.icns" "$R/kagami.icns"
# The CA roots main.c installs as the TLS default (harden_environment says why).
mkdir -p "$R/etc"
cp -L "$BREW/etc/ca-certificates/cert.pem" "$R/etc/cert.pem"
grep -q "BEGIN CERTIFICATE" "$R/etc/cert.pem" || { echo "no CA roots to bundle" >&2; exit 1; }

version=${AIRCAST_VERSION:-0.0.0}
# The oldest macOS it runs on is the newest any of its binaries was built for:
# Homebrew's bottles target the OS they were built on.
minos=$(find "$C" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' \) -print0 \
  | xargs -0 -n1 otool -l 2>/dev/null \
  | awk '/LC_BUILD_VERSION/{b=1} b && $1=="minos"{print $2; b=0}' \
  | sort -t. -k1,1n -k2,2n | tail -1 || true)
[ -n "$minos" ] || { echo "no LC_BUILD_VERSION anywhere in the bundle" >&2; exit 1; }
cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>Kagami</string>
	<key>CFBundleDisplayName</key>
	<string>Kagami</string>
	<key>CFBundleIdentifier</key>
	<string>io.kagami.receiver</string>
	<key>CFBundleExecutable</key>
	<string>kagami</string>
	<key>CFBundleIconFile</key>
	<string>kagami</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$version</string>
	<key>CFBundleVersion</key>
	<string>$version</string>
	<key>LSMinimumSystemVersion</key>
	<string>$minos</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSLocalNetworkUsageDescription</key>
	<string>Kagami receives the phone's screen straight over your local network when it can.</string>
</dict>
</plist>
EOF

# Ad hoc, inside out. Not notarised: that takes a paid Apple Developer ID.
find "$C" -type f \( -name '*.dylib' -o -name '*.so' \) -exec codesign --force --sign - {} \;
for f in "$M"/*; do codesign --force --sign - "$f"; done
codesign --force --sign - "$OUT"

# Nothing may still point into Homebrew: that is a Mac with Homebrew passing and
# every other Mac failing to start the app at all.
leaks=$(find "$C" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' \) -print0 \
  | xargs -0 -n1 otool -L 2>/dev/null | grep -E "^\s+($BREW|/usr/local|/opt/homebrew)" || true)
[ -z "$leaks" ] || { echo "still linked into Homebrew:"; echo "$leaks"; exit 1; } >&2
rpaths=$(for f in "${macho[@]}"; do otool -l "$f" | awk -v f="${f#"$C"/}" '$1=="cmd"{r=($2=="LC_RPATH")} r && $1=="path"{print f": "$2}'; done)
[ -z "$rpaths" ] || { echo "LC_RPATH left in the bundle:"; echo "$rpaths" | head; exit 1; } >&2
links=$(find "$C" -type l)
[ -z "$links" ] || { echo "symlinks left in the bundle:"; echo "$links" | head; exit 1; } >&2
codesign --verify --deep --strict --verbose=2 "$OUT"

# The same element list the Windows bundle proves, loaded from the bundle only.
ELEMENTS="
  webrtcbin rtph264depay rtpvp8depay h264parse
  tee queue identity capsfilter filesink fakesink funnel
  videoconvert matroskamux avdec_h264 vp8dec gtk4paintablesink
  rtpbin rtpjitterbuffer rtpssrcdemux rtpptdemux rtpstorage
  rtprtxsend rtprtxreceive
  nicesrc nicesink dtlssrtpenc dtlssrtpdec srtpenc srtpdec
  sctpenc sctpdec
"
reg=$(mktemp -d)/registry.bin
missing=
for el in $ELEMENTS; do
  env -u GST_PLUGIN_PATH -u GST_PLUGIN_PATH_1_0 \
    GST_PLUGIN_SYSTEM_PATH_1_0="$R/lib/gstreamer-1.0" \
    GST_PLUGIN_SCANNER_1_0="$M/gst-plugin-scanner" GST_REGISTRY_1_0="$reg" \
    "$M/gst-inspect-1.0" "$el" >/dev/null 2>&1 || missing="$missing $el"
done
if [ -n "$missing" ]; then
  # Why, for the first of them: which plugin file failed to load, and the dyld
  # or registration error that says what it wanted.
  first=${missing%% *}; first=${first:-${missing# }}; first=${first%% *}
  rm -f "$reg"
  env -u GST_PLUGIN_PATH -u GST_PLUGIN_PATH_1_0 GST_DEBUG="GST_PLUGIN_LOADING:4" \
    GST_PLUGIN_SYSTEM_PATH_1_0="$R/lib/gstreamer-1.0" \
    GST_PLUGIN_SCANNER_1_0="$M/gst-plugin-scanner" GST_REGISTRY_1_0="$reg" \
    "$M/gst-inspect-1.0" "$first" 2>&1 | grep -iE "fail|error|not|dlopen|image" | head -20 >&2 || true
  echo "the bundle is missing these elements:$missing" >&2
  exit 1
fi
echo "all $(echo $ELEMENTS | wc -w | tr -d ' ') elements resolve inside the bundle"
echo "$OUT: $(du -sh "$OUT" | cut -f1), $(ls "$F" | wc -l | tr -d ' ') libraries, macOS $minos+, $(uname -m)"

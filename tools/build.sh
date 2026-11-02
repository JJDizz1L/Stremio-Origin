#!/bin/bash
# Builds Stremio Origin (native) for one target:
#   tools/build.sh mac            -> build/mac/stremioorigin        (the Mac preview, SDL2 + OpenGL 2.1)
#   tools/build.sh webos          -> build/webos/stremioorigin      (LG webOS, ARMv7, SDL2 + GLES2)
#   tools/build.sh mac --run      -> build and start it
# Third-party code (FreeType, HarfBuzz, FriBidi, cJSON, stb) is compiled once per target into
# build/<target>/tp/libtp.a and reused until `--clean-tp`.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ORIGIN_BUILD="${ORIGIN_BUILD:-$HOME/origin-build}"   # the private build folder outside the repo: toolchain, SDK, sources, keys
TARGET="${1:-mac}"; shift || true
RUN=0; CLEAN_TP=0
for a in "$@"; do case "$a" in --run) RUN=1;; --clean-tp) CLEAN_TP=1;; esac; done
B="$ROOT/build/$TARGET"; TP="$ROOT/third_party"; mkdir -p "$B/tp" "$B/obj"
ZIG="${ZIG:-$ORIGIN_BUILD/zig-aarch64-macos-0.17.0/zig}"
SDK="${SDK:-$ORIGIN_BUILD/sdk/arm-webos-linux-gnueabi_sdk-buildroot/arm-webos-linux-gnueabi/sysroot}"
MACSDL="${MACSDL:-$ORIGIN_BUILD/mac/sdl2}"

case "$TARGET" in
  mac)
    CC="cc"; CXX="c++"; AR="ar"; WEBPCC="cc"
    TFLAGS="-DSB_MAC=1 -I$MACSDL/include -I$MACSDL/include/SDL2"
    LIBS="$($MACSDL/bin/sdl2-config --static-libs) -Wl,-framework,OpenGL -lcurl -liconv -lz"
    PLAT=mac ;;
  linux)
    CC="cc"; CXX="c++"; AR="ar"; WEBPCC="cc -U__SSE2__ -U__SSE4_1__"
    TFLAGS="-DSB_MAC=1 -DSB_LINUX=1 -U__SSE2__ -U__SSE4_1__"
    LIBS="$(sdl2-config --libs) -lGL -lmpv -lcurl -lz -lpthread -lm -ldl"
    PLAT=linux ;;
  webos)
    CC="$ZIG cc -target arm-linux-gnueabi.2.12 -mcpu=cortex_a9 -mfloat-abi=softfp"
    WEBPCC="$ZIG cc -target arm-linux-gnueabi.2.12 -mcpu=cortex_a9-neon -mfloat-abi=softfp"   # libwebp's NEON paths break this compiler's backend: plain C there
    CXX="$ZIG c++ -target arm-linux-gnueabi.2.12 -mcpu=cortex_a9 -mfloat-abi=softfp"
    AR="$ZIG ar"
    TFLAGS="-DSB_WEBOS=1 -D_FILE_OFFSET_BITS=64 -idirafter $SDK/usr/include"   # after the compiler's own libc headers: the SDK's old glibc headers must not mix with them
    LIBS="$SDK/usr/lib/libSDL2.so $SDK/usr/lib/libGLESv2.so $SDK/usr/lib/libz.so -ldl -lpthread -lm"
    PLAT=webos ;;
  *) echo "unknown target $TARGET"; exit 1;;
esac
OPT="-O2 -fno-strict-aliasing -ffunction-sections -fdata-sections"
if [ -n "${SB_FP:-}" ]; then OPT="$OPT -fno-omit-frame-pointer -marm"; fi  # SB_FP=1: frame pointers for the profiler
if [ -n "${SB_ASAN:-}" ]; then OPT="-O1 -g -fsanitize=address -fno-omit-frame-pointer"; B="$B-asan"; mkdir -p "$B/tp" "$B/obj"; fi  # SB_ASAN=1 tools/build.sh mac: a sanitizer build in build/mac-asan
APPID="${APPID:-tv.stremioorigin.app}"
CFLAGS="$OPT -std=c11 -D_GNU_SOURCE -DSB_APP_ID=\"$APPID\" -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $TFLAGS -I$ROOT/src -I$TP/freetype/include -I$TP/harfbuzz/src -I$TP/fribidi/lib -I$TP/config/fribidi -I$TP/cjson -I$TP/stb -I$TP/qrcodegen -I$TP/webp -I$TP/crypto -I$TP/libass"

# ---- built-in keys ------------------------------------------------------------------------
# The app's own keys (Trakt, TMDB) come from a private file outside the repo, KEYS=$ORIGIN_BUILD/keys.env
# by default (TRAKT_CLIENT_ID=..., TMDB_API_KEY=...; Trakt's app signs in with PKCE: no secret). They go into build/<target>/sbkeys.h, scrambled so they are
# not plain text in the binary. Without the file the build still works: each then needs a user's own key.
KEYS="${KEYS:-$ORIGIN_BUILD/keys.env}"
python3 - "$KEYS" "$B/sbkeys.h" <<'PY'
import os, sys, hashlib
src, out = sys.argv[1], sys.argv[2]; kv = {}
if os.path.isfile(src):
    for ln in open(src):
        ln = ln.strip()
        if ln and not ln.startswith('#') and '=' in ln: k, v = ln.split('=', 1); kv[k.strip()] = v.strip().strip('"\'')
def arr(name):
    v = kv.get(name, '').encode(); pad = hashlib.sha256(b'sb:' + name.encode()).digest()
    return 'static const unsigned char SBK_%s[] = { %s0 }, SBK_%s_PAD[32] = { %s };\n#define SBK_%s_LEN %d\n' % (name, ''.join('%d,' % (c ^ pad[i % 32]) for i, c in enumerate(v)), name, ','.join(str(x) for x in pad), name, len(v))
body = '/* made by tools/build.sh from the private keys file: never commit */\n' + arr('TRAKT_CLIENT_ID') + arr('TMDB_API_KEY')
if not os.path.isfile(out) or open(out).read() != body: open(out, 'w').write(body)
print('== keys built in: %s' % (', '.join(n for n in ('TRAKT_CLIENT_ID', 'TMDB_API_KEY') if kv.get(n)) or 'none (%s)' % src))
PY
CFLAGS="$CFLAGS -I$B"

# ---- third-party ---------------------------------------------------------------------------
if [ $CLEAN_TP = 1 ]; then rm -rf "$B/tp"; mkdir -p "$B/tp"; fi
if [ ! -f "$B/tp/libtp.a" ]; then
  echo "== third-party ($TARGET)"
  TPQ="-w $OPT"
  FT="-DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H=<ftmodule-sb.h> -I$TP/config -I$TP/freetype/include"
  for f in base/ftbase base/ftinit base/ftsystem base/ftdebug base/ftbitmap base/ftglyph base/ftmm base/ftbbox base/ftsynth base/ftstroke \
           gzip/ftgzip truetype/truetype sfnt/sfnt smooth/smooth autofit/autofit psnames/psnames pshinter/pshinter cff/cff psaux/psaux; do
    $CC $TPQ $FT -c "$TP/freetype/src/$f.c" -o "$B/tp/ft_$(basename $f).o"
  done
  $CXX -w $OPT -std=c++11 -fno-exceptions -fno-rtti -DHAVE_FREETYPE=1 -DHAVE_PTHREAD=1 -I$TP/freetype/include -I$TP/harfbuzz/src \
    -c "$TP/harfbuzz/src/harfbuzz.cc" -o "$B/tp/harfbuzz.o"
  for f in "$TP"/fribidi/lib/fribidi*.c; do
    $CC $TPQ -DHAVE_CONFIG_H -I$TP/config/fribidi -I$TP/fribidi/lib -c "$f" -o "$B/tp/fb_$(basename "${f%.c}").o"
  done
  $CC $TPQ -c "$TP/cjson/cJSON.c" -o "$B/tp/cJSON.o"
  $CC $TPQ -c "$TP/qrcodegen/qrcodegen.c" -o "$B/tp/qrcodegen.o"
  for f in "$TP"/webp/src/dec/*.c "$TP"/webp/src/dsp/*.c "$TP"/webp/src/utils/*.c; do   # libwebp, decoder only
    case "$(basename "$f")" in *enc*|*writer*|*quant_levels_utils*|*_sse*|*_msa*|*_mips*) continue;; esac
    $WEBPCC $TPQ -DWEBP_REDUCE_CSP -I$TP/webp -c "$f" -o "$B/tp/webp_$(basename "${f%.c}").o"
  done
  $AR rcs "$B/tp/libtp.a" "$B"/tp/*.o
fi

if [ ! -f "$B/tp/libass.a" ]; then   # libass (styled ASS / SSA subtitles) on the FreeType, HarfBuzz and FriBidi above, no system font provider
  echo "== libass ($TARGET)"; mkdir -p "$B/tp/ass"
  for f in "$TP"/libass/libass/*.c "$TP"/libass/libass/c/*.c; do
    $CC -w $OPT -std=gnu11 -DHAVE_CONFIG_H -I$TP/config/libass -I$TP/libass/libass -I$TP/freetype/include -I$TP/harfbuzz/src -I$TP/fribidi/lib -I$TP/config/fribidi \
      -c "$f" -o "$B/tp/ass/$(basename "${f%.c}").o"
  done
  $CC -w $OPT -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H="<ftmodule-sb.h>" -I$TP/config -I$TP/freetype/include -c "$TP/freetype/src/base/fttype1.c" -o "$B/tp/ass/ft_fttype1.o"   # (the Type 1 info libass asks FreeType for)
  $AR rcs "$B/tp/libass.a" "$B"/tp/ass/*.o
fi

# ---- the app -------------------------------------------------------------------------------
echo "== app ($TARGET)"
OBJS=()
STAMP="$B/obj/.appid"; if [ "$(cat "$STAMP" 2>/dev/null)" != "$APPID" ]; then rm -f "$B"/obj/*.o; echo "$APPID" > "$STAMP"; fi
for f in "$TP"/crypto/*.c; do   # SHA-256, AES, GCM, PBKDF2 (log sharing): small, built beside the app rather than into libtp.a
  o="$B/tp/crypto_$(basename "${f%.c}").o"; if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then $CC -w $OPT -I$TP/crypto -c "$f" -o "$o"; fi; OBJS+=("$o")
done
for f in "$ROOT"/src/core/*.c "$ROOT"/src/ui/*.c "$ROOT"/src/screens/*.c "$ROOT"/src/platform/$PLAT/*.c "$ROOT"/src/main.c; do
  [ -f "$f" ] || continue
  o="$B/obj/$(echo "${f#$ROOT/src/}" | tr / _).o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find "$ROOT/src" -name '*.h' -newer "$o" | head -1)" ] || [ "$B/sbkeys.h" -nt "$o" ]; then
    $CC $CFLAGS -c "$f" -o "$o"
  fi
  OBJS+=("$o")
done
if [ "$TARGET" = mac ] || [ "$TARGET" = linux ]; then
  $CXX $OPT -o "$B/stremioorigin" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" $LIBS
else
  $CXX $OPT -Wl,--gc-sections -o "$B/stremioorigin.dbg" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" $LIBS   # symbols kept: crash addresses resolve against it
  python3 "$ROOT/tools/elfstrip.py" "$B/stremioorigin.dbg" "$B/stremioorigin" && chmod +x "$B/stremioorigin"   # the same link, stripped: crash addresses match the symbols byte for byte (two links came out apart)
  BID=$(LC_ALL=C grep -ao 'SBBUILDID:[0-9a-f]\{32\}' "$B/stremioorigin.dbg" | head -1 | cut -c11-22)   # every build's debug copy kept by its id (the last 40): a crash from any build installed can be named
  if [ -n "$BID" ]; then mkdir -p "$ROOT/dist/dbg/builds"; cp "$B/stremioorigin.dbg" "$ROOT/dist/dbg/builds/$BID.dbg"; ls -t "$ROOT"/dist/dbg/builds/*.dbg | tail -n +41 | xargs rm -f; fi
fi
ls -l "$B/stremioorigin" | awk '{printf "%s  %.1f MB\n", $NF, $5/1048576}'
[ $RUN = 1 ] && [ "$TARGET" = mac ] && exec "$B/stremioorigin" "$ROOT/assets"
exit 0

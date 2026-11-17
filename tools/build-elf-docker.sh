#!/bin/sh
# Builds AnyACE without installing anything: the official PS5 payload SDK
# (the release zip, unpacked) mounted read-only into a container with clang 18
# and lld 18 for linux/amd64. The ELF is dist/AnyACE-PS5-<version>.elf, with the
# version from src/version.h, and a .sha256 beside it.
#
#   tools/build-elf-docker.sh /path/to/ps5-payload-sdk [image]
#
# The default image is any Ubuntu 24.04 one with clang-18 and lld-18, e.g. one
# made from:  FROM ubuntu:24.04 / RUN apt-get update && apt-get install -y clang-18 lld-18
set -e
SDK="${1:?usage: $0 /path/to/ps5-payload-sdk [image]}"
IMAGE="${2:-anypad-build}"
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^#define ANYPAD_VERSION "\(.*\)"/\1/p' "$PROJ/src/version.h")"
[ -n "$VERSION" ] || { echo "no version in src/version.h" >&2; exit 1; }
OUT="dist/AnyACE-PS5-$VERSION.elf"
mkdir -p "$PROJ/dist"
# The sources are the SRCS list of ps5.mk, so both builds compile the same files.
SRCS="$(sed -n '/^SRCS :=/,/[^\\]$/p' "$PROJ/ps5.mk" | sed 's/^SRCS :=//; s/\\//g' | tr -s ' \n' ' ')"
[ -n "$SRCS" ] || { echo "no SRCS in ps5.mk" >&2; exit 1; }

docker run --rm --platform linux/amd64 -e OUT="$OUT" -e SRCS="$SRCS" \
  -v "$SDK":/opt/ps5-payload-sdk-ro:ro -v "$PROJ":/work "$IMAGE" bash -c '
set -e
cp -r /opt/ps5-payload-sdk-ro /opt/ps5-payload-sdk
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
# the SDK wrappers ask llvm-config where LLVM lives
printf "#!/bin/sh\ncase \"\$1\" in --bindir) echo /usr/lib/llvm-18/bin;; --version) echo 18.1.3;; *) exit 1;; esac\n" \
  > /usr/local/bin/llvm-config
chmod +x /usr/local/bin/llvm-config
CC=$PS5_PAYLOAD_SDK/bin/prospero-clang
cd /work
rm -rf build/elf && mkdir -p build/elf
OBJS=""
for src in $SRCS; do
  f=$(basename "$src" .c)
  $CC -std=c11 -Wall -Wextra -Werror -O2 -Isrc -c "$src" -o build/elf/$f.o
  OBJS="$OBJS build/elf/$f.o"
done
$CC -o "$OUT" $OBJS -lScePad -lSceUserService -lSceSystemService -lSceAppInstUtil -ldl
'
cd "$PROJ"
{ shasum -a 256 "$OUT" 2>/dev/null || sha256sum "$OUT"; } | tee "$OUT.sha256"
ls -la "$OUT"

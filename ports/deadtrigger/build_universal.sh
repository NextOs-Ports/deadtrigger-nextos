#!/usr/bin/env bash
# Reproducible public AArch64 build.
#
# A pinned modern compiler generates the objects because GCC 8 miscompiles this
# Unity 2019 host's atomics/thread bridge. Linking and stripping happen inside a
# pinned Debian Buster image, which keeps every project-built Linux ELF at
# GLIBC <= 2.30.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
REPOSITORY_ROOT=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
OUTPUT_REL=build/deadtrigger-nextos
OUTPUT_PATH=$PORT_DIR/$OUTPUT_REL
BUILDER_IMAGE=playfetch-builder:buster
BUILDER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
COMPILER_VERSION=16.1.0
COMPILER_SHA256=2fcae05bfafdfc6c0c3453431ce4538fdb8b481169c4670ae48be48a28e4fbe9
CC1_SHA256=1e947f6ecee4219b010307e7dd0cd00dae78b9c8533c3e3d852a1a6c1d209a7b
ASSEMBLER_SHA256=d798e501ac1a4a674106e132865ba18236bb7e36304d18a1b886595b839f9f44
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786233600}

if [[ -n ${DT_UNIVERSAL_OUTPUT:-} &&
      $DT_UNIVERSAL_OUTPUT != "$OUTPUT_REL" ]]; then
    echo "DT_UNIVERSAL_OUTPUT may only select canonical $OUTPUT_REL" >&2
    exit 1
fi

for tool in docker aarch64-linux-gnu-gcc aarch64-linux-gnu-nm \
            aarch64-linux-gnu-readelf file sha256sum strings; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing pinned-build tool: $tool" >&2
        exit 1
    }
done

CC=$(command -v aarch64-linux-gnu-gcc)
CC1=$("$CC" -print-prog-name=cc1)
ASSEMBLER=$("$CC" -print-prog-name=as)
[[ $("$CC" -dumpmachine) == aarch64-linux-gnu &&
   $("$CC" -dumpfullversion -dumpversion) == "$COMPILER_VERSION" ]] || {
    echo "pinned AArch64 GCC $COMPILER_VERSION is required" >&2
    exit 1
}
printf '%s  %s\n' "$COMPILER_SHA256" "$CC" |
    sha256sum -c - >/dev/null
printf '%s  %s\n' "$CC1_SHA256" "$CC1" |
    sha256sum -c - >/dev/null
printf '%s  %s\n' "$ASSEMBLER_SHA256" "$ASSEMBLER" |
    sha256sum -c - >/dev/null

ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" \
    --format '{{.Id}}' 2>/dev/null) || {
    echo "offline builder image missing: $BUILDER_IMAGE" >&2
    exit 1
}
[[ $ACTUAL_IMAGE_ID == "$BUILDER_IMAGE_ID" ]] || {
    echo "builder image changed: $ACTUAL_IMAGE_ID" >&2
    exit 1
}

NEXTOS_ROOT=${NEXTOS_ROOT:-/mnt/ARQUIVOS/NextOS-Elite-Edition}
NEXTOS_TOOLCHAIN=$(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
        -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
        -print | sort -V | tail -1
)
[[ -n $NEXTOS_TOOLCHAIN ]] || {
    echo "NextOS header sysroot not found below $NEXTOS_ROOT" >&2
    exit 1
}
NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
[[ -d $NEXTOS_SYSROOT/usr/include/SDL2 ]] || {
    echo "NextOS SDL2 headers unavailable: $NEXTOS_SYSROOT" >&2
    exit 1
}

WORK_DIR=$(mktemp -d)
cleanup() {
    find "$WORK_DIR" -depth -delete 2>/dev/null || true
}
trap cleanup EXIT
mkdir -p "$WORK_DIR/obj" "$WORK_DIR/out"

# Only the target libc headers are copied out. The image never gets network
# access, and the host compiler/linker identities above are fail-closed.
docker run --rm --network none \
    --user "$(id -u):$(id -g)" \
    -v "$WORK_DIR":/work \
    "$BUILDER_IMAGE_ID" \
    sh -c 'cp -a /usr/aarch64-linux-gnu/include /work/buster-include'

SHARED_LOADER=$REPOSITORY_ROOT/ports/terraria/src
SHARED_JNI=$REPOSITORY_ROOT/ports/ff5/src
FRAMEWORK_ROOT=$REPOSITORY_ROOT/framework
GCC_INCLUDE=$("$CC" -print-file-name=include)
GCC_FIXED=$(dirname "$GCC_INCLUDE")/include-fixed

COMMON_INCLUDES=(
    -I "$PORT_DIR/src"
    -I "$SHARED_LOADER"
    -I "$SHARED_JNI"
    -I "$FRAMEWORK_ROOT/nxcompat/include"
    -I "$FRAMEWORK_ROOT/nxcompat/src"
    -I "$FRAMEWORK_ROOT/nxinput/include"
    -I "$FRAMEWORK_ROOT/nxinput/src"
    -I "$FRAMEWORK_ROOT/nxaudio/include"
    -nostdinc
    -isystem "$GCC_INCLUDE"
)
[[ ! -d $GCC_FIXED ]] ||
    COMMON_INCLUDES+=( -isystem "$GCC_FIXED" )
COMMON_INCLUDES+=(
    -isystem "$WORK_DIR/buster-include"
    -idirafter "$NEXTOS_SYSROOT/usr/include"
    -idirafter "$NEXTOS_SYSROOT/usr/include/SDL2"
)

SOURCES=(
    "$PORT_DIR/src/egl_sdl.c"
    "$PORT_DIR/src/falsojni_dt.c"
    "$PORT_DIR/src/framework_bridge.c"
    "$PORT_DIR/src/gamepad_dt.c"
    "$PORT_DIR/src/loader.c"
    "$PORT_DIR/src/main.c"
    "$PORT_DIR/src/media_dt.c"
    "$PORT_DIR/src/opensles_dt.c"
    "$PORT_DIR/src/prefs_dt.c"
    "$PORT_DIR/src/shims.c"
    "$SHARED_LOADER/so_util.c"
    "$SHARED_LOADER/error.c"
    "$SHARED_LOADER/util.c"
    "$SHARED_LOADER/pthread_fake.c"
    "$SHARED_LOADER/sem_shim.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_backend.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_graphics.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_plan.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_probe.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_receipts.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_registry.c"
    "$FRAMEWORK_ROOT/nxcompat/src/nxcompat_report.c"
    "$FRAMEWORK_ROOT/nxinput/src/nxinput.c"
    "$FRAMEWORK_ROOT/nxinput/src/nxinput_core.c"
    "$FRAMEWORK_ROOT/nxinput/src/nxinput_nxcompat.c"
    "$FRAMEWORK_ROOT/nxaudio/src/nxaudio.c"
)

index=0
for source in "${SOURCES[@]}"; do
    index=$((index + 1))
    printf -v object '%s/obj/%03d.o' "$WORK_DIR" "$index"
    "$CC" -std=gnu11 "${COMMON_INCLUDES[@]}" \
        -O2 -fPIE -mno-outline-atomics \
        -fno-omit-frame-pointer -fno-strict-aliasing \
        -fno-stack-protector -fno-ident -D_FORTIFY_SOURCE=2 \
        -ffile-prefix-map="$REPOSITORY_ROOT=/source" \
        -fdebug-prefix-map="$REPOSITORY_ROOT=/source" \
        -Wdate-time -Werror=date-time \
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
        -Wno-cast-function-type -Wno-format-truncation \
        -c "$source" -o "$object"
done

# GCC 8 is intentionally used only for the Buster link. SONAME-only stubs
# ensure that SDL/EGL/GLES come from the target firmware at runtime.
docker run --rm --network none \
    --user "$(id -u):$(id -g)" \
    -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -v "$WORK_DIR":/work \
    "$BUILDER_IMAGE_ID" \
    bash -c '
set -euo pipefail
objects=(/work/obj/*.o)
undefined=$(
    aarch64-linux-gnu-nm --undefined-only "${objects[@]}" 2>/dev/null |
        sed -E "s/.*[[:space:]]//" | sort -u
)
make_stub() {
    output=$1
    soname=$2
    pattern=$3
    source=/work/${output}.c
    : > "$source"
    for symbol in $(printf "%s\n" "$undefined" |
                    grep -E "$pattern" || true); do
        printf "void %s(void) {}\n" "$symbol" >> "$source"
    done
    aarch64-linux-gnu-gcc -shared -fPIC -nostdlib \
        -Wl,-soname,"$soname" "$source" -o "/work/$output"
}
make_stub libSDL2.so libSDL2-2.0.so.0 "^SDL_"
make_stub libEGL.so libEGL.so "^egl[A-Z]"
make_stub libGLESv2.so libGLESv2.so "^gl[A-Z]"
aarch64-linux-gnu-gcc -fPIE -pie -rdynamic \
    -o /work/out/deadtrigger-nextos "${objects[@]}" \
    -L/work -Wl,--as-needed -lSDL2 -lEGL -lGLESv2 \
    -ldl -lm -lpthread -lz -lgcc_s \
    -Wl,-z,relro,-z,now,-z,noexecstack,--build-id=sha1
aarch64-linux-gnu-strip --strip-unneeded /work/out/deadtrigger-nextos
'

mkdir -p "$(dirname "$OUTPUT_PATH")"
install -m 0755 "$WORK_DIR/out/deadtrigger-nextos" "$OUTPUT_PATH"

READELF=aarch64-linux-gnu-readelf
NM=aarch64-linux-gnu-nm
MAX_GLIBC=$(
    "$READELF" --version-info "$OUTPUT_PATH" |
        grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1
)
[[ -n $MAX_GLIBC ]] || {
    echo "unable to determine the GLIBC requirement" >&2
    exit 1
}
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
rest=${version_number#*.}
minor=${rest%%.*}
if (( major > 2 || (major == 2 && minor > 30) )); then
    echo "public build rejected: $MAX_GLIBC exceeds GLIBC_2.30" >&2
    exit 1
fi

MACHINE=$(
    "$READELF" -h "$OUTPUT_PATH" |
        sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p'
)
[[ $MACHINE == AArch64 ]] || {
    echo "unexpected architecture: $MACHINE" >&2
    exit 1
}
INTERPRETER=$(
    "$READELF" -lW "$OUTPUT_PATH" |
        sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p'
)
[[ $INTERPRETER == /lib/ld-linux-aarch64.so.1 ]] || {
    echo "unexpected interpreter: $INTERPRETER" >&2
    exit 1
}
if "$READELF" -lW "$OUTPUT_PATH" |
        awk '$1 == "LOAD" && $0 ~ /RWE/ {bad=1} END {exit !bad}'; then
    echo "public build contains an RWX PT_LOAD" >&2
    exit 1
fi
if "$READELF" -dW "$OUTPUT_PATH" |
        grep -Eq '\((RPATH|RUNPATH)\)'; then
    echo "public build contains a forbidden DT_RPATH/DT_RUNPATH" >&2
    exit 1
fi

NEEDED=$(
    "$READELF" -dW "$OUTPUT_PATH" |
        awk -F'[][]' '/NEEDED/ {print $2}' | sort
)
ALLOWED=$(
    printf '%s\n' libSDL2-2.0.so.0 libEGL.so libGLESv2.so \
        libc.so.6 libdl.so.2 libgcc_s.so.1 libm.so.6 \
        libpthread.so.0 libz.so.1 | sort
)
EXTRA=$(comm -23 <(printf '%s\n' "$NEEDED") \
                 <(printf '%s\n' "$ALLOWED"))
[[ -z $EXTRA ]] || {
    echo "unexpected DT_NEEDED entries:" >&2
    printf '%s\n' "$EXTRA" >&2
    exit 1
}
for required in libSDL2-2.0.so.0 libEGL.so libGLESv2.so libc.so.6; do
    printf '%s\n' "$NEEDED" | grep -qx "$required" || {
        echo "required DT_NEEDED missing: $required" >&2
        exit 1
    }
done

SYMBOL_TABLE=$("$NM" -D "$OUTPUT_PATH")
for symbol in nxcompat_probe nxinput_create nxaudio_classify_backend; do
    printf '%s\n' "$SYMBOL_TABLE" |
        awk -v wanted="$symbol" \
            '$3 == wanted {found=1} END {exit !found}' || {
        echo "framework symbol missing from public ELF: $symbol" >&2
        exit 1
    }
done
if strings "$OUTPUT_PATH" | grep -Fq "$REPOSITORY_ROOT"; then
    echo "public ELF contains a private source path" >&2
    exit 1
fi

echo "DEAD TRIGGER UNIVERSAL BUILD OK -> $OUTPUT_REL"
echo "compiler: AArch64 GCC $COMPILER_VERSION (pinned object code)"
echo "link sysroot: Debian Buster (pinned image)"
echo "glibc maximum: $MAX_GLIBC (limit GLIBC_2.30)"
echo "DT_NEEDED: $(printf '%s\n' "$NEEDED" | tr '\n' ' ')"
file "$OUTPUT_PATH"
sha256sum "$OUTPUT_PATH"

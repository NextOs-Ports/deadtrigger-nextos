#!/usr/bin/env bash
# AArch64 cross gate: pinned low-glibc GCC, Clang/LLD and QEMU.
# The test owns its in-memory synthetic ELF and never loads an external guest.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export LC_ALL=C

NXLOADER_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
BUSTER_IMAGE=${NXLOADER_AARCH64_BUSTER_IMAGE:-playfetch-builder:buster}
BUSTER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
DOCKER=${NXLOADER_DOCKER:-$(command -v docker 2>/dev/null || true)}
CLANG=${NXLOADER_AARCH64_CLANG:-$(command -v clang 2>/dev/null || true)}
LLD=${NXLOADER_AARCH64_LLD:-$(command -v ld.lld 2>/dev/null || true)}
QEMU=${NXLOADER_QEMU_AARCH64:-$(command -v qemu-aarch64 2>/dev/null || true)}
READELF=${NXLOADER_READELF:-$(command -v readelf 2>/dev/null || true)}
OBJDUMP=${NXLOADER_AARCH64_OBJDUMP:-$(command -v aarch64-linux-gnu-objdump 2>/dev/null || true)}
AARCH64_WORK=$(mktemp -d /tmp/nxloader-aarch64-cross.XXXXXX)
GCC_BUILD=$AARCH64_WORK/gcc
CLANG_BUILD=$AARCH64_WORK/clang
EXPORT_ROOT=$AARCH64_WORK/buster-root
SYSROOT=$EXPORT_ROOT/usr/aarch64-linux-gnu
CLANG_SYSROOT=$EXPORT_ROOT
GCC_INSTALL=$EXPORT_ROOT/usr/lib/gcc-cross/aarch64-linux-gnu/8
INTERPRETER=/lib/ld-linux-aarch64.so.1
ELF_COUNT=0
LOADABLE_ELF_COUNT=0
RELOCATABLE_ELF_COUNT=0
MAX_GLIBC=none

# Fixed only after the first complete durable GCC + Clang/LLD run measured
# both build trees. A toolchain/CMake layout change must be reviewed rather
# than silently shrinking the audited surface.
EXPECTED_ELF_COUNT=20
EXPECTED_LOADABLE_ELF_COUNT=5
EXPECTED_RELOCATABLE_ELF_COUNT=15
EXPECTED_GCC_ELF_COUNT=10
EXPECTED_CLANG_ELF_COUNT=10

cleanup() {
  local status=$?
  trap - EXIT
  case $AARCH64_WORK in
    /tmp/nxloader-aarch64-cross.??????)
      chmod -R u+w -- "$AARCH64_WORK" 2>/dev/null || true
      rm -rf -- "$AARCH64_WORK"
      ;;
    *)
      printf 'aarch64-cross: refused cleanup outside owned mktemp: %s\n' \
        "$AARCH64_WORK" >&2
      status=1
      ;;
  esac
  if [[ -e $AARCH64_WORK ]]; then
    printf 'aarch64-cross: cleanup_failed=%s\n' "$AARCH64_WORK" >&2
    [[ $status -ne 0 ]] || status=1
  else
    printf 'aarch64-cross: work_tree_cleaned=1\n'
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail() {
  printf 'aarch64-cross: FAIL %s\n' "$*" >&2
  exit 1
}

for utility in awk chmod cmake cp docker env find grep head id mkdir mktemp \
               od rm sed sha256sum sort tr; do
  command -v "$utility" >/dev/null 2>&1 ||
    fail "missing command: $utility"
done
for utility in "$DOCKER" "$CLANG" "$LLD" "$QEMU" "$READELF" \
               "$OBJDUMP"; do
  [[ -n $utility && -x $utility ]] ||
    fail "missing executable: ${utility:-empty}"
done

resolved_image_id=$($DOCKER image inspect --format '{{.Id}}' "$BUSTER_IMAGE") ||
  fail "cannot inspect pinned Buster image"
[[ $resolved_image_id == "$BUSTER_IMAGE_ID" ]] ||
  fail "Buster image drift: $resolved_image_id != $BUSTER_IMAGE_ID"

docker_base=(
  "$DOCKER" run --rm
  --network none
  --read-only
  --cap-drop ALL
  --security-opt no-new-privileges:true
  --pids-limit 256
  --user "$(id -u):$(id -g)"
  --tmpfs /tmp:rw,nosuid,nodev,noexec,size=256m
)

version_is_above() {
  awk -v found="$1" -v limit="$2" 'BEGIN {
    split(found, f, "."); split(limit, l, ".");
    fn = (f[1] + 0) * 1000000 + (f[2] + 0) * 1000 + (f[3] + 0);
    ln = (l[1] + 0) * 1000000 + (l[2] + 0) * 1000 + (l[3] + 0);
    exit !(fn > ln)
  }'
}

update_max_glibc() {
  local candidate=$1
  if [[ $MAX_GLIBC == none ]] ||
     version_is_above "$candidate" "$MAX_GLIBC"; then
    MAX_GLIBC=$candidate
  fi
}

is_elf() {
  local magic
  magic=$(od -An -N4 -t x1 -- "$1" 2>/dev/null | tr -d ' \n')
  [[ $magic == 7f454c46 ]]
}

resolve_needed() {
  local soname=$1 directory
  for directory in "$SYSROOT/lib" "$SYSROOT/usr/lib" "$GCC_INSTALL"; do
    [[ -e $directory/$soname ]] && return 0
  done
  return 1
}

audit_elf() {
  local elf=$1 header type programs stack_count stack_line
  local interpreter_count interpreter dynamic version_info versions version
  local needed sections

  header=$($READELF -hW -- "$elf") ||
    fail "readelf header failed: $elf"
  grep -Eq '^[[:space:]]*Class:[[:space:]]+ELF64$' <<<"$header" ||
    fail "not ELF64: $elf"
  grep -Eq "^[[:space:]]*Data:[[:space:]]+2's complement, little endian$" \
    <<<"$header" || fail "not little-endian: $elf"
  grep -Eq '^[[:space:]]*Machine:[[:space:]]+AArch64$' <<<"$header" ||
    fail "not EM_AARCH64: $elf"
  grep -Eq '^[[:space:]]*Flags:[[:space:]]+0x0$' <<<"$header" ||
    fail "unexpected AArch64 e_flags: $elf"

  type=$(sed -n \
    's/^[[:space:]]*Type:[[:space:]]*\([^[:space:]]*\).*$/\1/p' \
    <<<"$header")
  case $type in
    EXEC|DYN)
      LOADABLE_ELF_COUNT=$((LOADABLE_ELF_COUNT + 1))
      ;;
    REL)
      RELOCATABLE_ELF_COUNT=$((RELOCATABLE_ELF_COUNT + 1))
      sections=$($READELF -SW -- "$elf") ||
        fail "cannot inspect sections: $elf"
      grep -Fq '.note.GNU-stack' <<<"$sections" ||
        fail "relocatable ELF lacks .note.GNU-stack: $elf"
      if grep -E '\.note\.GNU-stack.*[[:space:]]X[[:space:]]' \
          <<<"$sections" >/dev/null; then
        fail "relocatable ELF requests executable stack: $elf"
      fi
      ;;
    *)
      fail "unexpected AArch64 ELF type ${type:-unknown}: $elf"
      ;;
  esac

  programs=$($READELF -lW -- "$elf" 2>/dev/null || true)
  interpreter_count=$(grep -c 'Requesting program interpreter:' \
    <<<"$programs" || true)
  if (( interpreter_count > 0 )); then
    (( interpreter_count == 1 )) || fail "multiple PT_INTERP entries: $elf"
    interpreter=$(sed -n \
      's/^.*Requesting program interpreter: \([^]]*\)].*$/\1/p' \
      <<<"$programs")
    [[ $interpreter == "$INTERPRETER" ]] ||
      fail "wrong PT_INTERP in $elf: ${interpreter:-none}"
  fi
  if [[ $type == EXEC ]]; then
    (( interpreter_count == 1 )) ||
      fail "dynamic AArch64 executable lacks PT_INTERP: $elf"
  fi

  if [[ $type == EXEC || $type == DYN ]]; then
    stack_count=$(grep -c '^[[:space:]]*GNU_STACK' <<<"$programs" || true)
    (( stack_count == 1 )) || fail "loadable ELF lacks one GNU_STACK: $elf"
    stack_line=$(grep '^[[:space:]]*GNU_STACK' <<<"$programs")
    grep -Eq '[[:space:]]RWE[[:space:]]' <<<"$stack_line" &&
      fail "executable GNU_STACK is forbidden: $elf"
    grep -Eq '^[[:space:]]*LOAD .*RWE[[:space:]]' <<<"$programs" &&
      fail "writable+executable PT_LOAD is forbidden: $elf"
  fi

  dynamic=$($READELF -dW -- "$elf" 2>/dev/null || true)
  grep -Eq '\((RPATH|RUNPATH)\)' <<<"$dynamic" &&
    fail "RPATH/RUNPATH is forbidden: $elf"
  grep -Eq '\(TEXTREL\)|FLAGS.*TEXTREL' <<<"$dynamic" &&
    fail "text relocation marker is forbidden: $elf"

  version_info=$($READELF --version-info -W -- "$elf" 2>/dev/null || true)
  versions=$(grep -oE 'GLIBC_[0-9]+([.][0-9]+)+' <<<"$version_info" |
    sed 's/^GLIBC_//' | sort -Vu || true)
  while IFS= read -r version; do
    [[ -n $version ]] || continue
    version_is_above "$version" 2.30 &&
      fail "$elf requires GLIBC_$version (maximum GLIBC_2.30)"
    update_max_glibc "$version"
  done <<<"$versions"
  grep -q 'GLIBC_PRIVATE' <<<"$version_info" &&
    fail "$elf requires GLIBC_PRIVATE"

  while IFS= read -r needed; do
    [[ -n $needed ]] || continue
    resolve_needed "$needed" ||
      fail "$elf has unresolved DT_NEEDED in Buster sysroot: $needed"
  done < <(sed -n 's/^.*(NEEDED).*\[\([^]]*\)\].*$/\1/p' <<<"$dynamic")

  ELF_COUNT=$((ELF_COUNT + 1))
}

audit_tree() {
  local tree=$1 elf
  while IFS= read -r -d '' elf; do
    is_elf "$elf" || continue
    audit_elf "$elf"
  done < <(find "$tree" -type f -print0)
}

audit_final_executable() {
  local elf=$1 programs count
  [[ -f $elf ]] || fail "missing cross-safe executable: $elf"
  programs=$($READELF -lW -- "$elf") ||
    fail "cannot inspect final executable: $elf"
  count=$(grep -c "Requesting program interpreter: $INTERPRETER" \
    <<<"$programs" || true)
  (( count == 1 )) ||
    fail "final executable lacks canonical AArch64 PT_INTERP: $elf"
}

audit_lld_linker() {
  local elf=$1 comments
  comments=$($READELF -p .comment -- "$elf" 2>/dev/null || true)
  grep -Fq 'Linker: LLD ' <<<"$comments" ||
    fail "Clang output does not prove an LLD link: $elf"
}

audit_aapcs64_disassembly() {
  local elf=$1 disassembly probe entry
  disassembly=$($OBJDUMP -d -- "$elf") ||
    fail "cannot disassemble AAPCS64 probe: $elf"
  probe=$(sed -n '/<nx_callee_saved_probe[^>]*>:/,/^$/p' \
    <<<"$disassembly")
  [[ -n $probe ]] || fail "callee-saved probe symbol missing: $elf"
  grep -Eq 'stp[[:space:]]+x19, x20' <<<"$probe" ||
    fail "probe does not save x19-x20: $elf"
  grep -Eq 'stp[[:space:]]+x27, x28' <<<"$probe" ||
    fail "probe does not save x27-x28: $elf"
  grep -Eq 'stp[[:space:]]+d8, d9' <<<"$probe" ||
    fail "probe does not save d8-d9: $elf"
  grep -Eq 'stp[[:space:]]+d14, d15' <<<"$probe" ||
    fail "probe does not save d14-d15: $elf"
  grep -Eq 'blr[[:space:]]+x9' <<<"$probe" ||
    fail "probe does not perform its AAPCS64 call: $elf"
  grep -Eq 'ldp[[:space:]]+x19, x20' <<<"$probe" ||
    fail "probe does not restore x19-x20: $elf"
  grep -Eq 'ldp[[:space:]]+d8, d9' <<<"$probe" ||
    fail "probe does not restore d8-d9: $elf"
  if grep -Eq 'x18([^0-9]|$)' <<<"$probe"; then
    fail "probe touches platform register x18: $elf"
  fi
  entry=$(sed -n '/<nx_entry_sp_alignment[^>]*>:/,/^$/p' \
    <<<"$disassembly")
  [[ -n $entry ]] || fail "entry-SP probe symbol missing: $elf"
  grep -Eq 'and[[:space:]]+x0, x9, #0xf' <<<"$entry" ||
    fail "entry-SP probe does not check 16-byte alignment: $elf"
}

audit_synthetic_source_contract() {
  local source=$NXLOADER_ROOT/tests/test_aarch64_cross.c flow
  flow=$(sed -n \
    '/nx_synthetic_loader_hook_gate(int \*rwx_seen)/,/^int main(void)/p' \
    "$source")
  [[ -n $flow ]] || fail "cannot isolate synthetic loader source flow"
  grep -Fq 'nxloader_module_load_memory(' "$source" ||
    fail "synthetic gate does not use the memory loader"
  grep -Fq 'nxloader_module_relocate(module)' "$source" ||
    fail "synthetic gate lacks the relocate phase"
  grep -Fq 'nxloader_module_resolve(module, empty_registry' "$source" ||
    fail "synthetic gate does not resolve against its empty registry"
  grep -Fq 'nxloader_module_install_hook(' "$source" ||
    fail "synthetic gate lacks public hook installation"
  grep -Fq 'nxloader_module_finalize(module)' "$source" ||
    fail "synthetic gate lacks the finalize/cache phase"
  grep -Fq 'header->e_shoff = 0;' "$source" ||
    fail "synthetic ELF is not explicitly sectionless"
  grep -Fq 'synthetic_test_elf_loaded=1 external_guest_elf_loaded=0' \
    "$source" || fail "runtime output does not distinguish synthetic/external"
  grep -Fq 'primed_entry_result = entry_function(execution_input);' \
    <<<"$flow" || fail "synthetic flow does not prime the guest entry cache"
  grep -Fq 'primed_pool_result = pool_function(execution_input);' \
    <<<"$flow" || fail "synthetic flow does not prime the veneer-pool cache"
  grep -Fq 'execution_result = entry_function(execution_input);' \
    <<<"$flow" || fail "synthetic flow does not reexecute the guest entry"
  grep -Fq 'pool_execution_result = pool_function(execution_input);' \
    <<<"$flow" || fail "synthetic flow does not reexecute the veneer pool"
  grep -Fq 'PROT_READ | PROT_EXEC' <<<"$flow" ||
    fail "synthetic flow does not establish RX for cache priming"
  grep -Fq 'PROT_READ | PROT_WRITE' <<<"$flow" ||
    fail "synthetic flow does not return primed pages to RW for patching"
  if grep -Fq '__builtin___clear_cache' <<<"$flow"; then
    fail "loader integration flow must leave post-patch cache clear to finalize"
  fi
  if grep -Eq 'PROT_READ[[:space:]]*\|[[:space:]]*PROT_WRITE[[:space:]]*\|[[:space:]]*PROT_EXEC|PROT_READ[[:space:]]*\|[[:space:]]*PROT_EXEC[[:space:]]*\|[[:space:]]*PROT_WRITE' \
      <<<"$flow"; then
    fail "synthetic cache transitions must never request RWX"
  fi
  if grep -Eq 'nxloader_module_load_file[[:space:]]*\(' "$source"; then
    fail "cross-safe source must not load an external ELF file"
  fi
  if grep -Eq 'nxloader_module_call_initializers[[:space:]]*\(' "$source"; then
    fail "cross-safe source must not call guest initializers"
  fi
  if grep -Fq 'JNI_OnLoad' "$source"; then
    fail "cross-safe source must not call or name a guest JNI entry"
  fi
}

audit_synthetic_flow_disassembly() {
  local elf=$1 disassembly flow destination symbol
  disassembly=$($OBJDUMP -d -- "$elf") ||
    fail "cannot disassemble synthetic loader flow: $elf"
  flow=$(sed -n '/<nx_synthetic_loader_hook_gate[^>]*>:/,/^$/p' \
    <<<"$disassembly")
  [[ -n $flow ]] || fail "synthetic loader-flow symbol missing: $elf"
  for symbol in nxloader_module_load_memory nxloader_module_relocate \
                nxloader_module_resolve nxloader_module_install_hook \
                nxloader_module_finalize; do
    grep -Eq "bl[[:space:]].*<${symbol}>" <<<"$flow" ||
      fail "synthetic flow does not call $symbol: $elf"
  done
  destination=$(sed -n \
    '/<nx_synthetic_hook_destination[^>]*>:/,/^$/p' <<<"$disassembly")
  [[ -n $destination ]] || fail "synthetic hook destination missing: $elf"
  grep -Eq '[[:space:]]ret([[:space:]]|$)' <<<"$destination" ||
    fail "synthetic hook destination does not return: $elf"
  if grep -Eq 'x18([^0-9]|$)' <<<"$destination"; then
    fail "synthetic hook destination touches platform register x18: $elf"
  fi
}

audit_synthetic_source_contract

printf 'aarch64-cross: image=%s image_id=%s network=none source_ro=1\n' \
  "$BUSTER_IMAGE" "$resolved_image_id"
printf 'aarch64-cross: clang=%s\n' "$($CLANG --version | head -n 1)"
printf 'aarch64-cross: lld=%s\n' "$($LLD --version | head -n 1)"
printf 'aarch64-cross: qemu=%s\n' "$($QEMU --version | head -n 1)"
printf 'aarch64-cross: readelf=%s\n' "$($READELF --version | head -n 1)"
printf 'aarch64-cross: objdump=%s\n' "$($OBJDUMP --version | head -n 1)"

mkdir -p -- "$EXPORT_ROOT/usr/lib/gcc-cross/aarch64-linux-gnu"
"${docker_base[@]}" \
  --volume "$EXPORT_ROOT:/export:rw" \
  "$BUSTER_IMAGE" bash -ceu '
    umask 022
    cp -a --no-preserve=ownership /usr/aarch64-linux-gnu /export/usr/
    cp -a --no-preserve=ownership \
      /usr/lib/gcc-cross/aarch64-linux-gnu/8 \
      /export/usr/lib/gcc-cross/aarch64-linux-gnu/
  '

[[ -d $SYSROOT && -d $GCC_INSTALL ]] ||
  fail "private Buster sysroot export is incomplete"
[[ -r $SYSROOT$INTERPRETER ]] ||
  fail "private Buster sysroot lacks $INTERPRETER"
[[ -r $SYSROOT/lib/libc.so.6 ]] ||
  fail "private Buster sysroot lacks libc.so.6"

printf 'aarch64-cross: manifest_begin\n'
printf 'image_id=%s\n' "$resolved_image_id"
sha256sum -- "$NXLOADER_ROOT/CMakeLists.txt" \
  "$NXLOADER_ROOT/include/nxloader.h" \
  "$NXLOADER_ROOT/src/nxloader_internal.h" \
  "$NXLOADER_ROOT/src/nxloader.c" \
  "$NXLOADER_ROOT/src/nxloader_elf32.c" \
  "$NXLOADER_ROOT/src/nxloader_elf64.c" \
  "$NXLOADER_ROOT/src/nxloader_hooks.c" \
  "$NXLOADER_ROOT/src/nxloader_protect.c" \
  "$NXLOADER_ROOT/src/nxloader_registry.c" \
  "$NXLOADER_ROOT/tests/test_aarch64_cross.c" \
  "$NXLOADER_ROOT/tests/run-aarch64-cross.sh" \
  "$CLANG" "$LLD" "$QEMU" "$READELF" "$OBJDUMP" \
  "$SYSROOT$INTERPRETER" "$SYSROOT/lib/libc.so.6"
"${docker_base[@]}" "$BUSTER_IMAGE" bash -ceu '
    aarch64-linux-gnu-gcc --version | head -n 1
    sha256sum /usr/bin/aarch64-linux-gnu-gcc \
      /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 \
      /usr/aarch64-linux-gnu/lib/libc.so.6
  '
printf 'aarch64-cross: manifest_end\n'

"${docker_base[@]}" \
  --volume "$NXLOADER_ROOT:/src:ro" \
  --volume "$AARCH64_WORK:/work:rw" \
  --workdir /work \
  "$BUSTER_IMAGE" bash -ceu '
    test ! -w /src/CMakeLists.txt
    cmake -S /src -B /work/gcc -G "Unix Makefiles" \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/usr/bin/aarch64-linux-gnu-gcc \
      -DCMAKE_C_FLAGS_INIT=-fno-omit-frame-pointer \
      -DNXLOADER_BUILD_TESTS=OFF \
      -DNXLOADER_BUILD_TOOLS=OFF \
      -DNXLOADER_BUILD_SOFTFP=OFF \
      -DNXLOADER_BUILD_ARMV7_CROSS_TEST=OFF \
      -DNXLOADER_BUILD_AARCH64_CROSS_TEST=ON \
      -DNXLOADER_BUILD_FUZZER=OFF \
      -DNXLOADER_ENABLE_SANITIZERS=OFF \
      -DNXLOADER_WARNINGS_AS_ERRORS=ON
    cmake --build /work/gcc --target nxloader_aarch64_cross_test \
      --parallel 2
  '

clang_flags="--gcc-install-dir=$GCC_INSTALL -isystem $SYSROOT/include -fno-omit-frame-pointer"
cmake -S "$NXLOADER_ROOT" -B "$CLANG_BUILD" -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CLANG" \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_SYSROOT="$CLANG_SYSROOT" \
  -DCMAKE_C_FLAGS_INIT="$clang_flags" \
  "-DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=lld --gcc-install-dir=$GCC_INSTALL" \
  -DNXLOADER_BUILD_TESTS=OFF \
  -DNXLOADER_BUILD_TOOLS=OFF \
  -DNXLOADER_BUILD_SOFTFP=OFF \
  -DNXLOADER_BUILD_ARMV7_CROSS_TEST=OFF \
  -DNXLOADER_BUILD_AARCH64_CROSS_TEST=ON \
  -DNXLOADER_BUILD_FUZZER=OFF \
  -DNXLOADER_ENABLE_SANITIZERS=OFF \
  -DNXLOADER_WARNINGS_AS_ERRORS=ON
cmake --build "$CLANG_BUILD" --target nxloader_aarch64_cross_test \
  --parallel 2

before_count=$ELF_COUNT
audit_tree "$GCC_BUILD"
GCC_ELF_COUNT=$((ELF_COUNT - before_count))
before_count=$ELF_COUNT
audit_tree "$CLANG_BUILD"
CLANG_ELF_COUNT=$((ELF_COUNT - before_count))

GCC_EXECUTABLE=$GCC_BUILD/nxloader_aarch64_cross_test
CLANG_EXECUTABLE=$CLANG_BUILD/nxloader_aarch64_cross_test
audit_final_executable "$GCC_EXECUTABLE"
audit_final_executable "$CLANG_EXECUTABLE"
audit_lld_linker "$CLANG_EXECUTABLE"
audit_aapcs64_disassembly "$GCC_EXECUTABLE"
audit_aapcs64_disassembly "$CLANG_EXECUTABLE"
audit_synthetic_flow_disassembly "$GCC_EXECUTABLE"
audit_synthetic_flow_disassembly "$CLANG_EXECUTABLE"

if (( EXPECTED_ELF_COUNT > 0 )); then
  (( ELF_COUNT == EXPECTED_ELF_COUNT )) ||
    fail "ELF count changed: $ELF_COUNT != $EXPECTED_ELF_COUNT"
  (( LOADABLE_ELF_COUNT == EXPECTED_LOADABLE_ELF_COUNT )) ||
    fail "loadable ELF count changed"
  (( RELOCATABLE_ELF_COUNT == EXPECTED_RELOCATABLE_ELF_COUNT )) ||
    fail "relocatable ELF count changed"
  (( GCC_ELF_COUNT == EXPECTED_GCC_ELF_COUNT )) ||
    fail "GCC ELF count changed"
  (( CLANG_ELF_COUNT == EXPECTED_CLANG_ELF_COUNT )) ||
    fail "Clang ELF count changed"
fi
[[ $MAX_GLIBC != none ]] || fail "no GLIBC version requirement was measured"

runtime_expected='aarch64-cross: PASS lp64=1 integer_args=1 fp_args=1 stack_align_16=1 callee_saved_gpr=1 callee_saved_fp=1 cache_rewrite=1 loader_lifecycle=1 sectionless=1 relative_relocation=1 hook_veneer=1 hook_execution=1 icache_primed_entry=1 icache_primed_pool=1 post_finalize_reexecution=1 finalize_only_loader_cache_clear=1 loader_cache_finalize=1 wx_mapping=0 synthetic_test_elf_loaded=1 external_guest_elf_loaded=0 guest_elf_loaded=0 guest_initializers_executed=0 device_access=0 hardware_ran=0'
if ! gcc_output=$(env -i LC_ALL=C "$QEMU" -L "$SYSROOT" \
    "$GCC_EXECUTABLE" 2>&1); then
  printf '%s\n' "$gcc_output" >&2
  fail "GCC QEMU execution failed"
fi
if ! clang_output=$(env -i LC_ALL=C "$QEMU" -L "$SYSROOT" \
    "$CLANG_EXECUTABLE" 2>&1); then
  printf '%s\n' "$clang_output" >&2
  fail "Clang/LLD QEMU execution failed"
fi
printf '%s\n' "$gcc_output"
printf '%s\n' "$clang_output"
[[ $gcc_output == "$runtime_expected" ]] ||
  fail "GCC runtime contract mismatch"
[[ $clang_output == "$runtime_expected" ]] ||
  fail "Clang/LLD runtime contract mismatch"

printf 'aarch64-cross: PASS gcc=1 clang=1 lld=1 qemu=1 '
printf 'lp64=2 integer_args=2 fp_args=2 stack_align_16=2 '
printf 'callee_saved_gpr=2 callee_saved_fp=2 cache_rewrite=2 '
printf 'loader_lifecycle=2 sectionless=2 relative_relocation=2 '
printf 'hook_veneer=2 hook_execution=2 icache_primed_entry=2 '
printf 'icache_primed_pool=2 post_finalize_reexecution=2 '
printf 'finalize_only_loader_cache_clear=2 loader_cache_finalize=2 '
printf 'wx_mapping=0 '
printf 'aapcs64_disassembly=2 loader_flow_disassembly=2 '
printf 'elf_count=%d gcc_elf_count=%d ' \
  "$ELF_COUNT" "$GCC_ELF_COUNT"
printf 'clang_elf_count=%d loadable_elf_count=%d ' \
  "$CLANG_ELF_COUNT" "$LOADABLE_ELF_COUNT"
printf 'relocatable_elf_count=%d glibc_max=%s pt_interp=%s ' \
  "$RELOCATABLE_ELF_COUNT" "$MAX_GLIBC" "$INTERPRETER"
printf 'source_ro=1 network_access=0 hardware_ran=0 device_access=0 '
printf 'synthetic_test_elf_loaded=2 external_guest_elf_loaded=0 '
printf 'guest_elf_loaded=0 guest_initializers_executed=0\n'

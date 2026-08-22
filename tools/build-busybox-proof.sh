#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    printf 'usage: %s OUTPUT_DIRECTORY WORK_DIRECTORY\n' "$0" >&2
    exit 2
fi

build_only=${SAPOTE_BUSYBOX_BUILD_ONLY:-0}
if [ "$build_only" != 0 ] && [ "$build_only" != 1 ]; then
    printf 'SAPOTE_BUSYBOX_BUILD_ONLY must be 0 or 1\n' >&2
    exit 2
fi

output_dir=$(realpath -m "$1")
work_dir=$(realpath -m "$2")
repository_root=$(git rev-parse --show-toplevel)

busybox_version=1.38.0
busybox_archive="busybox-${busybox_version}.tar.bz2"
busybox_url="https://busybox.net/downloads/${busybox_archive}"
busybox_sha256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
busybox_config_sha256=3fbc0403c6a4865fc4397240961c367ee9b36d6d350cc6ceb2d22cbbbea28480
busybox_binary_sha256=b308f2cad5b5cd0eeb92a622dec8d71c1a08f628a22cdc5bcde2b98b53220746
musl_version=1.2.6
musl_archive="musl-${musl_version}.tar.gz"
musl_upstream_url="https://musl.libc.org/releases/${musl_archive}"
musl_url="https://sources.buildroot.net/musl/${musl_archive}"
musl_sha256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a

rm -rf "$output_dir" "$work_dir"
mkdir -p "$output_dir" "$work_dir/downloads" "$work_dir/source" \
    "$work_dir/musl-build" "$work_dir/musl-install"

cp "$repository_root/userspace/busybox/source/$busybox_archive" \
    "$work_dir/downloads/$busybox_archive"
cp "$repository_root/userspace/busybox/source/$musl_archive" \
    "$work_dir/downloads/$musl_archive"

printf '%s  %s\n' "$busybox_sha256" \
    "$work_dir/downloads/$busybox_archive" | sha256sum --check --strict
printf '%s  %s\n' "$musl_sha256" \
    "$work_dir/downloads/$musl_archive" | sha256sum --check --strict

tar --extract --bzip2 --file "$work_dir/downloads/$busybox_archive" \
    --directory "$work_dir/source" --no-same-owner --no-same-permissions
tar --extract --gzip --file "$work_dir/downloads/$musl_archive" \
    --directory "$work_dir/source" --no-same-owner --no-same-permissions

musl_source="$work_dir/source/musl-${musl_version}"
busybox_source="$work_dir/source/busybox-${busybox_version}"
musl_cflags='-Os -fno-pie -mcmodel=large -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-tree-vectorize -fno-ident -Wno-return-local-addr'

(
    cd "$work_dir/musl-build"
    "$musl_source/configure" \
        --prefix="$work_dir/musl-install" \
        --disable-shared \
        CC=gcc \
        CFLAGS="$musl_cflags"
    make --jobs=2
    make install
)
# musl-gcc deliberately defaults every non-shared link to Scrt1.o.  This proof
# is a fixed ET_EXEC, so select musl's installed non-PIE crt1.o explicitly.
sed -i \
    -e 's|/Scrt1\.o|/crt1.o|' \
    -e 's| crtbeginS\.o%s||' \
    -e 's|crtendS\.o%s ||' \
    "$work_dir/musl-install/lib/musl-gcc.specs"
grep -Fq "$work_dir/musl-install/lib/crt1.o" \
    "$work_dir/musl-install/lib/musl-gcc.specs"
! grep -Fq '/Scrt1.o' "$work_dir/musl-install/lib/musl-gcc.specs"
! grep -Fq 'crtbeginS.o' "$work_dir/musl-install/lib/musl-gcc.specs"
! grep -Fq 'crtendS.o' "$work_dir/musl-install/lib/musl-gcc.specs"

cp "$repository_root/userspace/busybox/busybox.config" \
    "$work_dir/busybox.config"
printf '%s  %s\n' "$busybox_config_sha256" \
    "$work_dir/busybox.config" | sha256sum --check --strict
(
    cd "$busybox_source"
    export KCONFIG_NOTIMESTAMP=1
    cp "$work_dir/busybox.config" .config
    make silentoldconfig
    cmp --silent .config "$work_dir/busybox.config"
    test "$(grep -Ec '^CONFIG_[A-Z0-9_]+=y$' .config)" -ge 1
    grep -Fxq 'CONFIG_BUSYBOX=y' .config
    grep -Fxq 'CONFIG_STATIC=y' .config
    grep -Fxq 'CONFIG_ECHO=y' .config
    grep -Fxq '# CONFIG_FEATURE_FANCY_ECHO is not set' .config
    make --jobs=2 \
        CC="$work_dir/musl-install/bin/musl-gcc" \
        HOSTCFLAGS='-Wno-unused-result'
)

busybox_binary="$busybox_source/busybox"
cp "$busybox_binary" "$output_dir/busybox"
cp "$busybox_source/.config" "$output_dir/busybox.config"
if [ "$busybox_binary_sha256" != measure ]; then
    printf '%s  %s\n' "$busybox_binary_sha256" \
        "$output_dir/busybox" | sha256sum --check --strict
fi
printf '%s  %s\n' "$busybox_config_sha256" \
    "$output_dir/busybox.config" | sha256sum --check --strict
cp "$busybox_source/LICENSE" "$output_dir/BUSYBOX-LICENSE"
cp "$musl_source/COPYRIGHT" "$output_dir/MUSL-COPYRIGHT"
cp "$work_dir/downloads/$busybox_archive" "$output_dir/$busybox_archive"
cp "$work_dir/downloads/$musl_archive" "$output_dir/$musl_archive"

busybox_size=$(stat --format=%s "$output_dir/busybox")
readelf -W -h "$output_dir/busybox" >"$output_dir/elf-header.txt"
readelf -W -l "$output_dir/busybox" >"$output_dir/elf-program-headers.txt"
readelf -W -r "$output_dir/busybox" >"$output_dir/elf-relocations.txt"
readelf -W -d "$output_dir/busybox" >"$output_dir/elf-dynamic.txt" || true
printf 'measured binary bytes: %s\n' "$busybox_size"
printf 'measured binary SHA-256: %s\n' \
    "$(sha256sum "$output_dir/busybox" | awk '{print toupper($1)}')"
printf 'measured program headers: %s\n' \
    "$(readelf -W -h "$output_dir/busybox" | awk '/Number of program headers:/{print $5}')"
printf 'measured PT_LOAD headers: %s\n' \
    "$(readelf -W -l "$output_dir/busybox" | grep -Ec '^[[:space:]]+LOAD')"
test "$busybox_size" -le $((2 * 1024 * 1024))
test $(((busybox_size + 4095) / 4096)) -le 512
test "$(readelf -W -h "$output_dir/busybox" | awk '/Type:/{print $2}')" = EXEC
test "$(readelf -W -h "$output_dir/busybox" | awk '/Number of program headers:/{print $5}')" -le 8
test "$(readelf -W -l "$output_dir/busybox" | grep -Ec '^[[:space:]]+LOAD')" -le 4
! readelf -W -l "$output_dir/busybox" | grep -Eq \
    '^[[:space:]]+(INTERP|DYNAMIC)|LOAD[[:space:]].*RWE'
! readelf -W -r "$output_dir/busybox" | grep -Eq 'R_X86_64_'
! readelf -W -d "$output_dir/busybox" | grep -Eq '\(NEEDED\)|\(TEXTREL\)'

objdump -d --no-show-raw-insn "$output_dir/busybox" \
    >"$output_dir/busybox-disassembly.txt"
file "$output_dir/busybox" >"$output_dir/file.txt"
sha256sum "$output_dir/busybox" "$output_dir/busybox.config" \
    "$output_dir/$busybox_archive" "$output_dir/$musl_archive" \
    "$output_dir/BUSYBOX-LICENSE" "$output_dir/MUSL-COPYRIGHT" \
    >"$output_dir/SHA256SUMS"

{
    printf 'BusyBox version: %s\n' "$busybox_version"
    printf 'BusyBox source SHA-256: %s\n' "$busybox_sha256"
    printf 'musl version: %s\n' "$musl_version"
    printf 'musl upstream URL: %s\n' "$musl_upstream_url"
    printf 'musl byte-identical mirror URL: %s\n' "$musl_url"
    printf 'musl source SHA-256: %s\n' "$musl_sha256"
    printf 'host gcc: %s\n' "$(gcc --version | head -n 1)"
    printf 'host binutils: %s\n' "$(ld --version | head -n 1)"
    printf 'binary bytes: %s\n' "$busybox_size"
    printf 'FAT16 data clusters at 4096 bytes: %s\n' \
        "$(((busybox_size + 4095) / 4096))"
    printf 'binary SHA-256: %s\n' \
        "$(sha256sum "$output_dir/busybox" | awk '{print toupper($1)}')"
    printf 'configuration SHA-256: %s\n' \
        "$(sha256sum "$output_dir/busybox.config" | awk '{print toupper($1)}')"
} >"$output_dir/build-record.txt"

if [ "$build_only" = 1 ]; then
    exit 0
fi

stdout_file="$output_dir/stdout.txt"
stderr_file="$output_dir/stderr.txt"
trace_file="$output_dir/syscall-trace.txt"
env -i strace --argv0=busybox --quiet=all --string-limit=256 \
    --output="$trace_file" "$output_dir/busybox" echo SAPOTE \
    >"$stdout_file" 2>"$stderr_file"
printf 'SAPOTE\n' | cmp --silent - "$stdout_file"
test ! -s "$stderr_file"
sed -nE 's/^([a-z_][a-z0-9_]*)\(.*/\1/p' "$trace_file" \
    | grep -v '^execve$' >"$output_dir/syscall-sequence.txt"
cat >"$work_dir/expected-syscall-sequence.txt" <<'EOF'
arch_prctl
set_tid_address
brk
brk
mmap
mmap
write
munmap
exit_group
EOF
cmp --silent "$work_dir/expected-syscall-sequence.txt" \
    "$output_dir/syscall-sequence.txt"
test "$(sort -u "$output_dir/syscall-sequence.txt" | wc -l)" -eq 7
grep -Fq 'arch_prctl(ARCH_SET_FS, 0x400001008998)' "$trace_file"
grep -Fq 'set_tid_address(0x400001008b34)' "$trace_file"
grep -Fq 'write(1, "SAPOTE\n", 7)' "$trace_file"
grep -Fq 'exit_group(0)' "$trace_file"

qemu_stdout="$output_dir/qemu-stdout.txt"
qemu_stderr="$output_dir/qemu-stderr.txt"
qemu_trace="$output_dir/exercised-instructions.txt"
env -i qemu-x86_64 -0 busybox -d in_asm -D "$qemu_trace" \
    "$output_dir/busybox" echo SAPOTE \
    >"$qemu_stdout" 2>"$qemu_stderr"
printf 'SAPOTE\n' | cmp --silent - "$qemu_stdout"
test ! -s "$qemu_stderr"
forbidden_instructions=$(grep -Ei \
    '%(xmm|ymm|zmm|mm|k)[0-9]+|^[[:space:]]*0x[0-9a-f]+:[[:space:]]+[0-9a-f ]+[[:space:]]+(f[a-z0-9]+|emms|fxsave|fxrstor|ldmxcsr|stmxcsr|v[a-z0-9]+)([[:space:]]|$)' \
    "$qemu_trace" \
    | grep -Ev '[[:space:]](verr|verw)[[:space:]]' || true)
test -z "$forbidden_instructions" || {
    printf 'BusyBox exercised text contains floating-point or vector instructions:\n%s\n' \
        "$forbidden_instructions" >&2
    exit 1
}

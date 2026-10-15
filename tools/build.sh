#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

k3_pkg=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
k3_edk2=${EDK2_DIR:-"$k3_pkg/../edk2"}
k3_platforms=${EDK2_PLATFORMS_DIR:-"$k3_pkg/../edk2-platforms"}
k3_target=${TARGET:-RELEASE}
k3_jobs=${JOBS:-4}
k3_args=()
case "$k3_target" in
    DEBUG|RELEASE) ;;
    *) echo 'TARGET must be DEBUG or RELEASE' >&2; exit 2 ;;
esac
if [[ -n ${DEBUG_PRINT_ERROR_LEVEL:-} ]]; then
    k3_args+=(-D "DEBUG_PRINT_ERROR_LEVEL=$DEBUG_PRINT_ERROR_LEVEL")
fi

for k3_option in "$@"; do
    case "$k3_option" in
        --gop) k3_args+=(-D K3_GOP_ENABLE=TRUE) ;;
        --no-gop) k3_args+=(-D K3_GOP_ENABLE=FALSE) ;;
        *) echo "Unknown build option: $k3_option" >&2; exit 2 ;;
    esac
done
# Sourced edksetup.sh must not inherit this wrapper's build options.
set --

[[ -f "$k3_edk2/edksetup.sh" ]] || {
    echo 'Set EDK2_DIR to an EDK2 source checkout' >&2
    exit 2
}
[[ -d "$k3_platforms/Silicon/RISC-V" ]] || {
    echo 'Set EDK2_PLATFORMS_DIR to the official edk2-platforms checkout' >&2
    exit 2
}
k3_edk2=$(cd -- "$k3_edk2" && pwd)
k3_x64emu=${X64EMU_ENABLE:-TRUE}
case "$k3_x64emu" in
    TRUE|FALSE) ;;
    *) echo 'X64EMU_ENABLE must be TRUE or FALSE' >&2; exit 2 ;;
esac
if [[ "$k3_x64emu" == TRUE ]]; then
    make -C "$k3_pkg/../MultiArchUefiPkg" EDK2_DIR="$k3_edk2" \
        MAU_BUILD_DIR="$k3_pkg/build/multiarch" TARGET="$k3_target" JOBS="$k3_jobs" MAU_DYNAMIC_POLICY=TRUE \
        CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}" PYTHON="${PYTHON:-python3}"
    k3_emu="$k3_pkg/build/multiarch/Build/MultiArchUefiPkg/${k3_target}_GCC/RISCV64"
    k3_args+=(-D X64EMU_ENABLE=TRUE)
else
    k3_args+=(-D X64EMU_ENABLE=FALSE)
fi

k3_platforms=$(cd -- "$k3_platforms" && pwd)
mkdir -p "$k3_pkg/build/workspace" "$k3_pkg/build/logs"
export WORKSPACE="$k3_pkg/build/workspace"
if [[ "$k3_x64emu" == TRUE ]]; then
    mkdir -p "$WORKSPACE/X64EmulatorBin"
    cp "$k3_emu/EmulatorDxe.efi" "$WORKSPACE/X64EmulatorBin/"
    cp "$k3_emu/MultiArchUefiPkg/Drivers/Emulator/Emulator/OUTPUT/EmulatorDxe.depex" "$WORKSPACE/X64EmulatorBin/"
fi
export PACKAGES_PATH="$WORKSPACE:$k3_pkg/..:$k3_pkg:$k3_platforms:$k3_edk2"
export GCC_RISCV64_PREFIX=${CROSS_COMPILE:-riscv64-linux-gnu-}
export PYTHON_COMMAND=${PYTHON:-python3}

make -C "$k3_edk2/BaseTools/Source/C" -j"$k3_jobs" \
    > "$k3_pkg/build/logs/basetools.log" 2>&1
set +u
source "$k3_edk2/edksetup.sh" > "$k3_pkg/build/logs/environment.log" 2>&1
set -u
build -a RISCV64 -p SpacemitPlatformPkg/K3/PicoItx/PicoItx.dsc \
    -t GCC -b "$k3_target" -n "$k3_jobs" "${k3_args[@]}" \
    > "$k3_pkg/build/logs/firmware.log" 2>&1
printf 'Firmware: %s\n' \
    "$WORKSPACE/Build/SpacemitK3PicoItx/${k3_target}_GCC/FV/SPACEMITK3.fd"

# edk2-ruyi

EDK2 platform support for [ruyifirmware](https://github.com/openRuyi-Project/ruyifirmware).

Currently, only SpacemiT K3 Pico ITX is supported.

UEFI Setup → UEFI Driver Emulation controls non-native UEFI drivers.
Emulation is disabled by default. Save and restart to apply; the setting
is stored in a dedicated SPI sector, including when saved from RAM
recovery. Firmware updates reset emulation to disabled.
Onboard option ROMs use x64 by default. AMD GCN display devices in the
platform PCI ID list instead use AMD ARM64 GOP 1.68 for early GCN,
Polaris and Vega M. Other devices, including Vega 10/12/20 and RDNA,
retain onboard x64 GOPs. The GCN replacement path is experimental.
Image and binding policies select one driver family per controller while
preserving the original ROM data. Failed replacements do not fall back
to the onboard x64 GOP. Developer builds with `X64EMU_ENABLE=FALSE` omit
the emulator and its setup option.

The build fetches unmodified `Arm64Gop_1_68.efi` from AMD's RN-AAR
release and checks its fixed SHA-256 hash. Set `AMD_GOP_DIR` to a
directory containing this file to build without downloading it.

UEFI Setup → CPU Configuration → CPU mode selects one of three profiles:

- X100 only: eight X100 cores with the original CPU features (default).
- X100 + A100: sixteen cores without vector or hypervisor extensions.
- A100 only: eight A100 cores with vectors and without hypervisor support.

Save and restart to apply. A100-only mode transfers the UEFI boot hart to
A100 before handing its CPU description to Linux. Firmware updates restore
X100 only; UEFI variables are preserved. CPU selection is stored separately
and is persistent even when changed from RAM recovery.

Mixed mode needs userspace that can run without V, such as RV64GC. An
RVA23U64 root filesystem is unsuitable for that mode.

The OS handoff DT identifies the selected profile to userspace: `k3` for
X100 only, `k3-x100-a100` for mixed mode, and `k3-a100` for A100 only.
Firmware generates these descriptions without Linux source or binding changes.

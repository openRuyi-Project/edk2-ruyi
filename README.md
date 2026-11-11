# edk2-ruyi

EDK2 platform support for [ruyifirmware](https://github.com/openRuyi-Project/ruyifirmware).
Use ruyifirmware to build firmware images for these boards.

| Platform package | Board | Documentation |
| --- | --- | --- |
| `SpacemitPlatformPkg` | SpacemiT K3 Pico ITX | [K3](SpacemitPlatformPkg/README.md) |
| `SiFivePlatformPkg` | SiFive HiFive Unmatched A00 | [Unmatched](SiFivePlatformPkg/README.md) |
| `EswinPlatformPkg` | SiFive HiFive Premier P550 | [P550](https://github.com/openRuyi-Project/ruyifirmware/blob/ruyi/docs/p550/README.md) |

In UEFI Setup, System Information displays the active CPU profile and clocks,
installed memory, PCIe devices and link speeds, and whole-disk information.
Each page can refresh information from devices already enumerated by UEFI.
Unavailable details are shown as unknown.

In UEFI Setup, open Platform Settings and select UEFI driver emulation
to control non-native UEFI drivers. Ruyi Firmware packages enable
emulation by default. Save a menu choice and restart to apply it.

The shared settings protocol stores defaults through a platform backend.
UEFI variables can optionally override them. See [Platform settings](RuyiPlatformPkg/PlatformSettings.md).
The updater uses these policies:

| Update through Firmware Update | K3 | Unmatched | P550 |
| --- | --- | --- | --- |
| UEFI variables | Preserved | Preserved | Preserved |
| Platform defaults | Preserved | Preserved | Preserved |
| CPU mode | Preserved | Not applicable | Not applicable |
| DDR ECC setting | Not applicable | Not applicable | Preserved |

Raw programming bypasses the updater's preservation policy. The Unmatched
8 MiB image excludes UEFI variable storage. A full-chip erase removes it.

## Graphics drivers

Onboard option ROMs use x64 by default. AMD GCN display devices in the
platform PCI ID list instead use AMD ARM64 GOP 1.68 for early GCN,
Polaris and Vega M. Other devices, including Vega 10/12/20 and RDNA,
retain onboard x64 GOPs. The GCN replacement path is experimental.
Image and binding policies select one driver family per controller while
preserving the original ROM data. Failed replacements do not fall back
to the onboard x64 GOP. Builds with `X64EMU_ENABLE=FALSE` omit the emulator
and hide its Setup option on all boards.

The build fetches unmodified `Arm64Gop_1_68.efi` from AMD's RN-AAR
release and checks its fixed SHA-256 hash. Set `AMD_GOP_DIR` to a
directory containing this file to build without downloading it.

See the platform documentation for CPU modes, build options and installation.

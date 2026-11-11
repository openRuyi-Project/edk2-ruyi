# SpacemiT K3

This package provides UEFI support for the SpacemiT K3 Pico ITX.
coreboot initializes memory and clocks. OpenSBI provides machine-mode
services. EDK2 provides device protocols, Setup and USB Fastboot.

## CPU modes

In UEFI Setup, open Platform Settings and select CPU mode.

| Mode | CPU cores | Vector extensions | Hypervisor extensions | OS CPU name |
| --- | --- | --- | --- | --- |
| X100 only, default | 8 X100 | Preserved | Preserved | `k3` |
| X100 + A100 | 16 | Disabled | Disabled | `k3-x100-a100` |
| A100 only | 8 A100 | Enabled | Disabled | `k3-a100` |

Save the setting and restart to apply it. The SPI record also persists
when saved from RAM recovery. A100-only mode transfers the UEFI boot hart
to A100 before handing the CPU description to Linux.

X100 + A100 mode requires userspace that runs without V, such as RV64GC.
An RVA23U64 root filesystem cannot run in this mode.
Firmware adds the selected CPU profile to the OS device tree.
This does not require changes to Linux source or bindings.

## Clock

UEFI reads the P1 hardware RTC during boot. Time setting and alarms are
not supported in UEFI. After ExitBootServices, UEFI time services return
unsupported and Linux uses its native P1 RTC driver.

## Firmware updates

Firmware Update and the Fastboot `firmware` target preserve UEFI variables,
valid platform defaults and CPU selection. They clear DDR training data.
A raw write to the Fastboot `spi` target replaces the entire flash.

See the [K3 build and installation guide](https://github.com/openRuyi-Project/ruyifirmware/blob/ruyi/docs/k3/README.md).

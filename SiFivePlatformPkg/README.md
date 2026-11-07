# SiFive HiFive Unmatched

This package provides UEFI support for HiFive Unmatched A00 with FU740.
coreboot initializes memory and clocks. OpenSBI provides machine-mode
services. EDK2 provides PCIe, storage, USB, Setup and SPI-backed variables.

Unmatched CPU cores do not support RISC-V vector instructions.
Use an OS and userspace that support RV64GC.

## Configuration

In UEFI Setup, open Platform Settings to change UEFI driver emulation.
Save the setting and restart to apply it. Ruyi Firmware release images
initially enable it. Direct EDK2 builds use `MAU_DEFAULT_ENABLE` for the
fallback when no valid SPI setting exists.

PXE and HTTP Boot drivers are included in every image. Both boot methods
are disabled by default. Enable them separately in Platform Settings,
then save and restart.

## Firmware updates

Firmware Update accepts `normal-spi.bin` from a local filesystem.
Keep its matching `SHA256SUMS` file in the same directory.
The update preserves platform settings and UEFI variables.
A complete JTAG SPI reflash replaces both regions.

Unmatched does not provide Fastboot firmware updates.

See the [Unmatched build and installation guide](https://github.com/openRuyi-Project/ruyifirmware/blob/ruyi/docs/unmatched/README.md).

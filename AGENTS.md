# RISC-V UEFI address mapping

When Boot Services enable paging, identity-map every range published by
`GetMemoryMap()` (VA = PA). Check that both ends of each range are valid
virtual addresses in the selected paging mode. Keep any private CPU aliases
for other MMIO inside the driver. Use real physical addresses in EFI memory
descriptors and hardware resource descriptions.

Runtime services must also work in physical addressing mode before
`SetVirtualAddressMap()`. After a successful switch, use the virtual
addresses assigned by the OS for runtime pointers. Preserve physical
addresses wherever an interface requires them.

See the [UEFI RISC-V platform binding](https://uefi.org/specs/UEFI/2.11/02_Overview.html#risc-v-platforms)
and [UEFI runtime services](https://uefi.org/specs/UEFI/2.11/08_Services_Runtime_Services.html#setvirtualaddressmap).

# Firmware console layout

Keep the serial console and UEFI Setup at 80 columns by 25 rows on every
board. Render the 80×25 graphics console across the entire active GOP
resolution. Use the same cell boundaries for text, background, cursor,
scrolling and the Setup banner. Keep the boot logo's aspect ratio.

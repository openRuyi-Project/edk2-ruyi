# Platform settings

`RUYI_PLATFORM_SETTINGS_PROTOCOL` separates persistent defaults from optional
UEFI variable overrides. It is a boot-services protocol.

| Method | Operation |
| --- | --- |
| `GetDefaults` | Read the selected persistent record. |
| `SetDefaults` | Save the supplied defaults in the inactive storage slot. |
| `GetEffective` | Read defaults, then apply a valid UEFI override if one exists. |
| `ClearOverrides` | Delete the override variable without changing defaults. |

The values use `RUYI_SETTING_EMULATION`, `RUYI_SETTING_PXE` and
`RUYI_SETTING_HTTP`. Unknown bits are rejected. There is no old-record parser
or migration path. Install a complete firmware image when moving to this
format. Tools and images must come from the same release.

## Persistent storage

`PlatformSettingsStorageLib` provides slot reads, partial writes and slot
preparation. Each platform reserves two independent slots. Preparing a slot
must fill it with `0xff`. Writes must preserve bytes outside the requested
range. An SPI implementation can erase a sector. An eMMC implementation can
use block writes in a board-reserved boot area. The interface does not depend
on UEFI variable storage.

The current K3, Unmatched and P550 adapters use SPI. A new eMMC-only platform
must supply its own adapter and reserved layout before enabling the driver.

Each 24-byte record contains the `RYCF` magic, version, size, generation,
values, CRC32 and `DONE` marker. The CRC covers the first 16 bytes. The driver
chooses the newest valid generation. It verifies the inactive record before
writing the commit marker. A failed save leaves the previous slot valid.
An absent record returns `EFI_NOT_FOUND`; a caller can initialize it through
`SetDefaults` or install an image containing the new records.

## Optional override

The variable `PlatformSettingsOverride` uses
`gRuyiPlatformSettingsProtocolGuid`. Its data is `RUYI_SETTINGS_OVERRIDE`:
two little-endian UINT32 fields, `Mask` and `Values`. Bits outside `Mask`
must be zero. The effective value is:

```c
(Defaults & ~Override.Mask) | Override.Values
```

`GetEffective` reads the variable on every call. A missing variable or
unavailable variable service leaves defaults in effect. Other read errors
and malformed overrides are reported to the caller. The override does not
modify persistent records.

The driver applies the emulation choice before releasing the emulator's
DXE dependency. It waits for variable services when available and also
handles EndOfDxe for platforms without them. Changing policy does not unload
active drivers. Restart to apply changes to emulation and network boot.

## Setup and updates

Setup displays and edits persistent defaults, not effective values. A valid
UEFI override still takes priority after saving defaults. Restore Defaults
reloads the stored values and discards pending edits. Clear UEFI overrides
removes the optional override layer. CPU mode and DDR ECC use platform providers because
earlier boot stages need those values before UEFI runs.

A normal firmware update preserves valid persistent defaults, UEFI variables
where supported, and supported CPU/ECC settings. K3 DDR training data is still
cleared. A raw image installation replaces the persistent records with those
in the image. It does not necessarily remove UEFI overrides. For example,
Unmatched keeps UEFI variables outside its 8 MiB installation image.

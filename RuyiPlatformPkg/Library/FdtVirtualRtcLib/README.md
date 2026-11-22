# Virtual RTC for device-tree platforms

Select `FdtVirtualRtcLib` as the platform's `RealTimeClockLib` and bind
`FdtRtcSourceLib` to a boot-time reader. Set `PcdRtcLibStoresTimeSettings`
to `TRUE` so EFI SetTime saves the calendar and time zone in one variable.
The virtual RTC takes one sample
from the source during DXE initialization. It accepts the device tree
from the FDT HOB or the EFI configuration table. If the sample fails,
it starts from the last EFI SetTime value, or the build time when no valid
value has been saved.

For an enabled `dallas,ds1307` node at address `0x68`, select
`FdtDs1307RtcSourceLib` and bind `FdtI2cReadLib` to the platform's I2C
reader. Other RTC types can provide their own `FdtRtcSourceLib`.

The platform's `TimerLib` must keep counting across ExitBootServices.
The time for a full counter cycle must exceed the longest supported
interval between initializing or setting the clock and reading it. Both counter directions
are supported. Set `PcdVirtualRtcTimeVariableName` to retain an existing
fallback variable name when migrating a platform.

[SG2042 DSC library bindings](../../../SophgoPlatformPkg/SG2042/SG2042.dsc)

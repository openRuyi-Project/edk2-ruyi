# Virtual RTC for device-tree platforms

Select `FdtVirtualRtcLib` as the platform's `RealTimeClockLib` and bind
`FdtRtcSourceLib` to a boot-time reader. Set `PcdRtcLibStoresTimeSettings`
to `TRUE` so EFI SetTime saves the calendar and time zone in one variable.
The virtual RTC takes one sample
from the source during DXE initialization. It accepts the device tree
from the FDT HOB or the EFI configuration table.

The platform's `TimerLib` must keep counting across ExitBootServices.
The time for a full counter cycle must exceed the longest supported
interval between initializing or setting the clock and reading it. Both counter directions
are supported. Set `PcdVirtualRtcTimeVariableName` to retain an existing
fallback variable name when migrating a platform.

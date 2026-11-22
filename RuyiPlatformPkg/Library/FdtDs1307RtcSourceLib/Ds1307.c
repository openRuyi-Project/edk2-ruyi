/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtI2cReadLib.h>
#include <Library/FdtLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/FdtRtcSourceLib.h>

EFI_STATUS
EFIAPI
FdtRtcReadTime (
  IN  CONST VOID  *Fdt,
  OUT EFI_TIME    *Time
  )
{
  if (Fdt == NULL || Time == NULL) { return EFI_INVALID_PARAMETER; }
  INT32 Node = -1;
  do {
    Node = FdtNodeOffsetByCompatible (Fdt, Node, "dallas,ds1307");
    if (Node < 0) { return EFI_NOT_FOUND; }
  } while (!FdtNodeEnabled (Fdt, Node));

  INT32 Len;
  CONST UINT32 *Reg = FdtGetProp (Fdt, Node, "reg", &Len);
  if (Reg == NULL || Len != sizeof (*Reg) || Fdt32ToCpu (*Reg) != 0x68) { return EFI_UNSUPPORTED; }

  UINT8 Data[7];
  EFI_STATUS Status = FdtI2cReadRegisters (Fdt, FdtParentOffset (Fdt, Node), 0x68, 0, Data, sizeof (Data));
  if (EFI_ERROR (Status)) { return Status; }
  if (Data[0] & BIT7) { return EFI_NOT_READY; }

  UINT8 Hour = Data[2] & ((Data[2] & BIT6) ? 0x1f : 0x3f);
  UINT8 Fields[] = { Data[0], Data[1], Hour, Data[4], Data[5], Data[6] };
  for (UINTN I = 0; I < ARRAY_SIZE (Fields); I++) {
    if ((Fields[I] & 15) > 9 || (Fields[I] >> 4) > 9) { return EFI_DEVICE_ERROR; }
  }
  *Time = (EFI_TIME) {
    .Year = 2000 + BcdToDecimal8 (Data[6]), .Month = BcdToDecimal8 (Data[5]),
    .Day = BcdToDecimal8 (Data[4]), .Hour = BcdToDecimal8 (Hour),
    .Minute = BcdToDecimal8 (Data[1]), .Second = BcdToDecimal8 (Data[0]),
    .TimeZone = EFI_UNSPECIFIED_TIMEZONE
  };
  if (Data[2] & BIT6) {
    if (Time->Hour < 1 || Time->Hour > 12) { return EFI_DEVICE_ERROR; }
    Time->Hour = Time->Hour % 12 + ((Data[2] & BIT5) ? 12 : 0);
  }
  return IsTimeValid (Time) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

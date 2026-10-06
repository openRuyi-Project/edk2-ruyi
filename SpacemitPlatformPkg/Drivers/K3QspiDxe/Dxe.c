/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/RiscVSecHobData.h>
#include <Protocol/Clock.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/HobLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "Qspi.h"

extern EFI_GUID            gEdk2JedecSfdpSpiDxeDriverGuid;
STATIC K3_QSPI             mQspi;
STATIC EFI_EVENT           mExit;
STATIC EFI_SPI_PART        mPart = {
  .Vendor             = L"JEDEC",
  .PartNumber         = L"SPI NOR",
  .ChipSelectPolarity = FALSE
};
STATIC EFI_SPI_BUS         mBus   = { .FriendlyName = L"K3 QSPI" };
STATIC EFI_SPI_PERIPHERAL  mFlash = {
  .FriendlyName            = L"K3 firmware NOR",
  .SpiPeripheralDriverGuid = &gEdk2JedecSfdpSpiDxeDriverGuid,
  .SpiPart                 = &mPart,
  .SpiBus                  = &mBus
};
STATIC struct {
  MEMMAP_DEVICE_PATH          Memory;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH,              HW_MEMMAP_DP,                   { sizeof (MEMMAP_DEVICE_PATH), 0 }
    },
    EfiMemoryMappedIO, 0, 0
  },
  { END_DEVICE_PATH_TYPE,              END_ENTIRE_DEVICE_PATH_SUBTYPE,
      { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_STATUS EFIAPI
UpdatePeripheral (
  CONST EFI_SPI_IO_PROTOCOL  *This,
  CONST EFI_SPI_PERIPHERAL   *Peripheral
  )
{
  if ((This != &mQspi.Io) || !Peripheral || (Peripheral->SpiBus != &mBus) || !Peripheral->SpiPart) {
    return EFI_INVALID_PARAMETER;
  }

  // This board connection is fixed. Socketed replacement is not supported.
  return Peripheral == &mFlash ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC VOID EFIAPI
ExitBootServices (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  mQspi.Exited = TRUE;
}

STATIC EFI_STATUS
Resources (
  CONST VOID  *Fdt,
  INT32       *Node,
  UINT32      *MaximumHz
  )
{
  INT32  Current  = -1;
  INT32  Selected = -1;

  while ((Current = FdtNodeOffsetByCompatible (Fdt, Current, "spacemit,k3-qspi")) >= 0) {
    if (K3FdtEnabled (Fdt, Current)) {
      if (Selected >= 0) {
        return EFI_UNSUPPORTED;
      }

      Selected = Current;
    }
  }

  if (Selected < 0) {
    return EFI_NOT_FOUND;
  }

  UINTN       Window, Size;
  EFI_STATUS  Status = K3FdtRegister (Fdt, Selected, 0, 0x1000, &mQspi.Base, NULL);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3FdtRegister (Fdt, Selected, 1, SIZE_8MB, &Window, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((mQspi.Base != 0xd420c000) || (Window != 0xb8000000) ||
      (Size > SIZE_16MB) || Size & 1023 || (Window > MAX_UINT32 - Size))
  {
    return EFI_UNSUPPORTED;
  }

  INT32  Flash = -1;

  for (Current = FdtFirstSubnode (Fdt, Selected); Current >= 0; Current = FdtNextSubnode (Fdt, Current)) {
    if (!K3FdtEnabled (Fdt, Current)) {
      continue;
    }

    if ((Flash >= 0) || (FdtStringListSearch (Fdt, Current, "compatible", "jedec,spi-nor") < 0)) {
      return EFI_UNSUPPORTED;
    }

    Flash = Current;
  }

  if (Flash < 0) {
    return EFI_NOT_FOUND;
  }

  INT32         Length;
  CONST UINT32  *Property = FdtGetProp (Fdt, Flash, "reg", &Length);

  if (!Property || (Length != 4) || (Fdt32ToCpu (*Property) != 0)) {
    return EFI_UNSUPPORTED;
  }

  Property = FdtGetProp (Fdt, Flash, "spi-max-frequency", &Length);
  if (!Property || (Length != 4) || (Fdt32ToCpu (*Property) < 25600000)) {
    return EFI_UNSUPPORTED;
  }

  *MaximumHz        = Fdt32ToCpu (*Property);
  mQspi.Window      = Window;
  mQspi.WindowBytes = Size;
  *Node             = Selected;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
K3QspiInitialize (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST EFI_GUID           HandoffGuid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE        *Hob        = GetFirstGuidHob (&HandoffGuid);
  RISCV_SEC_HANDOFF_DATA   Handoff;
  INT32                    Node;
  UINT32                   MaximumHz, Bus, Serial, ResetSerial, ResetBus;
  SPACEMIT_CLOCK_PROTOCOL  *Clock;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  CONST VOID  *Fdt = Handoff.FdtPointer;

  if (!Fdt || FdtCheckHeader (Fdt) || (FdtTotalSize (Fdt) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  EFI_STATUS  Status = Resources (Fdt, &Node, &MaximumHz);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Clock->Resolve (Fdt, Node, "qspi_en", FALSE, &Bus);
  if (!EFI_ERROR (Status)) {
    Status = Clock->Resolve (Fdt, Node, "qspi", FALSE, &Serial);
  }

  if (!EFI_ERROR (Status)) {
    Status = Clock->Resolve (Fdt, Node, NULL, TRUE, &ResetSerial);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  INT32         Length;
  CONST UINT32  *Resets = FdtGetProp (Fdt, Node, "resets", &Length);

  if (!Resets || (Length != 16) || (Resets[0] != Resets[2]) ||
      (Bus != 36) || (Serial != 35) || (ResetSerial != 21) || (Fdt32ToCpu (Resets[3]) != 22))
  {
    return EFI_COMPROMISED_DATA;
  }

  ResetBus = Fdt32ToCpu (Resets[3]);
  Status   = K3MapMmio (mQspi.Base, 0x1000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Clock->Enable (Bus, TRUE);
  if (!EFI_ERROR (Status)) {
    Status = Clock->Reset (ResetBus, FALSE);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Do not take over a controller which still has IP/AHB/XIP activity.
  if ((MmioRead32 (mQspi.Base + 0x15c) & 7) || (MmioRead32 (mQspi.Base + 0x160) & BIT1)) {
    return EFI_ACCESS_DENIED;
  }

  Status = K3ApplyPins (Fdt, Node);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "K3 QSPI pinctrl failed: %r\n", Status));
    return Status;
  }

  Status = Clock->Enable (Serial, FALSE);

  if (!EFI_ERROR (Status)) {
    Status = Clock->Reset (ResetSerial, FALSE);
  }

  UINT64  Rate;

  if (!EFI_ERROR (Status)) {
    Status = Clock->SetRate (Serial, 25600000, &Rate);
    if (!EFI_ERROR (Status) && ((Rate != 25600000) || (Rate > MaximumHz))) {
      Status = EFI_UNSUPPORTED;
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = Clock->Enable (Serial, TRUE);
  }

  // K3 frequency change requires the reset released. Pulse resets only
  // after the functional and bus clocks are running.
  if (!EFI_ERROR (Status)) {
    Status = Clock->Reset (ResetSerial, TRUE);
  }

  if (!EFI_ERROR (Status)) {
    Status = Clock->Reset (ResetBus, TRUE);
  }

  if (!EFI_ERROR (Status)) {
    MicroSecondDelay (1);
    Status = Clock->Reset (ResetBus, FALSE);
  }

  if (!EFI_ERROR (Status)) {
    Status = Clock->Reset (ResetSerial, FALSE);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "K3 QSPI clock/reset failed: %r; references retained\n", Status));
    return Status;
  }

  mQspi.Signature = K3_QSPI_SIGNATURE;
  mQspi.ClockHz   = (UINT32)Rate;
  Status          = K3QspiControllerInitialize (&mQspi);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mPart.MaxClockHz               = MaximumHz;
  mFlash.MaxClockHz              = MaximumHz;
  mBus.Peripherallist            = &mFlash;
  mBus.ControllerPath            = (EFI_DEVICE_PATH_PROTOCOL *)&mPath;
  mPath.Memory.StartingAddress   = mQspi.Base;
  mPath.Memory.EndingAddress     = mQspi.Base + 0xfff;
  mQspi.Io.SpiPeripheral         = &mFlash;
  mQspi.Io.OriginalSpiPeripheral = &mFlash;
  mQspi.Io.FrameSizeSupportMask  = BIT7;
  mQspi.Io.MaximumTransferBytes  = K3_QSPI_DATA_BYTES;
  mQspi.Io.Transaction           = K3QspiTransaction;
  mQspi.Io.UpdateSpiPeripheral   = UpdatePeripheral;
  // Check transport reads before exposing the controller to the NOR driver.
  UINT8  IdCommand      = 0x9f;
  UINT8  Id[3]          = { 0 };
  UINT8  SfdpCommand[5] = { 0x5a, 0, 0, 0, 0 };
  UINT8  SfdpHeader[8]  = { 0 };

  Status = K3QspiTransaction (
             &mQspi.Io,
             SPI_TRANSACTION_WRITE_THEN_READ,
             FALSE,
             0,
             1,
             8,
             sizeof (IdCommand),
             &IdCommand,
             sizeof (Id),
             Id
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3QspiTransaction (
             &mQspi.Io,
             SPI_TRANSACTION_WRITE_THEN_READ,
             FALSE,
             0,
             1,
             8,
             sizeof (SfdpCommand),
             SfdpCommand,
             sizeof (SfdpHeader),
             SfdpHeader
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  ExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExit
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EFI_HANDLE  Handle = NULL;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mPath,
                  &gEdk2JedecSfdpSpiDxeDriverGuid,
                  &mQspi.Io,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mExit);
  }

  return Status;
}

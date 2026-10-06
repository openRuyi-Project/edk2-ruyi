/* SPDX-License-Identifier: BSD-2-Clause-Patent */

/* Cold-start native GOP. Retain the framebuffer and controller state
 * through OS handoff. Live DMA drain and mode replacement are not yet
 * supported; expose one validated EDID mode and refuse driver unload. */
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/K3DpuLib.h>
#include <Library/K3DisplayClockLib.h>
#include <Library/K3DisplayModeLib.h>
#include <Library/K3GopLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/K3InnoDpLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC K3_DISPLAY_CLOCK  Clock;
STATIC SOC_DP_DEV        Dp;
STATIC K3_DPU            Dpu;
STATIC K3_GOP            *Gop;
STATIC BOOLEAN           Starting;
STATIC BOOLEAN           RecoveryAttempted;
STATIC UINT64            LastRecovery;
STATIC EFI_HANDLE        DisplayHandle;
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} DisplayPath = {
  {
    { HARDWARE_DEVICE_PATH,              HW_VENDOR_DP,
        { sizeof (VENDOR_DEVICE_PATH),       0 }
    },
    { 0xfcc8227c,                        0x8f11,                        0x4f9f,
        { 0x85,                              0xe9,                          0x61, 0x19,0x89, 0x56, 0x33, 0x40 }
    }
  },
  { END_DEVICE_PATH_TYPE,              END_ENTIRE_DEVICE_PATH_SUBTYPE,
        { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_STATUS EFIAPI
Unload (
  EFI_HANDLE  Image
  )
{
  /* A loaded image owns the callbacks, even if initial startup failed
     after taking hardware ownership. Only a system reset releases them. */
  return EFI_ACCESS_DENIED;
}

STATIC EFI_STATUS
SelectPixel (
  VOID                 *Priv,
  UINT64               Hertz,
  K3_PIXEL_CLOCK_PLAN  *Plan
  )
{
  UINT32  Actual;

  if (!Plan || Hertz % 1000 || (Hertz > 750000000)) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = SocDpStatusToEfi (
                         SocDpPhyRoundPixelClk (
                           SOC_DP_REF_CLK_24M,
                           (UINT32)(Hertz / 1000),
                           &Actual
                           )
                         );

  if (!EFI_ERROR (Status)) {
    Plan->RequestedHz = Hertz;
    Plan->ActualHz    = (UINT64)Actual * 1000;
  }

  return Status;
}

STATIC EFI_STATUS
ColdResetState (
  VOID   *Priv,
  UINTN  BudgetUs
  )
{
  /* OFF->ON with reset held produced this fresh engine. No DMA has been
     started by this driver. After first use, retain all resources and
     recover through the verified whole-system reset instead of freeing. */
  if (!Clock.PowerAdded || !Clock.Ready || Clock.DmaStarted) {
    return EFI_UNSUPPORTED;
  }

  UINT32  Scene  = MmioRead32 (Dpu.Resources.Dpu + 0x340);
  UINT32  Timing = MmioRead32 (Dpu.Resources.Dpu + 0x51200);

  if ((Scene & (0x3ff | BIT18 | (7U << 23))) || (Timing & BIT2)) {
    return EFI_ACCESS_DENIED;
  }

  /* A composer layer default does not initiate DMA with the fresh scene
     and timing disabled. Normalize it before the first scanout only. */
  MmioAnd32 (Dpu.Resources.Dpu + 0x30000, (UINT32) ~BIT0);
  EFI_STATUS  Status = EFI_SUCCESS;

  if (Starting) {
    Status = K3DisplayMarkDma (&Clock);
  }

  return Status;
}

STATIC EFI_STATUS
Start (
  VOID                   *Priv,
  K3_DISPLAY_SURFACE     *Surface,
  CONST K3_DISPLAY_MODE  *Mode
  )
{
  EFI_STATUS  Status = SocDpStatusToEfi (SocDpModeSet (&Dp, &Mode->Timing));

  if (!EFI_ERROR (Status)) {
    Starting = TRUE;
    Status   = K3DpuStart (&Dpu, Surface, &Mode->Timing, Clock.Pixel.ActualHz);
    Starting = FALSE;
  }

  if (!EFI_ERROR (Status)) {
    Status = SocDpStatusToEfi (SocDpHwEnable (&Dp));
  }

  return Status;
}

STATIC EFI_STATUS
Stop (
  VOID  *Priv
  )
{
  return K3DpuStop (&Dpu);
}

STATIC EFI_STATUS
RecoverOutput (
  VOID
  )
{
  EFI_EDID_DISCOVERED_PROTOCOL  *Edid = NULL;
  UINT8                         Block[128];

  K3GopEdid (Gop, &Edid, NULL);
  if (!Edid || !Edid->Edid || !Edid->SizeOfEdid) {
    return EFI_COMPROMISED_DATA;
  }

  EFI_STATUS  Status = SocDpStatusToEfi (SocDpHwReadSinkCaps (&Dp));

  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Recover the published mode only for the same EDID. A different
     monitor needs mode discovery and a separate buffer/mode transition. */
  for (UINT32 Offset = 0; Offset < Edid->SizeOfEdid; Offset += 128) {
    Status = SocDpStatusToEfi (
               SocDpConnGetEdidBlock (&Dp, Block, Offset / 128, 128)
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (CompareMem (Block, Edid->Edid + Offset, sizeof (Block))) {
      return EFI_NO_MEDIA;
    }
  }

  return SocDpStatusToEfi (SocDpHwRecoverLink (&Dp));
}

STATIC EFI_STATUS
Check (
  VOID  *Priv
  )
{
  UINT64      Hertz;
  EFI_STATUS  Status = K3DisplayGetPixelClock (&Clock, &Hertz);

  if (!EFI_ERROR (Status) && (!Dpu.Active || (Hertz != Clock.Pixel.ActualHz))) {
    Status = EFI_NOT_READY;
  }

  /* Check runs inside GOP Blt/SetMode. Console output here would call
     GraphicsConsole and re-enter the same GOP while it is busy. */
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Dpu.Faulted || EFI_ERROR (Dpu.CleanupError) ||
      !Dp.Ready || Dp.CleanupError)
  {
    return EFI_DEVICE_ERROR;
  }

  if (Dp.Trained && !SocDpHwCheckLink (&Dp, NULL)) {
    RecoveryAttempted = FALSE;
    return EFI_SUCCESS;
  }

  if (SocDpHwDetectHpd (&Dp) != ConnectorStatusConnected) {
    return EFI_NO_MEDIA;
  }

  /* A text refresh may contain hundreds of Blt calls. Bound recovery to
     one attempt per second while the same receiver remains unavailable. */
  UINT64  Now = GetPerformanceCounter ();

  if (RecoveryAttempted &&
      (GetTimeInNanoSecond (Now - LastRecovery) < 1000000000ULL))
  {
    return EFI_NO_MEDIA;
  }

  RecoveryAttempted = TRUE;
  Status            = RecoverOutput ();
  LastRecovery      = GetPerformanceCounter ();
  if (EFI_ERROR (Status)) {
    if ((Status == EFI_COMPROMISED_DATA) || Dp.CleanupError) {
      return EFI_DEVICE_ERROR;
    }

    return EFI_NO_MEDIA;
  }

  RecoveryAttempted = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
K3DpuGopEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  VOID              *Fdt = NULL;
  K3_DPU_RESOURCES  Resources;
  EFI_STATUS        Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);

  if (EFI_ERROR (Status)) {
    CONST EFI_GUID     Guid = RISCV_SEC_HANDOFF_HOB_GUID;
    EFI_HOB_GUID_TYPE  *Hob = GetFirstGuidHob (&Guid);
    if (Hob && (GET_GUID_HOB_DATA_SIZE (Hob) >=
                sizeof (RISCV_SEC_HANDOFF_DATA)))
    {
      RISCV_SEC_HANDOFF_DATA  Handoff;
      CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
      Fdt = Handoff.FdtPointer;
    }
  }

  if (!Fdt || FdtCheckHeader (Fdt)) {
    return EFI_NOT_FOUND;
  }

  EFI_HANDLE                Existing = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Path    = &DisplayPath.Vendor.Header;

  if (!EFI_ERROR (
         gBS->LocateDevicePath (
                &gEfiGraphicsOutputProtocolGuid,
                &Path,
                &Existing
                )
         ))
  {
    return EFI_ALREADY_STARTED;
  }

  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;

  Status = gBS->HandleProtocol (
                  Image,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Loaded
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Loaded->Unload = Unload;
  INT32  Node = -1;

  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "spacemit,k3-saturn-dpu")) >= 0) {
    if (K3FdtEnabled (Fdt, Node) && !EFI_ERROR (K3DpuParse (Fdt, Node, &Resources))) {
      break;
    }
  }

  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  Status = K3DisplayClockParse (&Resources, &Clock);
  K3_DISPLAY_CLOCK_PLAN  EnginePlan;

  if (!EFI_ERROR (Status)) {
    Status = K3DisplayChooseClock (&Clock, 148500000, &EnginePlan);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3DisplayPrepare (&Clock, &EnginePlan);
  }

  if (EFI_ERROR (Status)) {
    if (Clock.Owned) {
      DEBUG ((DEBUG_ERROR, "K3 GOP prepare failed; clock/power owner retained.\n"));
      return EFI_SUCCESS;
    }

    return Status;
  }

  Status = K3MapMmio (Resources.Dpu, 0x54000);
  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Resources.Dp, 0x4000);
  }

  if (!EFI_ERROR (Status)) {
    Status = SocDpStatusToEfi (
               SocDpInit (
                 &Dp,
                 Resources.Dp,
                 SOC_DP_REF_CLK_24M,
                 SOC_VIDEO_RGB_8BIT
                 )
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = SocDpStatusToEfi (SocDpHwReadSinkCaps (&Dp));
  }

  UINT8   Edid[512];
  UINT32  Blocks = 0;

  if (!EFI_ERROR (Status)) {
    Status = SocDpStatusToEfi (SocDpConnGetEdidBlock (&Dp, Edid, 0, 128));
    Blocks = EFI_ERROR (Status) ? 0 : (UINT32)Edid[126] + 1;
    if (Blocks > 4) {
      Status = EFI_BUFFER_TOO_SMALL;
    }
  }

  for (UINT32 I = 1; !EFI_ERROR (Status) && I < Blocks; I++) {
    Status = SocDpStatusToEfi (SocDpConnGetEdidBlock (&Dp, Edid + I * 128, I, 128));
  }

  K3_DISPLAY_MODE_SET  Modes;

  if (!EFI_ERROR (Status)) {
    Status = K3DisplayModes (
               Edid,
               Blocks * 128,
               Dp.Link.MaxRate,
               Dp.Link.MaxNumLanes,
               SelectPixel,
               NULL,
               &Modes
               );
  }

  /* The initial implementation exposes one EDID mode within the tested
     engine bandwidth. Repeated SetMode on it clears the existing surface
     without replacing an active DMA buffer. */
  if (!EFI_ERROR (Status)) {
    UINT32  Selected = MAX_UINT32;
    for (UINT32 I = 0; I < Modes.Count; I++) {
      SOC_DP_VIDEO_MODE  *Timing = &Modes.Modes[I].Timing;
      if ((Timing->Hdisplay > 1920) || (Timing->Vdisplay > 1080) ||
          ((UINT64)Timing->Clock * 1000 > EnginePlan.ActualHz))
      {
        continue;
      }

      if ((Selected == MAX_UINT32) || Modes.Modes[I].Preferred) {
        Selected = I;
      }

      if (Modes.Modes[I].Preferred) {
        break;
      }
    }

    if (Selected == MAX_UINT32) {
      Status = EFI_UNSUPPORTED;
    } else {
      Modes.Modes[0] = Modes.Modes[Selected];
      Modes.Count    = 1;
    }
  }

  K3_DPU_OWNER_OPS  Owner = { K3DisplayGetPixelClock, ColdResetState };

  if (!EFI_ERROR (Status)) {
    Status = K3DpuBind (&Dpu, &Resources, &Owner, &Clock);
  }

  K3_GOP_OPS  Ops = { Start, Stop, Check };

  if (!EFI_ERROR (Status)) {
    Status = K3GopCreate (&Modes, Edid, Blocks * 128, &Ops, NULL, &Gop);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3GopProtocol (Gop)->SetMode (K3GopProtocol (Gop), 0);
  }

  if (!EFI_ERROR (Status)) {
    // Keep link recovery silent once consoles can draw through this GOP.
    Dp.Quiet     = TRUE;
    Dp.Phy.Quiet = TRUE;
    EFI_EDID_DISCOVERED_PROTOCOL  *Discovered;
    EFI_EDID_ACTIVE_PROTOCOL      *Active;
    K3GopEdid (Gop, &Discovered, &Active);
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &DisplayHandle,
                    &gEfiDevicePathProtocolGuid,
                    &DisplayPath,
                    &gEfiGraphicsOutputProtocolGuid,
                    K3GopProtocol (Gop),
                    &gEfiEdidDiscoveredProtocolGuid,
                    Discovered,
                    &gEfiEdidActiveProtocolGuid,
                    Active,
                    NULL
                    );
  }

  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "K3 native GOP publish: %r\n", Status));
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  if (Clock.DmaStarted) {
    DEBUG ((DEBUG_ERROR, "K3 GOP startup failed; DMA resources retained until reset.\n"));
    /* Returning an error could unload callback code still owned by DMA. */
    return EFI_SUCCESS;
  }

  if (Gop) {
    EFI_STATUS  Cleanup = K3GopDestroy (Gop);
    if (EFI_ERROR (Cleanup)) {
      DEBUG ((DEBUG_ERROR, "K3 GOP cleanup: %r; resources retained.\n", Cleanup));
      return EFI_SUCCESS;
    }

    Gop = NULL;
  }

  if (Dp.Regs) {
    SocDpExit (&Dp);
  }

  EFI_STATUS  Cleanup = K3DisplayColdAbort (&Clock);

  if (Clock.Owned) {
    DEBUG ((DEBUG_ERROR, "K3 GOP clock cleanup: %r; owner retained.\n", Cleanup));
    return EFI_SUCCESS;
  }

  return Status;
}

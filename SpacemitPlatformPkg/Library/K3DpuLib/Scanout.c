/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* Register recipe adapted from BSD SpacemiT DpuDxe, copyright (c) 2025
   SpacemiT Co., Ltd. K3 HEE fields/formats checked against pinned Linux.
   Burst/line-buffer preset words remain provisional vendor behavior. */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/K3DpuLib.h>
#include <Library/TimerLib.h>

#define SCENE           0x340
#define RDMA            0x1000
#define COMPOSER        0x30000
#define TMG             0x51200
#define IRQ_STATUS      0x730
#define IRQ_MASK        0x70c
#define CFG_DONE        BIT5
#define FRAME_EOF       BIT1
#define UNDERRUN        BIT8
#define STOP_BUDGET_US  200000
STATIC VOID
Write (
  K3_DPU  *D,
  UINTN   Offset,
  UINT32  Value
  )
{
  MmioWrite32 (D->Resources.Dpu + Offset, Value);
}

STATIC UINT32
Read (
  K3_DPU  *D,
  UINTN   Offset
  )
{
  return MmioRead32 (D->Resources.Dpu + Offset);
}

EFI_STATUS
K3DpuBind (
  K3_DPU                  *D,
  CONST K3_DPU_RESOURCES  *R,
  CONST K3_DPU_OWNER_OPS  *Ops,
  VOID                    *Priv
  )
{
  if (!D || !R || (R->Dpu != 0xc0440000) || (R->Dp != 0xcac88000)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Ops || !Ops->GetPixelClock || !Ops->ResetAndDrain) {
    return EFI_UNSUPPORTED;
  }

  if (D->Bound || D->Surface || D->Active) {
    return EFI_ALREADY_STARTED;
  }

  ZeroMem (D, sizeof (*D));
  D->Resources = *R;
  D->Ops       = *Ops;
  D->Priv      = Priv;
  D->Bound     = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
K3DpuStop (
  K3_DPU  *D
  )
{
  if (!D || !D->Bound) {
    return EFI_INVALID_PARAMETER;
  }

  /* The owner must keep reset/drain register access available even when
     a prior operation faulted. Do not substitute a timing EOF for DMA idle. */
  EFI_STATUS  Status = D->Ops.ResetAndDrain (D->Priv, STOP_BUDGET_US);

  if (!EFI_ERROR (Status) && ((Read (D, SCENE) & (0x3ff | BIT18 | (7U << 23))) ||
                              (Read (D, TMG) & BIT2) || (Read (D, COMPOSER) & BIT0)))
  {
    Status = EFI_DEVICE_ERROR;
  }

  D->CleanupError = Status;
  if (EFI_ERROR (Status)) {
    D->Faulted = TRUE;
    return Status;
  }

  if (D->Surface) {
    if (!D->Surface->Users) {
      D->Faulted      = TRUE;
      D->CleanupError = EFI_COMPROMISED_DATA;
      return D->CleanupError;
    }

    D->Surface->Users--;
    D->Surface = NULL;
  }

  D->Active = D->Faulted = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
K3DpuStart (
  K3_DPU                   *D,
  K3_DISPLAY_SURFACE       *S,
  CONST SOC_DP_VIDEO_MODE  *M,
  UINT64                   ExpectedDpuClockHz
  )
{
  if (!D || !D->Bound || !S || !ExpectedDpuClockHz || !SocDpModeValid (M)) {
    return EFI_INVALID_PARAMETER;
  }

  if (D->Active || D->Surface) {
    return EFI_ALREADY_STARTED;
  }

  if (D->Faulted || S->CleanupError || !S->Buffer || !S->Mapping || !S->Blt) {
    return EFI_NOT_READY;
  }

  if (S->Users) {
    return EFI_ACCESS_DENIED;
  }

  UINTN  Stride = (UINTN)S->Info.PixelsPerScanLine * 4;

  if ((S->Info.PixelFormat != PixelBlueGreenRedReserved8BitPerColor) ||
      (S->Info.HorizontalResolution != M->Hdisplay) || (S->Info.VerticalResolution != M->Vdisplay) ||
      (M->Hdisplay > 3840) || (M->Vdisplay > 2160) || (Stride > 65535) || (Stride < (UINTN)M->Hdisplay * 4) ||
      (S->Bytes != Stride * M->Vdisplay) || (S->MappedBytes < S->Bytes) ||
      (S->Device < 0x100000000ULL) || (S->Device > K3_DPU_LINEAR_MAX) ||
      !S->MappedBytes || (S->MappedBytes - 1 > K3_DPU_LINEAR_MAX - S->Device) || (S->Device & EFI_PAGE_MASK))
  {
    return EFI_UNSUPPORTED;
  }

  UINT32  Hs = M->HsyncEnd - M->HsyncStart, Hb = M->Htotal - M->HsyncEnd, Hf = M->HsyncStart - M->Hdisplay;
  UINT32  Vs = M->VsyncEnd - M->VsyncStart, Vb = M->Vtotal - M->VsyncEnd, Vf = M->VsyncStart - M->Vdisplay;

  if ((Hs > 1023) || (Hb > 4095) || (Hf > 4095) || (Vs > 1023) || (Vb > 4095)) {
    return EFI_UNSUPPORTED;
  }

  UINT64      Rate;
  EFI_STATUS  Status = D->Ops.GetPixelClock (D->Priv, &Rate);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Rate != ExpectedDpuClockHz) {
    return EFI_NOT_READY;
  }

  Status = K3DpuStop (D); /* Establish reset/empty state before taking the framebuffer. */
  if (EFI_ERROR (Status)) {
    return Status;
  }

  D->Surface = S;
  S->Users++;
  /* Ack stale events before opening K3's active-high reporting mask. */
  Write (D, IRQ_STATUS, CFG_DONE | FRAME_EOF | UNDERRUN);
  Write (D, IRQ_MASK, Read (D, IRQ_MASK) | CFG_DONE | FRAME_EOF | UNDERRUN);
  Write (D, COMPOSER, 1);
  Write (D, TMG + 0xc, Hf << 16);
  Write (D, TMG + 0x10, Hb << 16 | Hs);
  Write (D, TMG + 0x14, Vs << 16 | Vf);
  Write (D, TMG + 0x18, (UINT32)M->Hdisplay << 16 | Vb);
  Write (D, TMG + 0x1c, M->Vdisplay);
  Write (
    D,
    TMG,
    ((M->Flags & SOC_DP_MODE_FLAG_PHSYNC) ? BIT7 : 0) |
    ((M->Flags & SOC_DP_MODE_FLAG_PVSYNC) ? BIT8 : 0) | BIT2
    );
  Write (D, TMG + 8, 2U << 12);
  Write (D, TMG + 0x3c, 1);
  Write (D, 0x1284, 0x840);
  Write (D, RDMA, 0x0f00217c); /* Retained K3 linear RGB burst-control preset. */
  Write (D, RDMA + 0x24, (UINT32)S->Device);
  Write (D, RDMA + 0x28, (UINT32)(S->Device >> 32) & 3);
  Write (D, RDMA + 0x3c, Stride);
  Write (D, RDMA + 0x40, M->Hdisplay | (UINT32)M->Vdisplay << 16);
  Write (D, RDMA + 0x44, 0);
  Write (D, RDMA + 0x48, (M->Hdisplay - 1) | (UINT32)(M->Vdisplay - 1) << 16);
  Write (D, RDMA + 0x74, 8);
  Write (D, RDMA + 0x7c, 0x100001e0);                            /* Retained line-buffer preset. */
  Write (D, COMPOSER + 4, M->Hdisplay | (UINT32)M->Vdisplay << 16);
  Write (D, COMPOSER + 0x20, (UINT32)(M->Hdisplay - 1) << 16);
  Write (D, COMPOSER + 0x24, (UINT32)(M->Vdisplay - 1) << 16);
  Write (D, COMPOSER + 0x38, 0xff01); /* Constant opaque alpha; ignore GOP reserved byte. */
  Write (D, 0x30300, 0);
  Write (D, 0x30334, M->Hdisplay | (UINT32)M->Vdisplay << 16);
  Write (D, SCENE, BIT24 | BIT18 | BIT0);
  Write (D, SCENE + 0xc, 0x821);
  MemoryFence ();
  Write (D, SCENE + 8, 1);
  Write (D, SCENE + 0x10, 1);
  BOOLEAN  Config = FALSE, Frame = FALSE;

  Status = EFI_TIMEOUT;
  for (UINTN Us = 0; Us < STOP_BUDGET_US; Us += 100) {
    UINT32  Events = Read (D, IRQ_STATUS);
    if (Events & UNDERRUN) {
      Status = EFI_DEVICE_ERROR;
      break;
    }

    if (Events & CFG_DONE) {
      Config = TRUE;
    }

    if (Config && (Events & FRAME_EOF)) {
      Frame = TRUE;
    }

    Write (D, IRQ_STATUS, Events & (CFG_DONE | FRAME_EOF | UNDERRUN));
    if (Config && Frame) {
      Status = EFI_SUCCESS;
      break;
    }

    MicroSecondDelay (100);
  }

  /* Firmware polls these events and installs no DPU interrupt handler.
     Keep scanout running, but do not hand an asserted interrupt to the OS
     before its CRTC has acquired clocks and initialized IRQ handling. */
  Write (D, IRQ_MASK, Read (D, IRQ_MASK) & ~(CFG_DONE | FRAME_EOF | UNDERRUN));
  Write (D, IRQ_STATUS, CFG_DONE | FRAME_EOF | UNDERRUN);

  if (!EFI_ERROR (Status)) {
    D->Mode   = *M;
    D->Active = TRUE;
    return Status;
  }

  K3DpuStop (D); /* Preserve primary start failure; cleanup can retain the pin. */
  return Status;
}

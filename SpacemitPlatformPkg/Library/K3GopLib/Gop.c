/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3GopLib.h>
#include <Library/MemoryAllocationLib.h>
#define GOP_SIGNATURE  SIGNATURE_32 ('K','3','G','P')
struct K3_GOP {
  UINT32                                  Signature;
  K3_DISPLAY_MODE_SET                     Modes;
  K3_GOP_OPS                              Ops;
  VOID                                    *Priv;
  EFI_GRAPHICS_OUTPUT_PROTOCOL            Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    Info;
  EFI_EDID_DISCOVERED_PROTOCOL            Discovered;
  EFI_EDID_ACTIVE_PROTOCOL                Active;
  K3_DISPLAY_SURFACE                      *Surface, *Pending;
  BOOLEAN                                 Busy, Faulted, HardwareTouched;
};

STATIC K3_GOP *
Instance (
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *P
  )
{
  if (!P) {
    return NULL;
  }

  K3_GOP  *G = BASE_CR (P, K3_GOP, Gop);

  return G->Signature == GOP_SIGNATURE ? G : NULL;
}

STATIC VOID
NoMode (
  K3_GOP  *G
  )
{
  G->Mode.Mode            = MAX_UINT32;
  G->Mode.FrameBufferBase = 0;
  G->Mode.FrameBufferSize = 0;
  ZeroMem (&G->Info, sizeof (G->Info));
  G->Active.SizeOfEdid = 0;
  G->Active.Edid       = NULL;
}

STATIC EFI_STATUS
Release (
  K3_DISPLAY_SURFACE  **S
  )
{
  if (!*S) {
    return EFI_SUCCESS;
  }

  EFI_STATUS  Status = K3SurfaceFree (*S);

  if (!EFI_ERROR (Status)) {
    FreePool (*S);
    *S = NULL;
  }

  return Status;
}

STATIC EFI_STATUS
Halt (
  K3_GOP  *G
  )
{
  EFI_STATUS  Status = G->HardwareTouched ? G->Ops.Stop (G->Priv) : EFI_SUCCESS;

  if (!EFI_ERROR (Status) && ((G->Surface && G->Surface->Users) || (G->Pending && G->Pending->Users))) {
    Status = EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (Status)) {
    G->Faulted = TRUE;
    return Status;
  }

  G->HardwareTouched = FALSE;
  NoMode (G);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Query (
  EFI_GRAPHICS_OUTPUT_PROTOCOL          *P,
  UINT32                                N,
  UINTN                                 *Bytes,
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  K3_GOP  *G = Instance (P);

  if (!G || !Bytes || !Info || (N >= G->Modes.Count)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info  = NULL;
  *Bytes = 0;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *I = AllocateZeroPool (sizeof (*I));

  if (!I) {
    return EFI_OUT_OF_RESOURCES;
  }

  I->HorizontalResolution = G->Modes.Modes[N].Timing.Hdisplay;
  I->VerticalResolution   = G->Modes.Modes[N].Timing.Vdisplay;
  I->PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  I->PixelsPerScanLine    = ALIGN_VALUE ((UINTN)I->HorizontalResolution * 4, 64) / 4;
  *Info                   = I;
  *Bytes                  = sizeof (*I);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Set (
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *P,
  UINT32                        N
  )
{
  K3_GOP  *G = Instance (P);

  if (!G) {
    return EFI_INVALID_PARAMETER;
  }

  if (N >= G->Modes.Count) {
    return EFI_UNSUPPORTED;
  }

  if (G->Busy || G->Faulted || G->Pending) {
    return EFI_NOT_READY;
  }

  G->Busy = TRUE;
  EFI_STATUS  Status;

  if (G->Surface && (G->Mode.Mode == N)) {
    Status = G->Ops.Check (G->Priv);
    if (EFI_ERROR (Status) && (Status != EFI_NO_MEDIA)) {
      G->Faulted = TRUE;
    }

    if (!EFI_ERROR (Status)) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black = { 0 };
      Status = K3SurfaceBlt (
                 G->Surface,
                 &Black,
                 EfiBltVideoFill,
                 0,
                 0,
                 0,
                 0,
                 G->Info.HorizontalResolution,
                 G->Info.VerticalResolution,
                 0
                 );
      if (EFI_ERROR (Status)) {
        G->Faulted = TRUE;
      }
    }

    G->Busy = FALSE;
    return Status;
  }

  G->Pending = AllocateZeroPool (sizeof (*G->Pending));
  if (!G->Pending) {
    G->Busy = FALSE;
    return EFI_OUT_OF_RESOURCES;
  }

  CONST K3_DISPLAY_MODE  *M = &G->Modes.Modes[N];

  Status = K3SurfaceCreate (G->Pending, M->Timing.Hdisplay, M->Timing.Vdisplay);
  if (EFI_ERROR (Status)) {
    if (EFI_ERROR (Release (&G->Pending))) {
      G->Faulted = TRUE;
    }

    G->Busy = FALSE;
    return Status;
  }

  if (G->Surface) {
    Status = Halt (G);
    if (EFI_ERROR (Status)) {
      Release (&G->Pending);
      G->Busy = FALSE;
      return Status;
    }

    Status = Release (&G->Surface);
    if (EFI_ERROR (Status)) {
      G->Faulted = TRUE;
      Release (&G->Pending);
      G->Busy = FALSE;
      return Status;
    }
  }

  G->HardwareTouched = TRUE;
  Status             = G->Ops.Start (G->Priv, G->Pending, M);
  if (!EFI_ERROR (Status) && !G->Pending->Users) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Status = G->Ops.Check (G->Priv);
  }

  if (EFI_ERROR (Status)) {
    EFI_STATUS  Stop = Halt (G);
    if (!EFI_ERROR (Stop) && EFI_ERROR (Release (&G->Pending))) {
      G->Faulted = TRUE;
    }

    G->Busy = FALSE;
    return Status;
  }

  G->Surface              = G->Pending;
  G->Pending              = NULL;
  G->Info                 = G->Surface->Info;
  G->Mode.Mode            = N;
  G->Mode.FrameBufferBase = G->Surface->Device;
  G->Mode.FrameBufferSize = G->Surface->Bytes;
  G->Active               = (EFI_EDID_ACTIVE_PROTOCOL) {
    G->Discovered.SizeOfEdid, G->Discovered.Edid
  };
  G->Busy = FALSE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Blt (
  EFI_GRAPHICS_OUTPUT_PROTOCOL       *P,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *B,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  O,
  UINTN                              Sx,
  UINTN                              Sy,
  UINTN                              Dx,
  UINTN                              Dy,
  UINTN                              W,
  UINTN                              H,
  UINTN                              Delta
  )
{
  K3_GOP  *G = Instance (P);

  if (!G) {
    return EFI_INVALID_PARAMETER;
  }

  if (G->Busy || G->Faulted || !G->Surface || (G->Mode.Mode == MAX_UINT32)) {
    return EFI_NOT_READY;
  }

  G->Busy = TRUE;
  EFI_STATUS  Status = G->Ops.Check (G->Priv);

  if (EFI_ERROR (Status)) {
    if (Status != EFI_NO_MEDIA) {
      G->Faulted = TRUE;
    }
  } else {
    Status = K3SurfaceBlt (G->Surface, B, O, Sx, Sy, Dx, Dy, W, H, Delta);
  }

  G->Busy = FALSE;
  return Status;
}

EFI_STATUS
K3GopCreate (
  CONST K3_DISPLAY_MODE_SET  *Modes,
  CONST UINT8                *E,
  UINTN                      Bytes,
  CONST K3_GOP_OPS           *Ops,
  VOID                       *Priv,
  K3_GOP                     **Out
  )
{
  if (!Out || !Modes || !Modes->Count || (Modes->Count > K3_DISPLAY_MAX_MODES) || !E || (Bytes < 128) ||
      (Bytes > 32768) || Bytes % 128 || (Bytes != (UINTN)(E[126] + 1) * 128) ||
      !Ops || !Ops->Start || !Ops->Stop || !Ops->Check)
  {
    return EFI_INVALID_PARAMETER;
  }

  *Out = NULL;
  static CONST UINT8  Header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };

  if (CompareMem (E, Header, sizeof (Header))) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN Block = 0; Block < Bytes; Block += 128) {
    UINT8  Sum = 0;
    for (UINTN I = 0; I < 128; I++) {
      Sum += E[Block + I];
    }

    if (Sum) {
      return EFI_COMPROMISED_DATA;
    }
  }

  for (UINT32 I = 0; I < Modes->Count; I++) {
    CONST K3_DISPLAY_MODE  *M = &Modes->Modes[I];
    if (!SocDpModeValid (&M->Timing) || (M->Timing.Hdisplay > 3840) || (M->Timing.Vdisplay > 2160) ||
        !M->PixelClock.ActualHz || (M->PixelClock.ActualHz > 750000000) ||
        ((M->PixelClock.ActualHz + 500) / 1000 != M->Timing.Clock))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  K3_GOP  *G = AllocateZeroPool (sizeof (*G));

  if (!G) {
    return EFI_OUT_OF_RESOURCES;
  }

  G->Discovered.Edid = AllocateZeroPool (Bytes);
  if (!G->Discovered.Edid) {
    FreePool (G);
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (G->Discovered.Edid, E, Bytes);
  G->Discovered.SizeOfEdid = Bytes;
  G->Signature             = GOP_SIGNATURE;
  G->Modes                 = *Modes;
  G->Ops                   = *Ops;
  G->Priv                  = Priv;
  G->Gop                   = (EFI_GRAPHICS_OUTPUT_PROTOCOL) {
    Query, Set, Blt, &G->Mode
  };
  G->Mode.MaxMode    = Modes->Count;
  G->Mode.Info       = &G->Info;
  G->Mode.SizeOfInfo = sizeof (G->Info);
  NoMode (G);
  *Out = G;
  return EFI_SUCCESS;
}

EFI_GRAPHICS_OUTPUT_PROTOCOL *
K3GopProtocol (
  K3_GOP  *G
  )
{
  return G && G->Signature == GOP_SIGNATURE ? &G->Gop : NULL;
}

VOID
K3GopEdid (
  K3_GOP                        *G,
  EFI_EDID_DISCOVERED_PROTOCOL  **D,
  EFI_EDID_ACTIVE_PROTOCOL      **A
  )
{
  if (G && (G->Signature == GOP_SIGNATURE)) {
    if (D) {
      *D = &G->Discovered;
    }

    if (A) {
      *A = &G->Active;
    }
  }
}

EFI_STATUS
K3GopDestroy (
  K3_GOP  *G
  )
{
  if (!G || (G->Signature != GOP_SIGNATURE)) {
    return EFI_INVALID_PARAMETER;
  }

  if (G->Busy) {
    return EFI_NOT_READY;
  }

  G->Busy = TRUE;
  EFI_STATUS  Status = EFI_SUCCESS;

  if (G->Surface || G->Pending) {
    Status = Halt (G);
  }

  if (!EFI_ERROR (Status)) {
    Status = Release (&G->Pending);
  }

  if (!EFI_ERROR (Status)) {
    Status = Release (&G->Surface);
  }

  if (EFI_ERROR (Status)) {
    G->Busy    = FALSE;
    G->Faulted = TRUE;
    return Status;
  }

  FreePool (G->Discovered.Edid);
  G->Signature = 0;
  FreePool (G);
  return EFI_SUCCESS;
}

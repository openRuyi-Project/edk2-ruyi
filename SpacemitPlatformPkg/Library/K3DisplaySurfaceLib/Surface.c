/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DmaLib.h>
#include <Library/K3DmaBufferLib.h>
#include <Library/K3DisplaySurfaceLib.h>
#include <Library/MemoryAllocationLib.h>

EFI_STATUS
K3SurfaceFree (
  K3_DISPLAY_SURFACE  *S
  )
{
  if (!S) {
    return EFI_INVALID_PARAMETER;
  }

  if (S->Users) {
    return EFI_ACCESS_DENIED;
  }

  EFI_STATUS  Status;

  if (S->Mapping) {
    Status = DmaUnmap (S->Mapping);
    if (EFI_ERROR (Status)) {
      S->CleanupError = Status;
      return Status;
    }

    S->Mapping = NULL;
  }

  if (S->Buffer) {
    Status = DmaFreeBuffer (S->Pages, S->Buffer);
    if (EFI_ERROR (Status)) {
      S->CleanupError = Status;
      return Status;
    }

    S->Buffer = NULL;
  }

  if (S->Blt) {
    FreePool (S->Blt);
  }

  ZeroMem (S, sizeof (*S));
  return EFI_SUCCESS;
}

EFI_STATUS
K3SurfaceCreate (
  K3_DISPLAY_SURFACE  *S,
  UINT32              Width,
  UINT32              Height
  )
{
  if (!S || !Width || !Height) {
    return EFI_INVALID_PARAMETER;
  }

  if (S->Buffer || S->Mapping || S->Blt || S->Users) {
    return EFI_ALREADY_STARTED;
  }

  if ((Width > 3840) || (Height > 2160)) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (S, sizeof (*S));
  UINTN  Stride = ALIGN_VALUE ((UINTN)Width * 4, 64);

  S->Bytes                     = Stride * Height;
  S->Pages                     = EFI_SIZE_TO_PAGES (S->Bytes);
  S->Info.HorizontalResolution = Width;
  S->Info.VerticalResolution   = Height;
  S->Info.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  S->Info.PixelsPerScanLine    = Stride / 4;
  EFI_STATUS  Status = K3DmaAllocateBufferBelow (EfiRuntimeServicesData, S->Pages, K3_DPU_LINEAR_MAX, &S->Buffer);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  S->MappedBytes = EFI_PAGES_TO_SIZE (S->Pages);
  UINTN  Expected = S->MappedBytes;

  Status = DmaMap (MapOperationBusMasterCommonBuffer, S->Buffer, &S->MappedBytes, &S->Device, &S->Mapping);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  if (!S->Mapping || (S->MappedBytes != Expected) || (S->Device & EFI_PAGE_MASK) ||
      (S->Device < 0x100000000ULL) || (S->Device > K3_DPU_LINEAR_MAX) ||
      (S->MappedBytes - 1 > K3_DPU_LINEAR_MAX - S->Device))
  {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }

  UINTN  Size = 0;

  Status = FrameBufferBltConfigure (S->Buffer, &S->Info, NULL, &Size);
  if ((Status != EFI_BUFFER_TOO_SMALL) || !Size || (Size > 65536)) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  S->Blt = AllocateZeroPool (Size);
  if (!S->Blt) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  Status = FrameBufferBltConfigure (S->Buffer, &S->Info, S->Blt, &Size);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  ZeroMem (S->Buffer, Expected);
  MemoryFence ();
  return EFI_SUCCESS;
Fail:
  K3SurfaceFree (S); /* Preserve original failure and any retained cleanup. */
  return Status;
}

STATIC BOOLEAN
Rectangle (
  K3_DISPLAY_SURFACE  *S,
  UINTN               X,
  UINTN               Y,
  UINTN               W,
  UINTN               H
  )
{
  return X < S->Info.HorizontalResolution && Y < S->Info.VerticalResolution &&
         W <= S->Info.HorizontalResolution - X && H <= S->Info.VerticalResolution - Y;
}

EFI_STATUS
K3SurfaceBlt (
  K3_DISPLAY_SURFACE                 *S,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *Buffer,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  Op,
  UINTN                              Sx,
  UINTN                              Sy,
  UINTN                              Dx,
  UINTN                              Dy,
  UINTN                              W,
  UINTN                              H,
  UINTN                              Delta
  )
{
  if (!S || !S->Buffer || !S->Mapping || !S->Blt || S->CleanupError) {
    return EFI_NOT_READY;
  }

  if (!W || !H || ((UINT32)Op >= EfiGraphicsOutputBltOperationMax)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Op != EfiBltVideoToBltBuffer) && !Rectangle (S, Dx, Dy, W, H)) {
    return EFI_INVALID_PARAMETER;
  }

  if (((Op == EfiBltVideoToBltBuffer) || (Op == EfiBltVideoToVideo)) && !Rectangle (S, Sx, Sy, W, H)) {
    return EFI_INVALID_PARAMETER;
  }

  union {
    UINT32                           Alignment;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL    Pixel;
  } Fill;
  if (Op == EfiBltVideoFill) {
    if (!Buffer || ((UINTN)Buffer > MAX_UINTN - sizeof (Fill.Pixel))) {
      return EFI_INVALID_PARAMETER;
    }

    CopyMem (&Fill.Pixel, Buffer, sizeof (Fill.Pixel));
    Buffer = &Fill.Pixel;
  } else if ((Op == EfiBltBufferToVideo) || (Op == EfiBltVideoToBltBuffer)) {
    UINTN  X = Op == EfiBltBufferToVideo ? Sx : Dx, Y = Op == EfiBltBufferToVideo ? Sy : Dy;
    if (!Buffer || (W > MAX_UINTN / 4) || (X > MAX_UINTN / 4 - W) || (Y > MAX_UINTN - H)) {
      return EFI_INVALID_PARAMETER;
    }

    UINTN  Row = (X + W) * 4;
    if (!Delta) {
      Delta = W * 4;
    }

    if ((Delta < Row) || (Y + H - 1 > (MAX_UINTN - Row) / Delta)) {
      return EFI_INVALID_PARAMETER;
    }

    UINTN  End = (Y + H - 1) * Delta + Row;
    if ((UINTN)Buffer > MAX_UINTN - End) {
      return EFI_INVALID_PARAMETER;
    }
  }

  EFI_STATUS  Status = FrameBufferBlt (S->Blt, Buffer, Op, Sx, Sy, Dx, Dy, W, H, Delta);

  if (!EFI_ERROR (Status)) {
    MemoryFence ();
  }

  return Status;
}

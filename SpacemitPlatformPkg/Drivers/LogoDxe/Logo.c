// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BmpSupportLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/PlatformLogo.h>

STATIC EFI_STATUS EFIAPI
GetImage (
  EDKII_PLATFORM_LOGO_PROTOCOL           *This,
  UINT32                                 *Instance,
  EFI_IMAGE_INPUT                        *Image,
  EDKII_PLATFORM_LOGO_DISPLAY_ATTRIBUTE  *Attribute,
  INTN                                   *OffsetX,
  INTN                                   *OffsetY
  )
{
  VOID                           *Bmp;
  UINTN                          Size, BltSize = 0, Width, Height;
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Blt = NULL;

  if ((Instance == NULL) || (Image == NULL) || (Attribute == NULL) ||
      (OffsetX == NULL) || (OffsetY == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (*Instance != 0) {
    return EFI_NOT_FOUND;
  }

  Status = GetSectionFromAnyFv (
             &gOpenRuyiLogoGuid,
             EFI_SECTION_RAW,
             0,
             &Bmp,
             &Size
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = TranslateBmpToGopBlt (Bmp, Size, &Blt, &BltSize, &Height, &Width);
  FreePool (Bmp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Width > MAX_UINT16) || (Height > MAX_UINT16)) {
    FreePool (Blt);
    return EFI_UNSUPPORTED;
  }

  Image->Flags  = 0;
  Image->Width  = (UINT16)Width;
  Image->Height = (UINT16)Height;
  Image->Bitmap = Blt;
  *Attribute    = EdkiiPlatformLogoDisplayAttributeCenter;
  *OffsetX      = 0;
  *OffsetY      = 0;
  (*Instance)++;
  return EFI_SUCCESS;
}

STATIC EDKII_PLATFORM_LOGO_PROTOCOL  mLogo = { GetImage };

EFI_STATUS EFIAPI
LogoInitialize (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle = NULL;

  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEdkiiPlatformLogoProtocolGuid,
                &mLogo,
                NULL
                );
}

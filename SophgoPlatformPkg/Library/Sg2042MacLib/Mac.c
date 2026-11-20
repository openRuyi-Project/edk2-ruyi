/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/Sg2042MacLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Rng.h>

BOOLEAN
Sg2042ValidMac (CONST UINT8 Mac[6])
{
  return Mac != NULL && !(Mac[0] & 1) && !IsZeroBuffer (Mac, 6);
}

EFI_STATUS
Sg2042FallbackMac (CONST VOID *Port, UINTN PortSize, BOOLEAN Create, UINT8 Mac[6])
{
  struct {
    UINT8  Uid[16];
    UINT8  PortHash[SHA256_DIGEST_SIZE];
  } Key;
  UINT8   Hash[SHA256_DIGEST_SIZE];
  CHAR16  Name[40];
  UINT32  Attributes;
  UINTN   Size = 6;
  EFI_STATUS Status;
  BOOLEAN Persistent = !PcdGetBool (PcdEmuVariableNvModeEnable);

  if (!Port || !PortSize || !Mac) { return EFI_INVALID_PARAMETER; }
  if (!Sha256HashAll (Port, PortSize, Key.PortHash)) { return EFI_DEVICE_ERROR; }
  UnicodeSPrint (Name, sizeof (Name), L"Port-%02x%02x%02x%02x%02x%02x%02x%02x",
                 Key.PortHash[0], Key.PortHash[1], Key.PortHash[2], Key.PortHash[3],
                 Key.PortHash[4], Key.PortHash[5], Key.PortHash[6], Key.PortHash[7]);
  if (Persistent) {
    Status = gRT->GetVariable (Name, &gSg2042MacVariableGuid, &Attributes, &Size, Mac);
    if (!EFI_ERROR (Status) && Size == 6 && (Attributes & EFI_VARIABLE_NON_VOLATILE) &&
        Sg2042ValidMac (Mac)) { return EFI_SUCCESS; }
    if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND && Status != EFI_BUFFER_TOO_SMALL) {
      return Status;
    }
  }

  BOOLEAN HasUid = PcdGetSize (PcdSg2042FlashUid) == sizeof (Key.Uid) &&
                   !IsZeroBuffer (PcdGetPtr (PcdSg2042FlashUid), sizeof (Key.Uid));
  if (HasUid) {
    CopyMem (Key.Uid, PcdGetPtr (PcdSg2042FlashUid), sizeof (Key.Uid));
  } else {
    EFI_RNG_PROTOCOL *Rng;
    if (!Persistent || !Create) { return EFI_NOT_READY; }
    Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&Rng);
    if (EFI_ERROR (Status)) { return Status; }
    Status = Rng->GetRNG (Rng, NULL, sizeof (Key.Uid), Key.Uid);
    if (EFI_ERROR (Status)) { return Status; }
  }

  if (!Sha256HashAll (&Key, sizeof (Key), Hash)) { return EFI_DEVICE_ERROR; }
  CopyMem (Mac, Hash, 6);
  Mac[0] = (Mac[0] & 0xfc) | 2;
  if (Persistent && Create) {
    Status = gRT->SetVariable (Name, &gSg2042MacVariableGuid,
                               EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                               EFI_VARIABLE_RUNTIME_ACCESS, 6, Mac);
    if (EFI_ERROR (Status) && !HasUid) { return Status; }
  }
  return EFI_SUCCESS;
}

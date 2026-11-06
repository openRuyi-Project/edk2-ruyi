// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/NetworkBootConfigLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

EFI_STATUS
NetworkBootConfigGet (NETWORK_BOOT_CONFIG *Config)
{
  NETWORK_BOOT_CONFIG Saved;
  UINTN Size = sizeof (Saved);
  EFI_STATUS Status;

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Config, sizeof (*Config));
  Status = gRT->GetVariable (L"NetworkBoot", &gRuyiNetworkBootConfigGuid, NULL, &Size, &Saved);
  if (Status == EFI_NOT_FOUND || Status == EFI_BUFFER_TOO_SMALL) {
    return EFI_SUCCESS;
  }

  if (!EFI_ERROR (Status) && Size == sizeof (Saved) && Saved.Pxe <= 1 && Saved.Http <= 1) {
    *Config = Saved;
  }

  return Status;
}

EFI_STATUS
NetworkBootConfigSet (CONST NETWORK_BOOT_CONFIG *Config)
{
  NETWORK_BOOT_CONFIG Current;
  EFI_STATUS Status;

  if (Config == NULL || Config->Pxe > 1 || Config->Http > 1) {
    return EFI_INVALID_PARAMETER;
  }

  Status = NetworkBootConfigGet (&Current);
  if (EFI_ERROR (Status) || CompareMem (&Current, Config, sizeof (Current)) == 0) {
    return Status;
  }

  return gRT->SetVariable (L"NetworkBoot", &gRuyiNetworkBootConfigGuid,
                          EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                          sizeof (*Config), (VOID *)Config);
}

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/RngAlgorithm.h>
#include <Protocol/Rng.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/RngLib.h>
#include <Library/UefiBootServicesTableLib.h>

// FU740 has no entropy instruction. Expose only the explicitly unsafe
// algorithm for DHCP IDs and TCP sequence numbers, never RAW or a DRBG GUID.
STATIC EFI_STATUS EFIAPI
GetInfo (
  EFI_RNG_PROTOCOL   *This,
  UINTN              *Size,
  EFI_RNG_ALGORITHM  *Algorithms
  )
{
  if (!This || !Size) {
    return EFI_INVALID_PARAMETER;
  }

  UINTN  Capacity = *Size;

  *Size = sizeof (EFI_GUID);
  if (Capacity < *Size) {
    return EFI_BUFFER_TOO_SMALL;
  }

  if (!Algorithms) {
    return EFI_INVALID_PARAMETER;
  }

  CopyGuid (Algorithms, &gEdkiiRngAlgorithmUnSafe);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
GetRng (
  EFI_RNG_PROTOCOL   *This,
  EFI_RNG_ALGORITHM  *Algorithm,
  UINTN              Size,
  UINT8              *Data
  )
{
  if (!This || !Size || !Data) {
    return EFI_INVALID_PARAMETER;
  }

  if (Algorithm && !CompareGuid (Algorithm, &gEdkiiRngAlgorithmUnSafe)) {
    return EFI_UNSUPPORTED;
  }

  while (Size) {
    UINT64  Value;
    if (!GetRandomNumber64 (&Value)) {
      return EFI_DEVICE_ERROR;
    }

    UINTN  Count = MIN (Size, sizeof (Value));
    CopyMem (Data, &Value, Count);
    Data += Count;
    Size -= Count;
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
NetworkRngEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC EFI_RNG_PROTOCOL  Protocol = { GetInfo, GetRng };

  return gBS->InstallProtocolInterface (&Image, &gEfiRngProtocolGuid, EFI_NATIVE_INTERFACE, &Protocol);
}

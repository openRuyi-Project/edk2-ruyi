/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SnpAdapterInfoLib.h>

#define INFO_SIGNATURE  SIGNATURE_32('S','N','A','I')

STATIC SNP_ADAPTER_INFO *
Context (
  EFI_ADAPTER_INFORMATION_PROTOCOL  *This
  )
{
  if (!This) {
    return NULL;
  }

  SNP_ADAPTER_INFO  *Info = BASE_CR (This, SNP_ADAPTER_INFO, Protocol);

  return Info->Signature == INFO_SIGNATURE ? Info : NULL;
}

STATIC EFI_STATUS EFIAPI
Get (
  EFI_ADAPTER_INFORMATION_PROTOCOL  *This,
  EFI_GUID                          *Type,
  VOID                              **Block,
  UINTN                             *Bytes
  )
{
  SNP_ADAPTER_INFO  *Info = Context (This);

  if (!Info || !Type || !Block || !Bytes) {
    return EFI_INVALID_PARAMETER;
  }

  *Block = NULL;
  *Bytes = 0;
  if (CompareGuid (Type, &gEfiAdapterInfoMediaStateGuid)) {
    EFI_STATUS  Status = Info->Snp->GetStatus (Info->Snp, NULL, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    EFI_ADAPTER_INFO_MEDIA_STATE  *Media = AllocatePool (sizeof (*Media));
    if (!Media) {
      return EFI_OUT_OF_RESOURCES;
    }

    Media->MediaState = Info->Snp->Mode->MediaPresent ?
                        EFI_SUCCESS : EFI_NO_MEDIA;
    *Block = Media;
    *Bytes = sizeof (*Media);
    return EFI_SUCCESS;
  }

  if (CompareGuid (Type, &gEfiAdapterInfoUndiIpv6SupportGuid)) {
    EFI_ADAPTER_INFO_UNDI_IPV6_SUPPORT  *Ipv6 = AllocatePool (sizeof (*Ipv6));
    if (!Ipv6) {
      return EFI_OUT_OF_RESOURCES;
    }

    Ipv6->Ipv6Support = Info->Snp->Mode->IfType == 1 &&
                        Info->Snp->Mode->HwAddressSize == 6;
    *Block = Ipv6;
    *Bytes = sizeof (*Ipv6);
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI
Set (
  EFI_ADAPTER_INFORMATION_PROTOCOL  *This,
  EFI_GUID                          *Type,
  VOID                              *Block,
  UINTN                             Bytes
  )
{
  if (!Context (This) || !Type || !Block) {
    return EFI_INVALID_PARAMETER;
  }

  return CompareGuid (Type, &gEfiAdapterInfoMediaStateGuid) ||
         CompareGuid (Type, &gEfiAdapterInfoUndiIpv6SupportGuid) ?
         EFI_WRITE_PROTECTED : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI
Types (
  EFI_ADAPTER_INFORMATION_PROTOCOL  *This,
  EFI_GUID                          **Guids,
  UINTN                             *Count
  )
{
  if (!Context (This) || !Guids || !Count) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;
  *Guids = AllocatePool (2 * sizeof (**Guids));
  if (!*Guids) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyGuid (&(*Guids)[0], &gEfiAdapterInfoMediaStateGuid);
  CopyGuid (&(*Guids)[1], &gEfiAdapterInfoUndiIpv6SupportGuid);
  *Count = 2;
  return EFI_SUCCESS;
}

EFI_STATUS
SnpAdapterInfoInitialize (
  SNP_ADAPTER_INFO             *Info,
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  )
{
  if (!Info || !Snp || !Snp->Mode || !Snp->GetStatus) {
    return EFI_INVALID_PARAMETER;
  }

  Info->Signature = INFO_SIGNATURE;
  Info->Snp       = Snp;
  Info->Protocol  = (EFI_ADAPTER_INFORMATION_PROTOCOL) {
    Get, Set, Types
  };
  return EFI_SUCCESS;
}

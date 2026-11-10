/** @file
  Non-coherent PCI DMA mappings for EIC7700.

  This follows EmbeddedPkg NonCoherentIoMmuDxe and preserves the address
  limits required by 32-bit PCI DMA operations.

  Copyright (c) 2019, Linaro, Ltd. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <PiDxe.h>
#include <Library/DmaLib.h>
#include <Library/Eic7700DmaLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/IoMmu.h>

STATIC EFI_STATUS EFIAPI
SetAttribute (EDKII_IOMMU_PROTOCOL *This, EFI_HANDLE DeviceHandle,
              VOID *Mapping, UINT64 Access)
{
  if (!Mapping || (Access & ~(UINT64)(EDKII_IOMMU_ACCESS_READ |
                                     EDKII_IOMMU_ACCESS_WRITE))) {
    return EFI_INVALID_PARAMETER;
  }
  // This protocol supplies cache maintenance and bounce buffers, not
  // per-device isolation. Firmware memory protection remains in OpenSBI.
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Map (EDKII_IOMMU_PROTOCOL *This, EDKII_IOMMU_OPERATION Operation,
     VOID *Host, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Device, VOID **Mapping)
{
  DMA_MAP_OPERATION     DmaOperation;
  EFI_PHYSICAL_ADDRESS  Limit;

  switch (Operation) {
    case EdkiiIoMmuOperationBusMasterRead:
    case EdkiiIoMmuOperationBusMasterRead64:
      DmaOperation = MapOperationBusMasterRead;
      break;
    case EdkiiIoMmuOperationBusMasterWrite:
    case EdkiiIoMmuOperationBusMasterWrite64:
      DmaOperation = MapOperationBusMasterWrite;
      break;
    case EdkiiIoMmuOperationBusMasterCommonBuffer:
    case EdkiiIoMmuOperationBusMasterCommonBuffer64:
      DmaOperation = MapOperationBusMasterCommonBuffer;
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }
  Limit = Operation <= EdkiiIoMmuOperationBusMasterCommonBuffer ?
          MAX_UINT32 : MAX_UINT64;
  return Eic7700DmaMap (DmaOperation, Host, Bytes, Device, Mapping, Limit);
}

STATIC EFI_STATUS EFIAPI
Unmap (EDKII_IOMMU_PROTOCOL *This, VOID *Mapping)
{
  return DmaUnmap (Mapping);
}

STATIC EFI_STATUS EFIAPI
AllocateBuffer (EDKII_IOMMU_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
                EFI_MEMORY_TYPE MemoryType, UINTN Pages,
                VOID **Host, UINT64 Attributes)
{
  EFI_PHYSICAL_ADDRESS  Limit;

  if (Attributes & EDKII_IOMMU_ATTRIBUTE_INVALID_FOR_ALLOCATE_BUFFER) {
    return EFI_UNSUPPORTED;
  }
  // The protocol requires ignoring Type. The DMA attribute sets the limit.
  Limit = (Attributes & EDKII_IOMMU_ATTRIBUTE_DUAL_ADDRESS_CYCLE) ?
          MAX_UINT64 : MAX_UINT32;
  return Eic7700DmaAllocateBuffer (MemoryType, Pages, Host, Limit);
}

STATIC EFI_STATUS EFIAPI
FreeBuffer (EDKII_IOMMU_PROTOCOL *This, UINTN Pages, VOID *Host)
{
  return DmaFreeBuffer (Pages, Host);
}

STATIC EDKII_IOMMU_PROTOCOL mIoMmu = {
  EDKII_IOMMU_PROTOCOL_REVISION,
  SetAttribute,
  Map,
  Unmap,
  AllocateBuffer,
  FreeBuffer
};

EFI_STATUS EFIAPI
Eic7700IoMmuEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  return gBS->InstallProtocolInterface (&Image, &gEdkiiIoMmuProtocolGuid,
                                        EFI_NATIVE_INTERFACE, &mIoMmu);
}

/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DmaLib.h>
#include <Library/DebugLib.h>
#include <Library/K3DmaBufferLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>

#define K3_DRAM_START  0x100000000ULL
#define K3_CMO         BIT0
#define K3_PBMT        BIT2

typedef struct {
  LIST_ENTRY    Link;
  VOID          *Buffer;
  UINTN         Pages;
  UINT64        Attributes;
  UINTN         ActiveMaps;
  BOOLEAN       Usable;
} DMA_ALLOCATION;

typedef struct {
  LIST_ENTRY           Link;
  DMA_MAP_OPERATION    Operation;
  VOID                 *Host;
  VOID                 *DeviceBuffer;
  UINTN                Bytes;
  UINTN                BouncePages;
  DMA_ALLOCATION       *Allocation;
} DMA_MAPPING;

STATIC LIST_ENTRY             mAllocations;
STATIC LIST_ENTRY             mMappings;
STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;
STATIC UINTN                  mAlignment;

EFI_STATUS EFIAPI
SpacemitDmaConstructor (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  InitializeListHead (&mAllocations);
  InitializeListHead (&mMappings);
  mCpu       = NULL;
  mAlignment = 0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Cpu (
  VOID
  )
{
  EFI_STATUS  Status;

  if ((PcdGet64 (PcdRiscVFeatureOverride) & K3_CMO) == 0) {
    return EFI_UNSUPPORTED;
  }

  if (mCpu == NULL) {
    Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if ((mCpu->DmaBufferAlignment == 0) ||
      ((mCpu->DmaBufferAlignment & (mCpu->DmaBufferAlignment - 1)) != 0) ||
      (mCpu->DmaBufferAlignment > EFI_PAGE_SIZE))
  {
    return EFI_UNSUPPORTED;
  }

  // The CPU protocol reports DMA word alignment, not the CBOM granule.
  UINTN  CacheLine = PcdGet32 (PcdDmaCacheLineSize);

  if (!CacheLine || (CacheLine & (CacheLine - 1)) || (CacheLine > EFI_PAGE_SIZE)) {
    return EFI_UNSUPPORTED;
  }

  mAlignment = MAX (CacheLine, mCpu->DmaBufferAlignment);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
RamRange (
  UINTN   Address,
  UINTN   Bytes,
  UINT64  *Attributes
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status;
  UINTN                            End;

  if ((Bytes == 0) || (Address < K3_DRAM_START) || (Address > MAX_UINTN - Bytes)) {
    return EFI_INVALID_PARAMETER;
  }

  End = Address + Bytes;
  if (Attributes != NULL) {
    *Attributes = MAX_UINT64;
  }

  while (Address < End) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) ||
        (Descriptor.Length == 0) || (Descriptor.BaseAddress > Address) ||
        (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length) ||
        (Descriptor.BaseAddress + Descriptor.Length <= Address))
    {
      return EFI_UNSUPPORTED;
    }

    if (Attributes != NULL) {
      // Initial K3 DRAM mappings are cached through PMA even when GCD
      // reports no explicit cache bits. Treat that as WB before comparing
      // adjacent descriptors and recording the attributes restored on free.
      // Permission bits remain part of the comparison.
      UINT64  Effective = Descriptor.Attributes;
      if ((Effective & EFI_CACHE_ATTRIBUTE_MASK) == 0) {
        Effective |= EFI_MEMORY_WB;
      }

      if (*Attributes == MAX_UINT64) {
        *Attributes = Effective;
      } else if (*Attributes != Effective) {
        DEBUG ((
          DEBUG_ERROR,
          "K3 DMA: mixed GCD attributes at %lx: %lx versus %lx\n",
          (UINT64)Address,
          *Attributes,
          Descriptor.Attributes
          ));
        return EFI_UNSUPPORTED;
      }
    }

    Address = (UINTN)MIN ((UINT64)End, Descriptor.BaseAddress + Descriptor.Length);
  }

  return EFI_SUCCESS;
}

STATIC DMA_ALLOCATION *
AllocationFor (
  UINTN  Address,
  UINTN  Bytes
  )
{
  LIST_ENTRY      *Link;
  DMA_ALLOCATION  *Allocation;

  for (Link = GetFirstNode (&mAllocations); !IsNull (&mAllocations, Link);
       Link = GetNextNode (&mAllocations, Link))
  {
    Allocation = BASE_CR (Link, DMA_ALLOCATION, Link);
    UINTN  Base = (UINTN)Allocation->Buffer;
    UINTN  Size = EFI_PAGES_TO_SIZE (Allocation->Pages);
    if ((Address >= Base) && (Address - Base <= Size) &&
        (Bytes <= Size - (Address - Base)))
    {
      return Allocation;
    }
  }

  return NULL;
}

EFI_STATUS EFIAPI
DmaMap (
  DMA_MAP_OPERATION  Operation,
  VOID               *Host,
  UINTN              *Bytes,
  PHYSICAL_ADDRESS   *Device,
  VOID               **Mapping
  )
{
  DMA_MAPPING     *Map;
  DMA_ALLOCATION  *Allocation;
  EFI_STATUS      Status;
  UINTN           Size, Address;
  UINT64          Attributes;

  if ((Bytes == NULL) || (Device == NULL) || (Mapping == NULL) || (Host == NULL) ||
      ((UINT32)Operation >= MapOperationMaximum))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Mapping = NULL;
  *Device  = 0;
  Size     = *Bytes;
  Address  = (UINTN)Host;
  Status   = RamRange (Address, Size, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Cpu ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Allocation = AllocationFor (Address, Size);
  if (Allocation != NULL) {
    if (!Allocation->Usable || (Allocation->ActiveMaps == MAX_UINTN)) {
      return EFI_NOT_READY;
    }

    Status = RamRange (Address, Size, &Attributes);
    if (EFI_ERROR (Status) || ((Attributes & EFI_CACHE_ATTRIBUTE_MASK) != EFI_MEMORY_UC)) {
      return EFI_UNSUPPORTED;
    }
  }

  if ((Operation == MapOperationBusMasterCommonBuffer) && (Allocation == NULL)) {
    return EFI_UNSUPPORTED;
  }

  Map = AllocateZeroPool (sizeof (*Map));
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Map->Operation    = Operation;
  Map->Host         = Host;
  Map->DeviceBuffer = Host;
  Map->Bytes        = Size;
  Map->Allocation   = Allocation;
  // A device-write buffer sharing cache lines with unrelated data needs an
  // owned aligned bounce buffer. It still lives in normal high DRAM.
  if ((Allocation == NULL) && (Operation == MapOperationBusMasterWrite) &&
      (((Address | Size) & (mAlignment - 1)) != 0))
  {
    if (Size > MAX_UINTN - EFI_PAGE_MASK) {
      Status = EFI_INVALID_PARAMETER;
      goto Fail;
    }

    Map->BouncePages  = EFI_SIZE_TO_PAGES (Size);
    Map->DeviceBuffer = AllocatePages (Map->BouncePages);
    if (Map->DeviceBuffer == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Fail;
    }

    Status = RamRange ((UINTN)Map->DeviceBuffer, EFI_PAGES_TO_SIZE (Map->BouncePages), NULL);
    if (EFI_ERROR (Status)) {
      goto Fail;
    }
  }

  if (Allocation == NULL) {
    Status = mCpu->FlushDataCache (
                     mCpu,
                     (UINTN)Map->DeviceBuffer,
                     Size,
                     Operation == MapOperationBusMasterRead ? EfiCpuFlushTypeWriteBack :
                     EfiCpuFlushTypeWriteBackInvalidate
                     );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }
  }

  MemoryFence ();
  if (Allocation != NULL) {
    Allocation->ActiveMaps++;
  }

  InsertTailList (&mMappings, &Map->Link);
  *Device  = (PHYSICAL_ADDRESS)(UINTN)Map->DeviceBuffer;
  *Mapping = Map;
  return EFI_SUCCESS;
Fail:
  if ((Map->DeviceBuffer != NULL) && (Map->DeviceBuffer != Host)) {
    FreePages (Map->DeviceBuffer, Map->BouncePages);
  }

  FreePool (Map);
  return Status;
}

EFI_STATUS EFIAPI
DmaUnmap (
  VOID  *Mapping
  )
{
  LIST_ENTRY   *Link;
  DMA_MAPPING  *Map = NULL;
  EFI_STATUS   Status;

  for (Link = GetFirstNode (&mMappings); !IsNull (&mMappings, Link);
       Link = GetNextNode (&mMappings, Link))
  {
    DMA_MAPPING  *Candidate = BASE_CR (Link, DMA_MAPPING, Link);
    if (Candidate == Mapping) {
      Map = Candidate;
      break;
    }
  }

  if (Map == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // The caller must quiesce the DMA engine before unmapping, including errors.
  MemoryFence ();
  if ((Map->Allocation == NULL) && (Map->Operation == MapOperationBusMasterWrite)) {
    Status = mCpu->FlushDataCache (
                     mCpu,
                     (UINTN)Map->DeviceBuffer,
                     Map->Bytes,
                     EfiCpuFlushTypeInvalidate
                     );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Map->DeviceBuffer != Map->Host) {
      CopyMem (Map->Host, Map->DeviceBuffer, Map->Bytes);
    }
  }

  if (Map->DeviceBuffer != Map->Host) {
    FreePages (Map->DeviceBuffer, Map->BouncePages);
  }

  if (Map->Allocation != NULL) {
    Map->Allocation->ActiveMaps--;
  }

  RemoveEntryList (&Map->Link);
  FreePool (Map);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
DmaAllocateBuffer (
  EFI_MEMORY_TYPE  Type,
  UINTN            Pages,
  VOID             **Host
  )
{
  return DmaAllocateAlignedBuffer (Type, Pages, EFI_PAGE_SIZE, Host);
}

STATIC EFI_STATUS
AllocateCommon (
  EFI_MEMORY_TYPE       Type,
  UINTN                 Pages,
  UINTN                 Alignment,
  BOOLEAN               Bounded,
  EFI_PHYSICAL_ADDRESS  Maximum,
  VOID                  **Host
  )
{
  DMA_ALLOCATION  *Allocation;
  EFI_STATUS      Status, Restore;
  UINT64          Attributes;
  UINTN           Bytes;
  VOID            *Buffer;

  if ((Host == NULL) || (Pages == 0) || (Pages > (MAX_UINTN >> EFI_PAGE_SHIFT)) ||
      ((Type != EfiBootServicesData) && (Type != EfiRuntimeServicesData)) ||
      ((Alignment != 0) && (Alignment & (Alignment - 1))))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Host = NULL;
  Bytes = EFI_PAGES_TO_SIZE (Pages);
  if (Bounded && ((Maximum < K3_DRAM_START) || (Maximum > MAX_UINTN) ||
                  (Bytes - 1 > Maximum - K3_DRAM_START)))
  {
    return EFI_INVALID_PARAMETER;
  }

  // BaseRiscVMmuLib ignores UC/WC unless the PBMT override is enabled.
  // Do not return a supposedly shared buffer when it remains cached.
  if ((PcdGet64 (PcdRiscVFeatureOverride) & K3_PBMT) == 0) {
    return EFI_UNSUPPORTED;
  }

  Status = Cpu ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Alignment < EFI_PAGE_SIZE) {
    Alignment = EFI_PAGE_SIZE;
  }

  if (Alignment > MAX_UINTN - Bytes) {
    return EFI_INVALID_PARAMETER;
  }

  Allocation = AllocateZeroPool (sizeof (*Allocation));
  if (Allocation == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Bounded) {
    EFI_PHYSICAL_ADDRESS  Address = Maximum;
    Status = gBS->AllocatePages (AllocateMaxAddress, Type, Pages, &Address);
    if (EFI_ERROR (Status)) {
      FreePool (Allocation);
      return Status;
    }

    Buffer = (VOID *)(UINTN)Address;
    if ((Address & EFI_PAGE_MASK) || (Address > Maximum) || (Bytes - 1 > Maximum - Address)) {
      Status = EFI_UNSUPPORTED;
      goto Fail;
    }
  } else {
    Buffer = Type == EfiRuntimeServicesData ? AllocateAlignedRuntimePages (Pages, Alignment) :
             AllocateAlignedPages (Pages, Alignment);
  }

  if (Buffer == NULL) {
    FreePool (Allocation);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = RamRange ((UINTN)Buffer, Bytes, &Attributes);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  // Publish all dirty bytes before changing CPU memory type. No descriptor
  // or DMA mapping is visible to a device until this sequence completes.
  Status = mCpu->FlushDataCache (mCpu, (UINTN)Buffer, Bytes, EfiCpuFlushTypeWriteBackInvalidate);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  (UINTN)Buffer,
                  Bytes,
                  (Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC
                  );
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  Status = mCpu->FlushDataCache (mCpu, (UINTN)Buffer, Bytes, EfiCpuFlushTypeInvalidate);
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  Allocation->Buffer     = Buffer;
  Allocation->Pages      = Pages;
  Allocation->Attributes = Attributes;
  Allocation->Usable     = TRUE;
  InsertTailList (&mAllocations, &Allocation->Link);
  *Host = Buffer;
  return EFI_SUCCESS;
RestoreAttributes:
  Restore = gDS->SetMemorySpaceAttributes ((UINTN)Buffer, Bytes, Attributes);
  if (EFI_ERROR (Restore)) {
    // Quarantine uncertain memory. It cannot be mapped or silently recycled.
    Allocation->Buffer     = Buffer;
    Allocation->Pages      = Pages;
    Allocation->Attributes = Attributes;
    InsertTailList (&mAllocations, &Allocation->Link);
    return Restore;
  }

Fail:
  FreePages (Buffer, Pages);
  FreePool (Allocation);
  return Status;
}

EFI_STATUS EFIAPI
DmaAllocateAlignedBuffer (
  EFI_MEMORY_TYPE  Type,
  UINTN            Pages,
  UINTN            Alignment,
  VOID             **Host
  )
{
  return AllocateCommon (Type, Pages, Alignment, FALSE, 0, Host);
}

EFI_STATUS EFIAPI
K3DmaAllocateBufferBelow (
  EFI_MEMORY_TYPE       Type,
  UINTN                 Pages,
  EFI_PHYSICAL_ADDRESS  Maximum,
  VOID                  **Host
  )
{
  return AllocateCommon (Type, Pages, EFI_PAGE_SIZE, TRUE, Maximum, Host);
}

EFI_STATUS EFIAPI
DmaFreeBuffer (
  UINTN  Pages,
  VOID   *Host
  )
{
  DMA_ALLOCATION  *Allocation;
  EFI_STATUS      Status;

  if ((Host == NULL) || (Pages == 0) || (Pages > (MAX_UINTN >> EFI_PAGE_SHIFT))) {
    return EFI_INVALID_PARAMETER;
  }

  Allocation = AllocationFor ((UINTN)Host, EFI_PAGES_TO_SIZE (Pages));
  if ((Allocation == NULL) || (Allocation->Buffer != Host) || (Allocation->Pages != Pages)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Allocation->ActiveMaps != 0) {
    return EFI_ACCESS_DENIED;
  }

  MemoryFence ();
  Status = gDS->SetMemorySpaceAttributes ((UINTN)Host, EFI_PAGES_TO_SIZE (Pages), Allocation->Attributes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FreePages (Host, Pages);
  RemoveEntryList (&Allocation->Link);
  FreePool (Allocation);
  return EFI_SUCCESS;
}

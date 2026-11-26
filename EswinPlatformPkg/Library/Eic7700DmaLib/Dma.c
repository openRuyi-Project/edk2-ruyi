/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/Eic7702AddressMap.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DmaLib.h>
#include <Library/Eic7700DmaLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define CCACHE_CONFIG        0x000
#define CCACHE_BLOCK_SHIFT   24
#define PMA_ORDER_MASK       (BIT8 | BIT9)
#define CCACHE_FLUSH64       0x200
#define PMA_RW               (BIT0 | BIT1)
#define PMA_MEMORY           (BIT9 | BIT10 | BIT11)
#define PMA_CACHEABLE        BIT12
#define PMA_UNSAFE           BIT15
#define PMR_IS_ALIAS         BIT31
#define PMR_ALIAS_INDEX(F)   (((F) >> 24) & 0x7f)
#define PMR_CELLS            6

typedef struct {
  LIST_ENTRY    Link;
  VOID          *Cached;
  VOID          *Alias;
  UINTN         Pages;
  UINTN         ActiveMaps;
} DMA_ALLOCATION;

typedef struct {
  LIST_ENTRY          Link;
  DMA_MAP_OPERATION   Operation;
  VOID                *Host;
  UINTN               Bytes;
  DMA_ALLOCATION      *Allocation;
  VOID                *Bounce;
} DMA_MAPPING;

STATIC LIST_ENTRY  mAllocations = INITIALIZE_LIST_HEAD_VARIABLE (mAllocations);
STATIC LIST_ENTRY  mMappings    = INITIALIZE_LIST_HEAD_VARIABLE (mMappings);
typedef struct {
  UINT64  Cached;
  UINT64  Alias;
  UINT64  Size;
  UINTN   Cache;
  UINTN   OtherCache;
} DMA_WINDOW;

// EIC7702 also exposes a window striped across the two inclusive L3 caches.
STATIC DMA_WINDOW  mWindows[3];
STATIC UINTN       mWindowCount, mCacheLine;
STATIC BOOLEAN     mReady;

STATIC UINT64
Cells64 (CONST UINT32 *Cells)
{
  return LShiftU64 (Fdt32ToCpu (Cells[0]), 32) | Fdt32ToCpu (Cells[1]);
}

STATIC DMA_WINDOW *
WindowFor (UINTN Address, UINTN Bytes)
{
  for (UINTN Index = 0; Index < mWindowCount; Index++) {
    DMA_WINDOW *Window = &mWindows[Index];
    if (Address >= Window->Cached && Address - Window->Cached < Window->Size &&
        Bytes <= Window->Size - (Address - Window->Cached)) {
      return Window;
    }
  }
  return NULL;
}

STATIC EFI_STATUS
RamRange (UINTN Address, UINTN Bytes)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status;
  UINT64                           End = Address + Bytes;

  if (!Bytes || Address > MAX_UINTN - Bytes || WindowFor (Address, Bytes) == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  while (Address < End) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) ||
        !Descriptor.Length || (Descriptor.BaseAddress > Address) ||
        (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length) ||
        (Descriptor.BaseAddress + Descriptor.Length <= Address))
    {
      return EFI_UNSUPPORTED;
    }

    Address = MIN (End, Descriptor.BaseAddress + Descriptor.Length);
  }

  return EFI_SUCCESS;
}

STATIC VOID
Flush (UINTN Address, UINTN Bytes)
{
  UINTN  End = Address + Bytes;
  DMA_WINDOW *Window = WindowFor (Address, Bytes);

  ASSERT (Window != NULL);
  // Flush64 writes back and invalidates the inclusive cache hierarchy.
  // As in Linux sifive_ccache, fence the range rather than every cache line.
  // IoLib has no relaxed MMIO write, so use volatile 64-bit stores here.
  MemoryFence ();
  for (Address &= ~(mCacheLine - 1); Address < End; Address += mCacheLine) {
    UINTN Cache = Window->Cache;
    if (Window->OtherCache && (((Address >> 8) ^ (Address >> 18)) & 1)) {
      Cache = Window->OtherCache;
    }
    *(volatile UINT64 *)(Cache + CCACHE_FLUSH64) = Address;
  }
  MemoryFence ();
}

STATIC DMA_ALLOCATION *
AllocationFor (UINTN Address, UINTN Bytes)
{
  LIST_ENTRY      *Link;
  DMA_ALLOCATION  *Allocation;
  UINTN           Offset;

  BASE_LIST_FOR_EACH (Link, &mAllocations) {
    Allocation = BASE_CR (Link, DMA_ALLOCATION, Link);
    if (Address < (UINTN)Allocation->Alias) {
      continue;
    }

    Offset = Address - (UINTN)Allocation->Alias;
    if ((Offset < EFI_PAGES_TO_SIZE (Allocation->Pages)) &&
        (Bytes <= EFI_PAGES_TO_SIZE (Allocation->Pages) - Offset))
    {
      return Allocation;
    }
  }
  return NULL;
}

STATIC VOID *
AllocateBelow (EFI_MEMORY_TYPE Type, UINTN Pages, UINTN Alignment,
               EFI_PHYSICAL_ADDRESS MaximumAddress)
{
  EFI_PHYSICAL_ADDRESS  Address = MaximumAddress;
  EFI_STATUS            Status;
  UINTN                 Extra, Aligned, Before, After;

  Alignment = MAX (Alignment, EFI_PAGE_SIZE);
  Extra = EFI_SIZE_TO_PAGES (Alignment - EFI_PAGE_SIZE);
  if (Pages > (MAX_UINTN >> EFI_PAGE_SHIFT) - Extra) {
    return NULL;
  }
  Status = gBS->AllocatePages (AllocateMaxAddress, Type, Pages + Extra, &Address);
  if (EFI_ERROR (Status)) {
    return NULL;
  }
  Aligned = ALIGN_VALUE ((UINTN)Address, Alignment);
  Before  = EFI_SIZE_TO_PAGES (Aligned - (UINTN)Address);
  After   = Extra - Before;
  if (Before) {
    FreePages ((VOID *)(UINTN)Address, Before);
  }
  if (After) {
    FreePages ((VOID *)(Aligned + EFI_PAGES_TO_SIZE (Pages)), After);
  }
  return (VOID *)Aligned;
}

STATIC EFI_STATUS
Allocate (EFI_MEMORY_TYPE Type, UINTN Pages, UINTN Alignment,
          EFI_PHYSICAL_ADDRESS MaximumAddress, VOID **Host)
{
  DMA_ALLOCATION  *Allocation;
  EFI_STATUS      Status;
  UINTN           Bytes;
  UINT64          Attributes;

  if (!Host || !Pages || (Pages > (MAX_UINTN >> EFI_PAGE_SHIFT)) ||
      ((Type != EfiBootServicesData) && (Type != EfiRuntimeServicesData)) ||
      ((Alignment != 0) && ((Alignment & (Alignment - 1)) != 0)))
  {
    return EFI_INVALID_PARAMETER;
  }
  *Host = NULL;
  if (!mReady) {
    return EFI_NOT_READY;
  }

  Alignment = MAX (Alignment, EFI_PAGE_SIZE);
  Bytes     = EFI_PAGES_TO_SIZE (Pages);
  Allocation = AllocateZeroPool (sizeof (*Allocation));
  if (!Allocation) {
    return EFI_OUT_OF_RESOURCES;
  }

  Allocation->Cached = AllocateBelow (Type, Pages, Alignment, MaximumAddress);
  if (!Allocation->Cached) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeRecord;
  }

  Status = RamRange ((UINTN)Allocation->Cached, Bytes);
  if (EFI_ERROR (Status)) {
    goto FreeMemory;
  }
  DMA_WINDOW *Window = WindowFor ((UINTN)Allocation->Cached, Bytes);
  Allocation->Alias = (VOID *)(UINTN)(Window->Alias + ((UINTN)Allocation->Cached - Window->Cached));
  Allocation->Pages = Pages;
  if (((UINTN)Allocation->Alias & (Alignment - 1)) != 0) {
    Status = EFI_UNSUPPORTED;
    goto FreeMemory;
  }

  // No cached accesses to these pages are permitted while the alias is used.
  Flush ((UINTN)Allocation->Cached, Bytes);
  Attributes = EFI_MEMORY_UC | EFI_MEMORY_XP;
  if (Type == EfiRuntimeServicesData) {
    Attributes |= EFI_MEMORY_RUNTIME;
  }
  Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo,
                               (UINTN)Allocation->Alias, Bytes, Attributes);
  if (EFI_ERROR (Status)) {
    goto FreeMemory;
  }
  Status = gDS->SetMemorySpaceAttributes ((UINTN)Allocation->Alias, Bytes, Attributes);
  if (EFI_ERROR (Status)) {
    gDS->RemoveMemorySpace ((UINTN)Allocation->Alias, Bytes);
    goto FreeMemory;
  }

  InsertTailList (&mAllocations, &Allocation->Link);
  *Host = Allocation->Alias;
  return EFI_SUCCESS;

FreeMemory:
  FreePages (Allocation->Cached, Pages);
FreeRecord:
  FreePool (Allocation);
  return Status;
}

STATIC EFI_STATUS
Free (UINTN Pages, VOID *Host)
{
  DMA_ALLOCATION  *Allocation;
  EFI_STATUS      Status;

  if (!Pages || (Pages > (MAX_UINTN >> EFI_PAGE_SHIFT)) || !Host) {
    return EFI_INVALID_PARAMETER;
  }
  Allocation = AllocationFor ((UINTN)Host, EFI_PAGES_TO_SIZE (Pages));
  if (!Allocation || (Allocation->Alias != Host) || (Allocation->Pages != Pages)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Allocation->ActiveMaps) {
    return EFI_ACCESS_DENIED;
  }

  MemoryFence ();
  Status = gDS->RemoveMemorySpace ((UINTN)Host, EFI_PAGES_TO_SIZE (Pages));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Flush ((UINTN)Allocation->Cached, EFI_PAGES_TO_SIZE (Pages));
  FreePages (Allocation->Cached, Pages);
  RemoveEntryList (&Allocation->Link);
  FreePool (Allocation);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Map (DMA_MAP_OPERATION Operation, VOID *Host, UINTN *Bytes,
     PHYSICAL_ADDRESS *Device, VOID **Mapping,
     EFI_PHYSICAL_ADDRESS MaximumAddress)
{
  DMA_ALLOCATION  *Allocation;
  DMA_MAPPING     *Record;
  EFI_STATUS      Status;
  VOID            *Bounce;
  UINTN           Address;
  BOOLEAN         BounceNeeded;

  if (!Host || !Bytes || !*Bytes || !Device || !Mapping ||
      ((UINT32)Operation >= MapOperationMaximum) ||
      ((UINTN)Host > MAX_UINTN - *Bytes))
  {
    return EFI_INVALID_PARAMETER;
  }
  *Device  = 0;
  *Mapping = NULL;
  if (!mReady) {
    return EFI_NOT_READY;
  }

  Allocation = AllocationFor ((UINTN)Host, *Bytes);
  if (!Allocation) {
    if (Operation == MapOperationBusMasterCommonBuffer) {
      return EFI_UNSUPPORTED;
    }
    Status = RamRange ((UINTN)Host, *Bytes);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Address = Allocation ? (UINTN)Allocation->Cached + (UINTN)Host -
                         (UINTN)Allocation->Alias : (UINTN)Host;
  BounceNeeded = (*Bytes - 1 > MaximumAddress) ||
                 (Address > MaximumAddress - (*Bytes - 1)) ||
                 (!Allocation && (((Address | *Bytes) & (mCacheLine - 1)) != 0));
  if (BounceNeeded && (Operation == MapOperationBusMasterCommonBuffer)) {
    return EFI_UNSUPPORTED;
  }

  Record = AllocateZeroPool (sizeof (*Record));
  if (!Record) {
    return EFI_OUT_OF_RESOURCES;
  }
  Record->Operation  = Operation;
  Record->Host       = Host;
  Record->Bytes      = *Bytes;
  Record->Allocation = Allocation;

  if (BounceNeeded) {
    if (*Bytes > MAX_UINTN - EFI_PAGE_MASK) {
      Status = EFI_INVALID_PARAMETER;
      goto FreeRecord;
    }
    // Streaming bounce buffers are not shared concurrently with the device.
    // Keep CPU copies cacheable and transfer ownership with cache maintenance.
    Bounce = AllocateBelow (EfiBootServicesData, EFI_SIZE_TO_PAGES (*Bytes),
                            EFI_PAGE_SIZE, MaximumAddress);
    if (Bounce == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto FreeRecord;
    }
    Status = RamRange ((UINTN)Bounce, *Bytes);
    if (EFI_ERROR (Status)) {
      FreePages (Bounce, EFI_SIZE_TO_PAGES (*Bytes));
      goto FreeRecord;
    }
    Record->Bounce = Bounce;
    if (Operation == MapOperationBusMasterRead) {
      CopyMem (Bounce, Host, *Bytes);
    }
    Flush ((UINTN)Bounce, *Bytes);
    *Device = (UINTN)Bounce;
  } else if (Allocation) {
    *Device = Address;
  } else {
    Flush ((UINTN)Host, *Bytes);
    *Device = (UINTN)Host;
  }

  if (Allocation) {
    Allocation->ActiveMaps++;
  }
  MemoryFence ();
  InsertTailList (&mMappings, &Record->Link);
  *Mapping = Record;
  return EFI_SUCCESS;

FreeRecord:
  FreePool (Record);
  return Status;
}

STATIC EFI_STATUS
Unmap (VOID *Mapping)
{
  LIST_ENTRY   *Link;
  DMA_MAPPING  *Record = NULL;

  BASE_LIST_FOR_EACH (Link, &mMappings) {
    if (BASE_CR (Link, DMA_MAPPING, Link) == Mapping) {
      Record = Mapping;
      break;
    }
  }
  if (!Record) {
    return EFI_INVALID_PARAMETER;
  }

  MemoryFence ();
  if (Record->Operation == MapOperationBusMasterWrite) {
    if (Record->Bounce) {
      Flush ((UINTN)Record->Bounce, Record->Bytes);
      CopyMem (Record->Host, Record->Bounce, Record->Bytes);
    } else if (!Record->Allocation) {
      Flush ((UINTN)Record->Host, Record->Bytes);
    }
  }
  if (Record->Allocation) {
    Record->Allocation->ActiveMaps--;
  }
  if (Record->Bounce) {
    FreePages (Record->Bounce, EFI_SIZE_TO_PAGES (Record->Bytes));
  }
  RemoveEntryList (&Record->Link);
  FreePool (Record);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Eic7700DmaMap (DMA_MAP_OPERATION Operation, VOID *Host, UINTN *Bytes,
               PHYSICAL_ADDRESS *Device, VOID **Mapping,
               EFI_PHYSICAL_ADDRESS MaximumAddress)
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Map (Operation, Host, Bytes, Device, Mapping, MaximumAddress);
  gBS->RestoreTPL (Old);
  return Status;
}

EFI_STATUS EFIAPI
DmaMap (DMA_MAP_OPERATION Operation, VOID *Host, UINTN *Bytes,
        PHYSICAL_ADDRESS *Device, VOID **Mapping)
{
  return Eic7700DmaMap (Operation, Host, Bytes, Device, Mapping, MAX_UINT64);
}

EFI_STATUS EFIAPI
DmaUnmap (VOID *Mapping)
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Unmap (Mapping);
  gBS->RestoreTPL (Old);
  return Status;
}

EFI_STATUS EFIAPI
DmaAllocateAlignedBuffer (EFI_MEMORY_TYPE Type, UINTN Pages, UINTN Alignment, VOID **Host)
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Allocate (Type, Pages, Alignment, MAX_UINT64, Host);
  gBS->RestoreTPL (Old);
  return Status;
}

EFI_STATUS EFIAPI
Eic7700DmaAllocateBuffer (EFI_MEMORY_TYPE Type, UINTN Pages, VOID **Host,
                         EFI_PHYSICAL_ADDRESS MaximumAddress)
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Allocate (Type, Pages, EFI_PAGE_SIZE, MaximumAddress, Host);
  gBS->RestoreTPL (Old);
  return Status;
}

EFI_STATUS EFIAPI
DmaAllocateBuffer (EFI_MEMORY_TYPE Type, UINTN Pages, VOID **Host)
{
  return DmaAllocateAlignedBuffer (Type, Pages, EFI_PAGE_SIZE, Host);
}

EFI_STATUS EFIAPI
DmaFreeBuffer (UINTN Pages, VOID *Host)
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Free (Pages, Host);
  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS
CacheForNode (CONST VOID *Fdt, UINT32 Id, UINTN *Cache)
{
  CONST UINT32 *Prop;
  INT32 Node, Length;
  UINTN Line, Base;
  EFI_STATUS Status;

  Node = -1;
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "eswin,eic7700-l3-cache")) >= 0) {
    if (!FdtNodeEnabled (Fdt, Node)) { continue; }
    Status = FdtGetMmio (Fdt, Node, 0, CCACHE_FLUSH64 + sizeof (UINT64), &Base, NULL);
    if (EFI_ERROR (Status)) { return Status; }
    if (EIC7702_MMIO_DIE (Base) != Id) { continue; }
    Prop = FdtGetProp (Fdt, Node, "cache-block-size", &Length);
    if (Prop == NULL || Length != sizeof (*Prop)) {
      return EFI_COMPROMISED_DATA;
    }
    Line = Fdt32ToCpu (*Prop);
    if (!Line || (Line & (Line - 1)) || Line > EFI_PAGE_SIZE || (mCacheLine && Line != mCacheLine)) {
      return EFI_UNSUPPORTED;
    }
    Status = FdtMapMmio (Base, EFI_PAGE_SIZE);
    if (EFI_ERROR (Status)) { return Status; }
    UINT32 Shift = MmioRead32 (Base + CCACHE_CONFIG) >> CCACHE_BLOCK_SHIFT;
    if (Shift > EFI_PAGE_SHIFT || (1U << Shift) != Line) {
      return EFI_COMPROMISED_DATA;
    }
    *Cache = Base;
    mCacheLine = Line;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
BindCache (CONST VOID *Fdt, DMA_WINDOW *Window)
{
  CONST UINT32 *Reg, *Numa;
  UINT32 Id = 0;
  INT32 Node, Length;
  BOOLEAN Found = FALSE;
  EFI_STATUS Status;

  FdtForEachSubnode (Node, Fdt, 0) {
    CONST CHAR8 *Type = FdtGetProp (Fdt, Node, "device_type", &Length);
    if (Type == NULL || Length != sizeof ("memory") || CompareMem (Type, "memory", Length)) {
      continue;
    }
    Reg = FdtGetProp (Fdt, Node, "reg", &Length);
    if (Reg == NULL || Length != 4 * sizeof (*Reg)) {
      continue;
    }
    UINT64 Base = Cells64 (Reg);
    if (Base < Window->Cached || Base - Window->Cached >= Window->Size ||
        Cells64 (Reg + 2) > Window->Size - (Base - Window->Cached)) {
      continue;
    }
    Numa = FdtGetProp (Fdt, Node, "numa-node-id", &Length);
    if (Numa != NULL && Length != sizeof (*Numa)) {
      return EFI_COMPROMISED_DATA;
    }
    Id = Numa == NULL ? 0 : Fdt32ToCpu (*Numa);
    Found = TRUE;
    break;
  }
  if (!Found) {
    return EFI_NOT_FOUND;
  }

  if ((Window->Cached & BIT38) != 0 && FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7702") >= 0) {
    Status = CacheForNode (Fdt, 1, &Window->OtherCache);
    if (EFI_ERROR (Status)) { return Status; }
    Id = 0;
  }
  return CacheForNode (Fdt, Id, &Window->Cache);
}

EFI_STATUS EFIAPI
Eic7700DmaInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  RUYI_BOARD_INFO  Info;
  CONST VOID      *Fdt;
  CONST UINT32    *Regions, *Alias, *Primary;
  UINT32          Flags, PrimaryFlags;
  UINTN           Count, Index, Reference;
  INT32           Length;
  EFI_STATUS      Status;

  if (mReady) { return EFI_SUCCESS; }
  mWindowCount = 0;
  mCacheLine = 0;
  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((FdtAddressCells (Fdt, 0) != 2) || (FdtSizeCells (Fdt, 0) != 2)) {
    return EFI_UNSUPPORTED;
  }
  Regions = FdtGetProp (Fdt, 0, "riscv,physical-memory-regions", &Length);
  if (!Regions || (Length <= 0) || (Length % (PMR_CELLS * sizeof (UINT32)))) {
    return EFI_UNSUPPORTED;
  }
  Count = Length / (PMR_CELLS * sizeof (UINT32));
  for (Index = 0; Index < Count; Index++) {
    Alias = Regions + Index * PMR_CELLS;
    Flags = Fdt32ToCpu (Alias[4]);
    Reference = PMR_ALIAS_INDEX (Flags);
    if (!(Flags & PMR_IS_ALIAS) || (Reference >= Index) ||
        (Flags & (PMA_CACHEABLE | PMA_UNSAFE)) ||
        ((Flags & PMA_ORDER_MASK) != BIT9) ||
        ((Flags & (PMA_RW | PMA_MEMORY)) != (PMA_RW | PMA_MEMORY)) || Alias[5])
    {
      continue;
    }
    Primary      = Regions + Reference * PMR_CELLS;
    PrimaryFlags = Fdt32ToCpu (Primary[4]);
    if (!(PrimaryFlags & PMA_CACHEABLE) || ((PrimaryFlags & PMA_RW) != PMA_RW) ||
        (PrimaryFlags & (PMR_IS_ALIAS | PMA_UNSAFE)) || Primary[5])
    {
      continue;
    }
    DMA_WINDOW Window = { Cells64 (Primary), Cells64 (Alias),
                          MIN (Cells64 (Primary + 2), Cells64 (Alias + 2)), 0, 0 };
    if (!Window.Size || ((Window.Cached | Window.Alias | Window.Size) & EFI_PAGE_MASK) ||
        Window.Cached > MAX_UINT64 - Window.Size || Window.Alias > MAX_UINT64 - Window.Size ||
        (Window.Alias < Window.Cached + Window.Size && Window.Cached < Window.Alias + Window.Size)) {
      return EFI_COMPROMISED_DATA;
    }
    Status = BindCache (Fdt, &Window);
    if (Status == EFI_NOT_FOUND) { continue; }
    if (EFI_ERROR (Status)) { return Status; }
    if (mWindowCount == ARRAY_SIZE (mWindows)) { return EFI_UNSUPPORTED; }
    for (UINTN I = 0; I < mWindowCount; I++) {
      DMA_WINDOW *Other = &mWindows[I];
      if ((Window.Cached < Other->Cached + Other->Size && Other->Cached < Window.Cached + Window.Size) ||
          (Window.Alias < Other->Alias + Other->Size && Other->Alias < Window.Alias + Window.Size) ||
          (Window.Cached < Other->Alias + Other->Size && Other->Alias < Window.Cached + Window.Size) ||
          (Window.Alias < Other->Cached + Other->Size && Other->Cached < Window.Alias + Window.Size)) {
        return EFI_COMPROMISED_DATA;
      }
    }
    mWindows[mWindowCount++] = Window;
    DEBUG ((DEBUG_INFO, "EIC770x DMA: cached=%Lx alias=%Lx L3=%lx\n", Window.Cached, Window.Alias, Window.Cache));
  }
  if (mWindowCount == 0) { return EFI_UNSUPPORTED; }
  mReady = TRUE;
  return EFI_SUCCESS;
}

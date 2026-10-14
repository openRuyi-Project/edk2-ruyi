/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <Guid/FdtHob.h>
#include <Guid/K3DdrTraining.h>
#include <Guid/MemoryTypeInformation.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrePiLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/SerialPortLib.h>
#include "Cpu.h"

VOID EFIAPI
ProcessLibraryConstructorList (
  VOID
  );

EFI_STATUS
PublishMemory (
  IN CONST VOID  *Fdt,
  IN UINT64      WorkspaceBase,
  IN UINT64      WorkspaceSize
  );

VOID
NORETURN
EFIAPI
SecEntry (
  IN UINTN       BootHart,
  IN CONST VOID  *Fdt
  )
{
  UINT64                       Base, Size, StackSize, FdtAddress;
  UINTN                        FdtSize, Pages;
  VOID                         *Copy;
  EFI_STATUS                   Status;
  EFI_HOB_HANDOFF_INFO_TABLE   *Hobs;
  RISCV_SEC_HANDOFF_DATA       Handoff;
  CONST EFI_GUID               HandoffGuid   = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_MEMORY_TYPE_INFORMATION  MemoryTypes[] = {
    { EfiRuntimeServicesCode, FixedPcdGet32 (PcdMemoryTypeEfiRuntimeServicesCode) },
    { EfiRuntimeServicesData, FixedPcdGet32 (PcdMemoryTypeEfiRuntimeServicesData) },
    { EfiMaxMemoryType,       0                                                   }
  };

  SerialPortInitialize ();
  DEBUG ((DEBUG_INIT, "SpacemitPlatformPkg SEC: hart=%lu FDT=%p\n", BootHart, Fdt));
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    DEBUG ((DEBUG_ERROR, "Invalid FDT handoff\n"));
    CpuDeadLoop ();
  }

  FdtSize = FdtTotalSize (Fdt);
  if ((FdtSize < sizeof (FDT_HEADER)) || (FdtSize > SIZE_1MB)) {
    CpuDeadLoop ();
  }

  Status = SelectBootHart (BootHart, Fdt);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CPU boot-hart handoff failed: %r\n", Status));
    CpuDeadLoop ();
  }

  Base      = FixedPcdGet64 (PcdSecMemoryBase);
  Size      = FixedPcdGet64 (PcdSecMemorySize);
  StackSize = FixedPcdGet32 (PcdSecStackSize);
  Hobs      = HobConstructor (
                (VOID *)(UINTN)Base,
                Size,
                (VOID *)(UINTN)Base,
                (VOID *)(UINTN)(Base + Size - StackSize)
                );
  PrePeiSetHobList (Hobs);
  Status = PublishMemory (Fdt, Base, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FDT memory publication failed: %r\n", Status));
    CpuDeadLoop ();
  }

  // SPL-compatible cold-training result. Quickboot clears the magic, so
  // only a newly trained record is handed to DXE for persistent storage.
  CONST K3_DDR_TRAINING_RECORD  *Training = (CONST VOID *)(UINTN)K3_DDR_TRAINING_ADDRESS;

  if ((Training->Magic == K3_DDR_TRAINING_MAGIC) &&
      (Training->Crc32 == CalculateCrc32 ((VOID *)&Training->ChipId, sizeof (*Training) - 8)))
  {
    if (BuildGuidDataHob (&gSpacemitDdrTrainingGuid, (VOID *)Training, sizeof (*Training)) == NULL) {
      CpuDeadLoop ();
    }
  }

  // Keep runtime allocations together instead of interleaving boot memory.
  BuildGuidDataHob (&gEfiMemoryTypeInformationGuid, MemoryTypes, sizeof (MemoryTypes));

  // PCI I/O cycles use translated CPU MMIO apertures on K3. The generic
  // host bridge allocates their host addresses from the GCD I/O map, so
  // its address namespace must cover the same physical width as memory.
  BuildCpuHob (56, 56);
  SetBootMode (
    (MmioRead32 (0xd4282d10) & 0xfff) == 0x55a ?
    BOOT_IN_RECOVERY_MODE : BOOT_WITH_FULL_CONFIGURATION
    );
  BuildStackHob (Base + Size - StackSize, StackSize);

  Pages = EFI_SIZE_TO_PAGES (FdtSize + SIZE_4KB);
  Copy  = AllocatePages (Pages);
  if ((Copy == NULL) || (FdtOpenInto (Fdt, Copy, EFI_PAGES_TO_SIZE (Pages)) != 0)) {
    CpuDeadLoop ();
  }

  Status = FixupCpuStatus (BootHart, Copy);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CPU device tree selection failed: %r\n", Status));
    CpuDeadLoop ();
  }

  FdtAddress         = (UINTN)Copy;
  Handoff.BootHartId = BootHart;
  Handoff.FdtPointer = Copy;
  if ((BuildGuidDataHob (&gFdtHobGuid, &FdtAddress, sizeof (FdtAddress)) == NULL) ||
      (BuildGuidDataHob (&HandoffGuid, &Handoff, sizeof (Handoff)) == NULL))
  {
    CpuDeadLoop ();
  }

  BuildFvHob (FixedPcdGet64 (PcdFdBase), FixedPcdGet32 (PcdFdSize));
  BuildMemoryAllocationHob (
    FixedPcdGet64 (PcdFdBase),
    FixedPcdGet32 (PcdFdSize),
    EfiBootServicesCode
    );
  ProcessLibraryConstructorList ();
  Status = DecompressFirstFv ();
  if (!EFI_ERROR (Status)) {
    Status = LoadDxeCoreFromFv (NULL, 0);
  }

  DEBUG ((DEBUG_ERROR, "DXE transfer returned: %r\n", Status));
  CpuDeadLoop ();
  UNREACHABLE ();
}

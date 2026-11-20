/** SPDX-License-Identifier: BSD-2-Clause-Patent
    SG2042 SPIFMC register protocol from SOPHGO's SpifmcDxe.
**/
#include <Library/CpuMmioAddressLib.h>
#include <PiDxe.h>
#include <Library/Sg2042SpiNorLib.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/NvStorageLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

STATIC SG2042_SPI_NOR mNor;

STATIC EFI_STATUS
Read (VOID *Context, UINT32 Offset, UINTN Bytes, VOID *Buffer)
{
  return Sg2042SpiRead (&mNor, Offset, Bytes, Buffer);
}

STATIC EFI_STATUS
Program (VOID *Context, UINT32 Offset, UINTN Bytes, CONST VOID *Buffer)
{
  if (Offset < SG2042_VARIABLE_BASE || Offset - SG2042_VARIABLE_BASE >= NV_TOTAL_SIZE ||
      Bytes > NV_TOTAL_SIZE - (Offset - SG2042_VARIABLE_BASE)) { return EFI_INVALID_PARAMETER; }
  return Sg2042SpiProgram (&mNor, Offset, Bytes, Buffer);
}

STATIC EFI_STATUS
Erase (VOID *Context, UINT32 Offset)
{
  if (Offset < SG2042_VARIABLE_BASE || Offset - SG2042_VARIABLE_BASE >= NV_TOTAL_SIZE) { return EFI_INVALID_PARAMETER; }
  return Sg2042SpiErase (&mNor, Offset);
}

STATIC VOID
VirtualChange (VOID)
{
  EfiConvertPointer (0, (VOID **)&mNor.Base);
  EfiConvertPointer (0, (VOID **)&mNor.Gate);
}

STATIC EFI_STATUS
RamVariables (VOID)
{
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status = PcdSetBoolS (PcdEmuVariableNvModeEnable, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gRuyiNvFvbReadyGuid, NULL,
                &gEdkiiNvVarStoreFormattedGuid, NULL, NULL);
}

STATIC
EFI_STATUS
MapFlashPage (UINTN Address)
{
  return FdtMapMmio (Address, EFI_PAGE_SIZE);
}

STATIC
VOID
ReadGdFlashUid (VOID)
{
  UINT8 Uid[16], Any = 0, All = 0xff;
  UINTN Size = sizeof (Uid);
  if (EFI_ERROR (Sg2042SpiReadUid (&mNor, Uid))) { return; }
  for (UINTN I = 0; I < Size; I++) { Any |= Uid[I]; All &= Uid[I]; }
  if (Any != 0 && All != 0xff) { PcdSetPtrS (PcdSg2042FlashUid, &Size, Uid); }
}

STATIC
BOOLEAN
HasVariablePartition (VOID)
{
  UINT8 Entry[56];
  for (UINTN I = 0; I < EFI_PAGE_SIZE / sizeof (Entry); I++) {
    EFI_STATUS Status = Read (NULL, SG2042_SCP_TABLE + (UINT32)(I * sizeof (Entry)), sizeof (Entry), Entry);
    if (EFI_ERROR (Status) || ReadUnaligned32 ((VOID *)Entry) != SG2042_SCP_MAGIC) { break; }
    if (!CompareMem (Entry + 4, "ruyi-vars", sizeof ("ruyi-vars")) &&
        ReadUnaligned32 ((VOID *)(Entry + 36)) == SG2042_VARIABLE_BASE &&
        ReadUnaligned32 ((VOID *)(Entry + 40)) == NV_TOTAL_SIZE) { return TRUE; }
  }
  return FALSE;
}

EFI_STATUS EFIAPI
Sg2042NvEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  NV_FLASH_OPS Ops = { Read, Program, Erase, NULL };
  EFI_STATUS Status;
  VOID *Hob = GetFirstGuidHob (&gFdtHobGuid);
  if (!Hob || GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64)) { return EFI_NOT_FOUND; }
  VOID *Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
  INT32 FirmwareNode = -1;
  while ((FirmwareNode = FdtNodeOffsetByCompatible (Fdt, FirmwareNode, "sophgo,sg2042-spifmc-nor")) >= 0) {
    UINTN Address;
    if (FdtNodeEnabled (Fdt, FirmwareNode) &&
        !EFI_ERROR (FdtGetMmio (Fdt, FirmwareNode, 0, EFI_PAGE_SIZE, &Address, NULL)) &&
        Address == SG2042_FIRMWARE_SPI) { break; }
  }
  if (FirmwareNode < 0) { return EFI_NOT_FOUND; }
  // Reserve the boot flash even on an SD boot with the factory partition table.
  if (FdtSetProp (Fdt, FirmwareNode, "status", "reserved", sizeof ("reserved"))) { return EFI_OUT_OF_RESOURCES; }
  INT32 Node = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  if (EFI_ERROR (FdtGetMmio (Fdt, Node, 0, sizeof (UINT32), &mNor.Gate, NULL)) ||
      EFI_ERROR (MapFlashPage (mNor.Gate))) { return RamVariables (); }
  MmioOr32 (CpuMmioAddress (mNor.Gate), BIT9);

  // The vendor firmware layout uses SPI1. Also accept an existing Ruyi
  // variable store on SPI0 if SPI1 has none.
  INT32 BootFlash = -1;
  UINTN StoreBase = 0;
  for (Node = -1; (Node = FdtNodeOffsetByCompatible (Fdt, Node, "sophgo,sg2042-spifmc-nor")) >= 0;) {
    UINT8 Id[3];
    if ((Node != FirmwareNode && !FdtNodeEnabled (Fdt, Node)) ||
        EFI_ERROR (FdtGetMmio (Fdt, Node, 0, EFI_PAGE_SIZE, &mNor.Base, NULL)) ||
        EFI_ERROR (MapFlashPage (mNor.Base))) { continue; }
    Status = Sg2042SpiReadId (&mNor, Id);
    if (EFI_ERROR (Status)) { continue; }
    if (Id[0] == 0xc8 && Node == FirmwareNode) { ReadGdFlashUid (); }
    if (HasVariablePartition () && (BootFlash < 0 || mNor.Base == SG2042_FIRMWARE_SPI)) {
      BootFlash = Node;
      StoreBase = mNor.Base;
    }
  }
  if (BootFlash < 0) { return RamVariables (); }
  mNor.Base = StoreBase;
  UINTN Pages[2] = {mNor.Base, mNor.Gate};
  for (UINTN I = 0; I < ARRAY_SIZE (Pages); I++) {
    Status = gDS->SetMemorySpaceAttributes (Pages[I], EFI_PAGE_SIZE, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) { return RamVariables (); }
  }
  // Linux must not drive the runtime variable controller concurrently.
  // "okay" and "fail" both fit in place, but reserved can require FDT growth.
  Status = FdtSetProp (Fdt, BootFlash, "status", "reserved", sizeof ("reserved")) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
  if (EFI_ERROR (Status)) { return RamVariables (); }
  Status = NvStorageInstall (&Ops, SG2042_VARIABLE_BASE, VirtualChange);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SG2042: SPI variable store unavailable (%r), using RAM variables\n", Status));
    return RamVariables ();
  }
  return EFI_SUCCESS;
}

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
#include <Library/BaseRiscVSbiLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/NvStorageLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

STATIC SG2042_SPI_NOR mNor;

// Private SG2042 SBI interface. Offsets are relative to ruyi-vars.
#define SG2042_NV_EID  0x08534e56
#define SG2042_NV_ABI  0x534e5601
enum { NvProbe, NvRead, NvProgram, NvErase };

STATIC EFI_STATUS
NvStatus (SBI_RET Ret)
{
  switch ((INTN)Ret.Error) {
    case SBI_SUCCESS:           return EFI_SUCCESS;
    case SBI_ERR_NOT_SUPPORTED: return EFI_UNSUPPORTED;
    case SBI_ERR_INVALID_PARAM: return EFI_INVALID_PARAMETER;
    case SBI_ERR_DENIED:        return EFI_ACCESS_DENIED;
    case SBI_ERR_INVALID_STATE: return EFI_NOT_READY;
    case SBI_ERR_TIMEOUT:       return EFI_TIMEOUT;
    default:                    return EFI_DEVICE_ERROR;
  }
}

STATIC BOOLEAN
InStore (UINT32 Offset, UINTN Bytes)
{
  return Offset >= SG2042_VARIABLE_BASE &&
         Offset - SG2042_VARIABLE_BASE <= NV_TOTAL_SIZE &&
         Bytes <= NV_TOTAL_SIZE - (Offset - SG2042_VARIABLE_BASE);
}

STATIC EFI_STATUS
Read (VOID *Context, UINT32 Offset, UINTN Bytes, VOID *Buffer)
{
  if (!InStore (Offset, Bytes)) { return EFI_INVALID_PARAMETER; }
  // Initializing the mirror reads the entire partition before ExitBootServices.
  if (!EfiAtRuntime ()) { return Sg2042SpiRead (&mNor, Offset, Bytes, Buffer); }
  UINT8 *Out = Buffer;
  while (Bytes > 0) {
    UINTN Chunk = MIN (Bytes, sizeof (UINTN));
    SBI_RET Ret = SbiCall (SG2042_NV_EID, NvRead, 2, (UINTN)(Offset - SG2042_VARIABLE_BASE), Chunk);
    if (Ret.Error != SBI_SUCCESS) { return NvStatus (Ret); }
    CopyMem (Out, &Ret.Value, Chunk);
    Out += Chunk;
    Offset += (UINT32)Chunk;
    Bytes -= Chunk;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Program (VOID *Context, UINT32 Offset, UINTN Bytes, CONST VOID *Buffer)
{
  CONST UINT8 *In = Buffer;
  if (!InStore (Offset, Bytes)) { return EFI_INVALID_PARAMETER; }
  while (Bytes > 0) {
    UINTN Words[4] = { 0 };
    UINTN Chunk = MIN (Bytes, MIN (sizeof (Words), 256U - (Offset & 255U)));
    CopyMem (Words, In, Chunk);
    SBI_RET Ret = SbiCall (SG2042_NV_EID, NvProgram, 6, (UINTN)(Offset - SG2042_VARIABLE_BASE),
                           Chunk, Words[0], Words[1], Words[2], Words[3]);
    if (Ret.Error != SBI_SUCCESS) { return NvStatus (Ret); }
    In += Chunk;
    Offset += (UINT32)Chunk;
    Bytes -= Chunk;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Erase (VOID *Context, UINT32 Offset)
{
  if (!InStore (Offset, NV_BLOCK_SIZE) || Offset % NV_BLOCK_SIZE) { return EFI_INVALID_PARAMETER; }
  return NvStatus (SbiCall (SG2042_NV_EID, NvErase, 1, (UINTN)(Offset - SG2042_VARIABLE_BASE)));
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

STATIC EFI_STATUS
ProtectFlashClock (VOID *Fdt, INT32 Node)
{
  INT32 Len;
  CONST UINT32 *Clocks = FdtGetProp (Fdt, Node, "clocks", &Len);
  if (Clocks == NULL || Len != 2 * sizeof (*Clocks)) { return EFI_COMPROMISED_DATA; }

  INT32 Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[0]));
  if (Provider < 0 || FdtStringListSearch (Fdt, Provider, "compatible", "sophgo,sg2042-clkgen") < 0 ||
      Fdt32ToCpu (Clocks[1]) != 46) { // GATE_CLK_AHB_SF
    return EFI_COMPROMISED_DATA;
  }
  EFI_STATUS Status = FdtGetMmio (Fdt, Provider, 0, sizeof (UINT32), &mNor.Gate, NULL);
  if (EFI_ERROR (Status)) { return Status; }

  UINT32 Gate = Clocks[1];
  CONST UINT32 *List = FdtGetProp (Fdt, Provider, "protected-clocks", &Len);
  if (List != NULL) {
    if (Len % sizeof (*List)) { return EFI_COMPROMISED_DATA; }
    for (UINTN I = 0; I < (UINTN)Len / sizeof (*List); I++) {
      if (List[I] == Gate) { return EFI_SUCCESS; }
    }
  }
  return FdtAppendProp (Fdt, Provider, "protected-clocks", &Gate, sizeof (Gate)) ?
         EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
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
    if (FdtNodeEnabled (Fdt, FirmwareNode) &&
        !EFI_ERROR (FdtGetMmio (Fdt, FirmwareNode, 0, EFI_PAGE_SIZE, &mNor.Base, NULL)) &&
        mNor.Base == SG2042_FIRMWARE_SPI) { break; }
  }
  if (FirmwareNode < 0) { return RamVariables (); }
  // Keep the boot flash reserved on SD boots and with the factory layout.
  if (FdtSetProp (Fdt, FirmwareNode, "status", "reserved", sizeof ("reserved"))) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (EFI_ERROR (ProtectFlashClock (Fdt, FirmwareNode)) || EFI_ERROR (MapFlashPage (mNor.Gate))) {
    return RamVariables ();
  }
  MmioOr32 (CpuMmioAddress (mNor.Gate), SG2042_SPIFMC_GATE);

  UINT8 Id[3];
  if (EFI_ERROR (MapFlashPage (mNor.Base)) || EFI_ERROR (Sg2042SpiReadId (&mNor, Id))) {
    return RamVariables ();
  }
  if (Id[0] == 0xc8) { ReadGdFlashUid (); }
  SBI_RET Probe = SbiCall (SG2042_NV_EID, NvProbe, 0);
  if ((Probe.Error != SBI_SUCCESS) || (Probe.Value != SG2042_NV_ABI)) {
    DEBUG ((DEBUG_WARN, "SG2042: SBI variable store unavailable, using RAM variables\n"));
    return RamVariables ();
  }
  Status = NvStorageInstall (&Ops, SG2042_VARIABLE_BASE, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SG2042: SPI variable store unavailable (%r), using RAM variables\n", Status));
    return RamVariables ();
  }
  return EFI_SUCCESS;
}

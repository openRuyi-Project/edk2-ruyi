/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/Eic7700BootConfig.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/MemoryConfigLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct {
  UINT32 Magic;
  UINT32 Version;
  UINT32 Generation;
  UINT32 Flags;
  UINT32 Crc;
} BOOT_CONFIG;
STATIC_ASSERT (sizeof (BOOT_CONFIG) == EIC7700_BOOT_CONFIG_BYTES, "Boot policy layout");

STATIC BOOLEAN
Valid (CONST BOOT_CONFIG *Record)
{
  UINT32 Crc;
  return Record->Magic == EIC7700_BOOT_CONFIG_MAGIC &&
         Record->Version == EIC7700_BOOT_CONFIG_VERSION &&
         !(Record->Flags & ~EIC7700_BOOT_CONFIG_ECC) &&
         !EFI_ERROR (gBS->CalculateCrc32 ((VOID *)Record, OFFSET_OF (BOOT_CONFIG, Crc), &Crc)) &&
         Crc == Record->Crc;
}

STATIC EFI_STATUS
CheckDispatcher (VOID)
{
  UINT8 Table[80], Header[36];
  UINT64 Offset, Bytes;
  EFI_STATUS Status = Eic7700SpiNorRead (0, sizeof (Table), Table);
  if (EFI_ERROR (Status)) { return Status; }
  if (ReadUnaligned32 ((UINT32 *)Table) != 0x42575345 ||
      ReadUnaligned32 ((UINT32 *)(Table + 4)) != 3 || Table[66] != 0x10) {
    return EFI_UNSUPPORTED;
  }
  Offset = ReadUnaligned64 ((UINT64 *)(Table + 48));
  Bytes = ReadUnaligned64 ((UINT64 *)(Table + 56));
  if (Offset > SIZE_8MB - 256 - sizeof (Header) || Bytes < sizeof (Header)) {
    return EFI_UNSUPPORTED;
  }
  Status = Eic7700SpiNorRead ((UINT32)Offset + 256, sizeof (Header), Header);
  if (EFI_ERROR (Status)) { return Status; }
  if (CompareMem (Header + 4, "RUYIDDR", 8) ||
      ReadUnaligned32 ((UINT32 *)(Header + 12)) != 1) { return EFI_UNSUPPORTED; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Read (BOOT_CONFIG Records[EIC7700_BOOT_CONFIG_SLOT_COUNT], UINTN *Selected)
{
  EFI_STATUS Status = Eic7700SpiNorInit (FALSE);
  UINTN Slot;
  *Selected = EIC7700_BOOT_CONFIG_SLOT_COUNT;
  if (EFI_ERROR (Status)) { return Status; }
  Status = CheckDispatcher ();
  if (EFI_ERROR (Status)) { return Status; }
  for (Slot = 0; Slot < EIC7700_BOOT_CONFIG_SLOT_COUNT; Slot++) {
    Status = Eic7700SpiNorRead (EIC7700_BOOT_CONFIG_OFFSET + Slot * EIC7700_BOOT_CONFIG_SLOT_SIZE,
                               sizeof (*Records), &Records[Slot]);
    if (EFI_ERROR (Status)) { return Status; }
    if (Valid (&Records[Slot]) &&
        (*Selected == EIC7700_BOOT_CONFIG_SLOT_COUNT ||
         (INT32)(Records[Slot].Generation - Records[*Selected].Generation) > 0)) {
      *Selected = Slot;
    }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
MemoryConfigGet (BOOLEAN *EccEnabled)
{
  BOOT_CONFIG Records[EIC7700_BOOT_CONFIG_SLOT_COUNT];
  UINTN Selected;
  EFI_STATUS Status;
  if (EccEnabled == NULL) { return EFI_INVALID_PARAMETER; }
  *EccEnabled = FALSE;
  Status = Read (Records, &Selected);
  if (!EFI_ERROR (Status) && Selected < EIC7700_BOOT_CONFIG_SLOT_COUNT) {
    *EccEnabled = (Records[Selected].Flags & EIC7700_BOOT_CONFIG_ECC) != 0;
  }
  return Status;
}

EFI_STATUS
MemoryConfigSet (BOOLEAN EccEnabled)
{
  BOOT_CONFIG Records[EIC7700_BOOT_CONFIG_SLOT_COUNT], Next, Verify;
  UINTN Selected, Target;
  UINT32 Offset;
  EFI_TPL Old;
  EFI_STATUS Status;
  if (EccEnabled > TRUE) { return EFI_INVALID_PARAMETER; }
  Old = gBS->RaiseTPL (TPL_NOTIFY);
  Status = Read (Records, &Selected);
  if (EFI_ERROR (Status)) { goto Done; }
  if ((Selected < EIC7700_BOOT_CONFIG_SLOT_COUNT &&
       EccEnabled == !!(Records[Selected].Flags & EIC7700_BOOT_CONFIG_ECC)) ||
      (Selected == EIC7700_BOOT_CONFIG_SLOT_COUNT && !EccEnabled)) { goto Done; }
  Target = Selected < EIC7700_BOOT_CONFIG_SLOT_COUNT ? Selected ^ 1 : 0;
  Next = (BOOT_CONFIG) {
    EIC7700_BOOT_CONFIG_MAGIC, EIC7700_BOOT_CONFIG_VERSION,
    Selected < EIC7700_BOOT_CONFIG_SLOT_COUNT ? Records[Selected].Generation + 1 : 0,
    EccEnabled ? EIC7700_BOOT_CONFIG_ECC : 0, 0
  };
  Status = gBS->CalculateCrc32 (&Next, OFFSET_OF (BOOT_CONFIG, Crc), &Next.Crc);
  if (EFI_ERROR (Status)) { goto Done; }
  Offset = EIC7700_BOOT_CONFIG_OFFSET + Target * EIC7700_BOOT_CONFIG_SLOT_SIZE;
  // Erase only the inactive slot. An interrupted write leaves the old slot valid.
  Status = Eic7700SpiNorErase (Offset, EIC7700_BOOT_CONFIG_SLOT_SIZE);
  if (!EFI_ERROR (Status)) { Status = Eic7700SpiNorWrite (Offset, sizeof (Next), &Next); }
  if (!EFI_ERROR (Status)) { Status = Eic7700SpiNorRead (Offset, sizeof (Verify), &Verify); }
  if (!EFI_ERROR (Status) && CompareMem (&Next, &Verify, sizeof (Next))) { Status = EFI_DEVICE_ERROR; }
Done:
  gBS->RestoreTPL (Old);
  return Status;
}

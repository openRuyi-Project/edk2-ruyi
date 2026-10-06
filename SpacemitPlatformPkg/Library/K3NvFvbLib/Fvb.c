/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFormat.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3NvFvbLib.h>

#define SIGNATURE   SIGNATURE_32 ('K', '3', 'N', 'V')
#define ATTRIBUTES  (EFI_FVB2_READ_ENABLED_CAP | EFI_FVB2_READ_STATUS |\
                    EFI_FVB2_WRITE_ENABLED_CAP | EFI_FVB2_WRITE_DISABLED_CAP | EFI_FVB2_WRITE_STATUS | \
                    EFI_FVB2_MEMORY_MAPPED | EFI_FVB2_STICKY_WRITE | \
                    EFI_FVB2_ERASE_POLARITY)
#define BLOCKS      (K3_NV_TOTAL_SIZE / K3_NV_BLOCK_SIZE)

typedef struct {
  EFI_FIRMWARE_VOLUME_HEADER    Header;
  EFI_FV_BLOCK_MAP_ENTRY        Terminator;
  VARIABLE_STORE_HEADER         Variables;
} INITIAL_HEADER;

STATIC K3_NV_FVB *
Instance (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol
  )
{
  if (!Protocol) {
    return NULL;
  }

  K3_NV_FVB  *F = BASE_CR (Protocol, K3_NV_FVB, Protocol);

  return F->Signature == SIGNATURE ? F : NULL;
}

STATIC EFI_STATUS EFIAPI
Attributes (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  EFI_FVB_ATTRIBUTES_2                       *Value
  )
{
  K3_NV_FVB  *F = Instance (Protocol);

  if (!F || !Value) {
    return EFI_INVALID_PARAMETER;
  }

  *Value = F->Attributes;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
SetAttributes (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  EFI_FVB_ATTRIBUTES_2                       *Value
  )
{
  K3_NV_FVB  *F = Instance (Protocol);

  if (!F || !Value) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*Value ^ F->Attributes) & ~EFI_FVB2_WRITE_STATUS) {
    return EFI_INVALID_PARAMETER;
  }

  if (F->WriteLocked && (*Value & EFI_FVB2_WRITE_STATUS)) {
    return EFI_WRITE_PROTECTED;
  }

  if (F->Busy) {
    return EFI_NOT_READY;
  }

  F->Attributes = *Value;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Physical (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  EFI_PHYSICAL_ADDRESS                       *Address
  )
{
  K3_NV_FVB  *F = Instance (Protocol);

  if (!F || !Address) {
    return EFI_INVALID_PARAMETER;
  }

  *Address = F->Physical;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
BlockSize (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  EFI_LBA                                    Lba,
  UINTN                                      *Size,
  UINTN                                      *Count
  )
{
  if (!Instance (Protocol) || !Size || !Count || (Lba >= BLOCKS)) {
    return EFI_INVALID_PARAMETER;
  }

  *Size  = K3_NV_BLOCK_SIZE;
  *Count = BLOCKS - (UINTN)Lba;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Range (
  K3_NV_FVB  *F,
  EFI_LBA    Lba,
  UINTN      Offset,
  UINTN      *Bytes,
  VOID       *Buffer
  )
{
  if (!F || !Bytes || !Buffer || (Lba >= BLOCKS) ||
      (Offset >= K3_NV_BLOCK_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (F->Faulted) {
    return EFI_DEVICE_ERROR;
  }

  if (F->Busy) {
    return EFI_NOT_READY;
  }

  if (*Bytes > K3_NV_BLOCK_SIZE - Offset) {
    *Bytes = K3_NV_BLOCK_SIZE - Offset;
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Read (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  EFI_LBA                                    Lba,
  UINTN                                      Offset,
  UINTN                                      *Bytes,
  UINT8                                      *Buffer
  )
{
  K3_NV_FVB   *F     = Instance (Protocol);
  EFI_STATUS  Status = Range (F, Lba, Offset, Bytes, Buffer);

  if (EFI_ERROR (Status) && (Status != EFI_BAD_BUFFER_SIZE)) {
    return Status;
  }

  CopyMem (Buffer, F->Mirror + Lba * K3_NV_BLOCK_SIZE + Offset, *Bytes);
  return Status;
}

STATIC EFI_STATUS
Refresh (
  K3_NV_FVB  *F,
  UINT32     Offset,
  UINTN      Bytes
  )
{
  EFI_STATUS  Status = F->Flash.Read (
                                  F->Flash.Context,
                                  K3_NV_FLASH_OFFSET + Offset,
                                  Bytes,
                                  F->Scratch
                                  );

  if (EFI_ERROR (Status)) {
    F->Faulted = TRUE;
    return Status;
  }

  CopyMem (F->Mirror + Offset, F->Scratch, Bytes);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Write (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  EFI_LBA                                    Lba,
  UINTN                                      Offset,
  UINTN                                      *Bytes,
  UINT8                                      *Buffer
  )
{
  K3_NV_FVB   *F       = Instance (Protocol);
  EFI_STATUS  Boundary = Range (F, Lba, Offset, Bytes, Buffer);

  if (EFI_ERROR (Boundary) && (Boundary != EFI_BAD_BUFFER_SIZE)) {
    return Boundary;
  }

  UINT32  Position = (UINT32)(Lba * K3_NV_BLOCK_SIZE + Offset);
  UINTN   Size     = *Bytes;

  *Bytes = 0;
  if (!(F->Attributes & EFI_FVB2_WRITE_STATUS)) {
    return EFI_WRITE_PROTECTED;
  }

  for (UINTN I = 0; I < Size; I++) {
    if ((Buffer[I] & F->Mirror[Position + I]) != Buffer[I]) {
      return EFI_WRITE_PROTECTED;
    }
  }

  if (!Size) {
    return Boundary;
  }

  /* Input may point into the mirror. Keep an independent expected value
     while refreshing actual hardware contents after programming. */
  CopyMem (F->Expected, Buffer, Size);
  F->Busy = TRUE;
  EFI_STATUS  Status = F->Flash.Program (
                                  F->Flash.Context,
                                  K3_NV_FLASH_OFFSET + Position,
                                  Size,
                                  F->Expected
                                  );
  EFI_STATUS  Verify = Refresh (F, Position, Size);

  F->Busy = FALSE;
  if (EFI_ERROR (Status) || EFI_ERROR (Verify)) {
    return EFI_ERROR (Status) ? Status : Verify;
  }

  if (CompareMem (F->Mirror + Position, F->Expected, Size)) {
    return EFI_DEVICE_ERROR;
  }

  *Bytes = Size;
  return Boundary;
}

STATIC EFI_STATUS EFIAPI
Erase (
  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Protocol,
  ...
  )
{
  K3_NV_FVB  *F = Instance (Protocol);
  VA_LIST    Args;

  if (!F) {
    return EFI_INVALID_PARAMETER;
  }

  if (F->Faulted || F->Busy) {
    return F->Faulted ? EFI_DEVICE_ERROR : EFI_NOT_READY;
  }

  if (!(F->Attributes & EFI_FVB2_WRITE_STATUS)) {
    return EFI_WRITE_PROTECTED;
  }

  /* PI requires validating the entire tuple list before the first erase. */
  VA_START (Args, Protocol);
  while (TRUE) {
    EFI_LBA  Lba = VA_ARG (Args, EFI_LBA);

    if (Lba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    UINTN  Count = VA_ARG (Args, UINTN);

    if (!Count || (Lba >= BLOCKS) || (Count > BLOCKS - Lba)) {
      VA_END (Args);
      return EFI_INVALID_PARAMETER;
    }
  }

  VA_END (Args);
  F->Busy = TRUE;
  EFI_STATUS  Status = EFI_SUCCESS;

  VA_START (Args, Protocol);
  while (TRUE) {
    EFI_LBA  Lba = VA_ARG (Args, EFI_LBA);

    if (Lba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    UINTN  Count = VA_ARG (Args, UINTN);

    for (UINTN I = 0; I < Count; I++) {
      UINT32  Position = (UINT32)((Lba + I) * K3_NV_BLOCK_SIZE);

      Status = F->Flash.Erase (
                          F->Flash.Context,
                          K3_NV_FLASH_OFFSET + Position
                          );
      EFI_STATUS  Verify = Refresh (F, Position, K3_NV_BLOCK_SIZE);

      if (EFI_ERROR (Status) || EFI_ERROR (Verify)) {
        Status = EFI_ERROR (Status) ? Status : Verify;
        goto Done;
      }

      for (UINTN Byte = 0; Byte < K3_NV_BLOCK_SIZE; Byte++) {
        if (F->Scratch[Byte] != 0xff) {
          Status = EFI_DEVICE_ERROR;
          goto Done;
        }
      }
    }
  }

Done:
  VA_END (Args);
  F->Busy = FALSE;
  return Status;
}

STATIC BOOLEAN
ValidHeader (
  CONST UINT8  *Mirror
  )
{
  CONST EFI_FIRMWARE_VOLUME_HEADER  *H = (CONST VOID *)Mirror;
  CONST VARIABLE_STORE_HEADER       *V;

  if ((H->Signature != EFI_FVH_SIGNATURE) ||
      (H->Revision != EFI_FVH_REVISION) || (H->FvLength != K3_NV_TOTAL_SIZE) ||
      (H->HeaderLength != OFFSET_OF (INITIAL_HEADER, Variables)) ||
      !CompareGuid (&H->FileSystemGuid, &gEfiSystemNvDataFvGuid) ||
      ((H->Attributes != ATTRIBUTES) &&
       (H->Attributes != (ATTRIBUTES & ~EFI_FVB2_WRITE_DISABLED_CAP))) ||
      (H->BlockMap[0].NumBlocks != BLOCKS) ||
      (H->BlockMap[0].Length != K3_NV_BLOCK_SIZE) ||
      H->BlockMap[1].NumBlocks || H->BlockMap[1].Length ||
      CalculateSum16 ((CONST UINT16 *)Mirror, H->HeaderLength))
  {
    return FALSE;
  }

  V = (CONST VOID *)(Mirror + H->HeaderLength);
  return CompareGuid (&V->Signature, &gEfiAuthenticatedVariableGuid) &&
         V->Size == K3_NV_VARIABLE_SIZE - H->HeaderLength &&
         V->Format == VARIABLE_STORE_FORMATTED &&
         V->State == VARIABLE_STORE_HEALTHY;
}

EFI_STATUS
K3NvFvbInitialize (
  K3_NV_FVB              *F,
  VOID                   *Mirror,
  CONST K3_NV_FLASH_OPS  *Flash
  )
{
  if (!F || !Mirror || !Flash || !Flash->Read || !Flash->Program ||
      !Flash->Erase || ((UINTN)Mirror & EFI_PAGE_MASK))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (F, sizeof (*F));
  F->Signature  = SIGNATURE;
  F->Mirror     = Mirror;
  F->Physical   = (EFI_PHYSICAL_ADDRESS)(UINTN)Mirror;
  F->Flash      = *Flash;
  F->Attributes = ATTRIBUTES;
  F->Protocol   = (EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL) {
    Attributes, SetAttributes, Physical, BlockSize, Read, Write, Erase, NULL
  };
  EFI_STATUS  Status = Flash->Read (
                                Flash->Context,
                                K3_NV_FLASH_OFFSET,
                                K3_NV_TOTAL_SIZE,
                                Mirror
                                );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ValidHeader (Mirror)) {
    return EFI_SUCCESS;
  }

  for (UINTN I = 0; I < K3_NV_TOTAL_SIZE; I++) {
    if (((UINT8 *)Mirror)[I] != 0xff) {
      return EFI_VOLUME_CORRUPTED;
    }
  }

  INITIAL_HEADER  Initial;

  ZeroMem (&Initial, sizeof (Initial));
  CopyGuid (&Initial.Header.FileSystemGuid, &gEfiSystemNvDataFvGuid);
  Initial.Header.FvLength              = K3_NV_TOTAL_SIZE;
  Initial.Header.Signature             = EFI_FVH_SIGNATURE;
  Initial.Header.Attributes            = ATTRIBUTES;
  Initial.Header.HeaderLength          = OFFSET_OF (INITIAL_HEADER, Variables);
  Initial.Header.Revision              = EFI_FVH_REVISION;
  Initial.Header.BlockMap[0].NumBlocks = BLOCKS;
  Initial.Header.BlockMap[0].Length    = K3_NV_BLOCK_SIZE;
  Initial.Header.Checksum              = CalculateCheckSum16 (
                                           (UINT16 *)&Initial,
                                           Initial.Header.HeaderLength
                                           );
  CopyGuid (&Initial.Variables.Signature, &gEfiAuthenticatedVariableGuid);
  Initial.Variables.Size   = K3_NV_VARIABLE_SIZE - Initial.Header.HeaderLength;
  Initial.Variables.Format = VARIABLE_STORE_FORMATTED;
  Initial.Variables.State  = VARIABLE_STORE_HEALTHY;
  UINTN  Bytes = OFFSET_OF (INITIAL_HEADER, Variables) +
                 sizeof (Initial.Variables);

  Status = Write (&F->Protocol, 0, 0, &Bytes, (UINT8 *)&Initial);
  if (!EFI_ERROR (Status) && !ValidHeader (Mirror)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

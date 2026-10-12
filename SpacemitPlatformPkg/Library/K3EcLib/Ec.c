/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/K3EcLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/TimerLib.h>

#define EC_WINDOW         0x84000000
#define EC_PACKET         (EC_WINDOW + 0x100)
#define EC_ESPI           0xcac8c000
#define EC_CLOCK          0xd4282a40
#define EC_PLL2           0xd409011c
#define EC_GET_VERSION    0x02
#define EC_FLASH_INFO     0x10
#define EC_FLASH_READ     0x11
#define EC_FLASH_WRITE    0x12
#define EC_FLASH_ERASE    0x13
#define EC_REBOOT         0xd2
#define EC_TRANSFER_SIZE  64
#define EC_ERASE_SIZE     SIZE_4KB
#define EC_IMAGE_RO       1

// Version-0 EC host-command response layouts.
typedef struct {
  CHAR8     VersionRo[K3_EC_VERSION_SIZE];
  CHAR8     VersionRw[K3_EC_VERSION_SIZE];
  CHAR8     Reserved[K3_EC_VERSION_SIZE];
  UINT32    CurrentImage;
} EC_VERSION_RESPONSE;

typedef struct {
  UINT32    FlashSize;
  UINT32    WriteBlockSize;
  UINT32    EraseBlockSize;
  UINT32    ProtectBlockSize;
} EC_FLASH_INFO_RESPONSE;

STATIC_ASSERT (sizeof (EC_VERSION_RESPONSE) == 100, "EC version response size");
STATIC_ASSERT (sizeof (EC_FLASH_INFO_RESPONSE) == 16, "EC flash response size");

STATIC BOOLEAN  mMapped;

STATIC UINT32
Le32 (
  CONST UINT8  *Data
  )
{
  return Data[0] | (UINT32)Data[1] << 8 | (UINT32)Data[2] << 16 | (UINT32)Data[3] << 24;
}

STATIC EFI_STATUS
Prepare (
  VOID
  )
{
  EFI_STATUS  Status;

  if (!mMapped) {
    CONST UINTN  Pages[] = { 0xd4282000, 0xd4090000, EC_ESPI, EC_WINDOW };
    for (UINTN I = 0; I < ARRAY_SIZE (Pages); I++) {
      Status = K3MapMmio (Pages[I], SIZE_4KB);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    mMapped = TRUE;
  }

  // Coreboot owns eSPI initialization. Never reset the EC to probe it.
  if (((MmioRead32 (EC_CLOCK) & 0xff) != 0x8f) ||
      ((MmioRead32 (EC_PLL2) & (BIT16 | BIT4)) != (BIT16 | BIT4)))
  {
    return EFI_NOT_READY;
  }

  if ((MmioRead32 (EC_ESPI + 0x38) != EC_WINDOW) ||
      !(MmioRead32 (EC_ESPI + 0x34) & BIT5))
  {
    return EFI_NOT_READY;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
WaitReady (
  VOID
  )
{
  for (UINTN Us = 0; Us < 1000000; Us += 10) {
    UINT8  Status = MmioRead8 (EC_WINDOW);
    if (Status == 0xff) {
      return EFI_NO_RESPONSE;
    }

    if (!(Status & (BIT1 | BIT2))) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Command (
  UINT16      Code,
  CONST VOID  *Params,
  UINTN       ParamsSize,
  VOID        *Response,
  UINTN       ResponseSize
  )
{
  UINT8       Request[268] = { 3, 0, (UINT8)Code, (UINT8)(Code >> 8), 0, 0 };
  UINT8       Reply[268], Sum = 0;
  EFI_STATUS  Status;
  UINTN       Length;

  if ((ParamsSize > sizeof (Request) - 8) || (ResponseSize > sizeof (Reply) - 8)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = Prepare ();
  if (!EFI_ERROR (Status)) {
    Status = WaitReady ();
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Request[6] = (UINT8)ParamsSize;
  Request[7] = (UINT8)(ParamsSize >> 8);
  if (ParamsSize) {
    CopyMem (Request + 8, Params, ParamsSize);
  }

  for (UINTN I = 0; I < 8 + ParamsSize; I++) {
    Sum += Request[I];
  }

  Request[1] = (UINT8)-Sum;
  for (UINTN I = 0; I < 8 + ParamsSize; I++) {
    MmioWrite8 (EC_PACKET + I, Request[I]);
  }

  MemoryFence ();
  MmioWrite8 (EC_WINDOW, 0xda);
  Status = WaitReady ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MemoryFence ();
  for (UINTN I = 0; I < 8; I++) {
    Reply[I] = MmioRead8 (EC_PACKET + I);
  }

  Length = Reply[4] | (UINTN)Reply[5] << 8;
  if ((Reply[0] != 3) || Reply[6] || Reply[7] || (Length > sizeof (Reply) - 8)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 8; I < 8 + Length; I++) {
    Reply[I] = MmioRead8 (EC_PACKET + I);
  }

  Sum = 0;
  for (UINTN I = 0; I < 8 + Length; I++) {
    Sum += Reply[I];
  }

  if (Sum) {
    return EFI_CRC_ERROR;
  }

  if (Reply[2] || Reply[3]) {
    return EFI_DEVICE_ERROR;
  }

  if (Length != ResponseSize) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (Length) {
    CopyMem (Response, Reply + 8, Length);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3EcGetVersion (
  CHAR8  Version[K3_EC_VERSION_SIZE]
  )
{
  EC_VERSION_RESPONSE  Response;
  EFI_STATUS           Status;

  if (Version == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Command (EC_GET_VERSION, NULL, 0, &Response, sizeof (Response));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (
    Version,
    Response.CurrentImage == EC_IMAGE_RO ? Response.VersionRo : Response.VersionRw,
    K3_EC_VERSION_SIZE
    );
  Version[K3_EC_VERSION_SIZE - 1] = '\0';
  return Version[0] ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC EFI_STATUS
ReadBlock (
  UINT32  Offset,
  UINT32  Size,
  VOID    *Data
  )
{
  CONST UINT32  Params[] = { Offset, Size };

  return Command (EC_FLASH_READ, Params, sizeof (Params), Data, Size);
}

EFI_STATUS
K3EcFlashRead (
  UINT32                     Offset,
  UINTN                      Size,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  )
{
  UINT8       *Next  = Data;
  EFI_STATUS  Status = EFI_SUCCESS;
  UINTN       Done   = 0;

  if (((Data == NULL) && Size) || (Offset > K3_EC_UPDATE_SIZE) ||
      (Size > K3_EC_UPDATE_SIZE - Offset))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Progress) {
    Status = Progress (0, Size, FastbootStorageRead);
  }

  while (!EFI_ERROR (Status) && (Done < Size)) {
    UINTN  Count = MIN (Size - Done, EC_TRANSFER_SIZE);
    Status = ReadBlock (Offset + Done, Count, Next + Done);
    Done  += Count;
    if (!EFI_ERROR (Status) && Progress) {
      Status = Progress (Done, Size, FastbootStorageRead);
    }
  }

  return Status;
}

STATIC BOOLEAN
ImageValid (
  CONST UINT8  *Image,
  UINTN        Size
  )
{
  STATIC CONST UINT8  Magic[] = { 0xe1, 0, 'i', 'm', 'a', 'g', 'e', 1 };
  STATIC CONST CHAR8  Board[] = "SPACEMIT_PICO_ITX";

  if ((Image == NULL) || (Size < 40) || (Size > K3_EC_UPDATE_SIZE) || (Size % 4) ||
      CompareMem (Image, Magic, sizeof (Magic)) || (Le32 (Image + 12) != Size - 32))
  {
    return FALSE;
  }

  for (UINTN I = 32; I <= Size - sizeof (Board); I++) {
    if (!CompareMem (Image + I, Board, sizeof (Board))) {
      return TRUE;
    }
  }

  return FALSE;
}

EFI_STATUS
K3EcUpdate (
  CONST VOID                 *Image,
  UINTN                      Size,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  )
{
  CONST UINT8             *Data = Image;
  CHAR8                   Version[K3_EC_VERSION_SIZE];
  EC_FLASH_INFO_RESPONSE  Info;

  struct {
    UINT32    Offset, Size;
    UINT8     Data[EC_TRANSFER_SIZE];
  } Write;
  UINT8       Verify[EC_TRANSFER_SIZE];
  EFI_STATUS  Status;

  if (!ImageValid (Data, Size)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3EcGetVersion (Version);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (AsciiStrnCmp (Version, "SPACEMIT_PICO_ITX-", sizeof ("SPACEMIT_PICO_ITX-") - 1) &&
      AsciiStrnCmp (Version, "SPACEMIT_DEB1-", sizeof ("SPACEMIT_DEB1-") - 1))
  {
    return EFI_UNSUPPORTED;
  }

  Status = Command (EC_FLASH_INFO, NULL, 0, &Info, sizeof (Info));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Info.FlashSize < K3_EC_UPDATE_SIZE) || !Info.WriteBlockSize ||
      (EC_TRANSFER_SIZE % Info.WriteBlockSize) || (Size % Info.WriteBlockSize) ||
      !Info.EraseBlockSize || (EC_ERASE_SIZE % Info.EraseBlockSize))
  {
    return EFI_UNSUPPORTED;
  }

  Status = Progress ? Progress (0, K3_EC_UPDATE_SIZE, FastbootStorageErase) : EFI_SUCCESS;
  for (UINT32 Offset = 0; !EFI_ERROR (Status) && Offset < K3_EC_UPDATE_SIZE; Offset += EC_ERASE_SIZE) {
    CONST UINT32  Erase[] = { Offset, EC_ERASE_SIZE };
    Status = Command (EC_FLASH_ERASE, Erase, sizeof (Erase), NULL, 0);
    if (!EFI_ERROR (Status) && Progress) {
      Status = Progress (Offset + EC_ERASE_SIZE, K3_EC_UPDATE_SIZE, FastbootStorageErase);
    }
  }

  if (!EFI_ERROR (Status) && Progress) {
    Status = Progress (0, Size, FastbootStorageWrite);
  }

  for (UINTN Offset = 0; !EFI_ERROR (Status) && Offset < Size;) {
    UINTN  Count = MIN (Size - Offset, sizeof (Write.Data));
    Write.Offset = Offset;
    Write.Size   = Count;
    CopyMem (Write.Data, Data + Offset, Count);
    Status  = Command (EC_FLASH_WRITE, &Write, 8 + Count, NULL, 0);
    Offset += Count;
    if (!EFI_ERROR (Status) && Progress) {
      Status = Progress (Offset, Size, FastbootStorageWrite);
    }
  }

  if (!EFI_ERROR (Status) && Progress) {
    Status = Progress (0, Size, FastbootStorageVerify);
  }

  for (UINTN Offset = 0; !EFI_ERROR (Status) && Offset < Size;) {
    UINTN  Count = MIN (Size - Offset, sizeof (Verify));
    Status = ReadBlock (Offset, Count, Verify);
    if (!EFI_ERROR (Status) && CompareMem (Data + Offset, Verify, Count)) {
      Status = EFI_CRC_ERROR;
    }

    Offset += Count;
    if (!EFI_ERROR (Status) && Progress) {
      Status = Progress (Offset, Size, FastbootStorageVerify);
    }
  }

  return Status;
}

EFI_STATUS
K3EcReboot (
  VOID
  )
{
  CONST UINT8  Params[] = { 2, 0 }; // EC_REBOOT_JUMP_RW, no flags.

  return Command (EC_REBOOT, Params, sizeof (Params), NULL, 0);
}

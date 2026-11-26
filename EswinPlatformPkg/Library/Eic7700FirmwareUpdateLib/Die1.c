/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseRiscVSbiLib.h>
#include <Library/Eic7700DmaLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/TimerLib.h>
#include "Internal.h"
#include "Die1Io.h"

#define WORKER_OFFSET  SIZE_2KB
#define DIE0_LOW_END   0xbfffffffU
#define HSM_START      0
#define HSM_STATUS     2
#define HSM_STOPPED    1
#define IO_TIMEOUT_NS  1000000000ULL

typedef struct {
  volatile UINT32 Request, Complete, Ready, Stop;
  UINT32 Region, Offset, Count, Write;
  UINT32 Words[64];
} DIE1_CHANNEL;

STATIC_ASSERT (OFFSET_OF (DIE1_CHANNEL, Words) == D560_BUFFER, "worker ABI");
STATIC_ASSERT (OFFSET_OF (DIE1_CHANNEL, Write) == D560_WRITE, "worker ABI");
STATIC_ASSERT (sizeof (DIE1_CHANNEL) <= WORKER_OFFSET, "workspace overlap");
extern CONST UINT8 D560WorkerStart[], D560WorkerEnd[];

STATIC BOOLEAN
Expired (UINT64 Start)
{
  return GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= IO_TIMEOUT_NS;
}

STATIC EFI_STATUS
Transfer (VOID *Context, BOOLEAN Write, UINTN Address, UINT32 *Words, UINTN Count)
{
  D560_REMOTE *Remote = Context;
  DIE1_CHANNEL *Channel = Remote->Channel;
  UINT32 Region;
  UINTN Offset;

  if (Channel->Ready != 1 || Channel->Stop != 0) {
    return EFI_DEVICE_ERROR;
  }
  if (Address >= D560_SPI1_DATA && Address - D560_SPI1_DATA < sizeof (Channel->Words)) {
    Region = D560_WINDOW;
    Offset = Address - D560_SPI1_DATA;
    if (!Count || Count > (sizeof (Channel->Words) - Offset) / sizeof (UINT32)) {
      return EFI_INVALID_PARAMETER;
    }
  } else if (Address >= D560_SPI1_CRG && Address - D560_SPI1_CRG <= 0x490) {
    Region = D560_CLOCKS;
    Offset = Address - D560_SPI1_CRG;
    if (Count != 1 || (Offset != 0x104 && Offset != 0x108 && Offset != 0x1e4 && Offset != 0x490)) {
      return EFI_INVALID_PARAMETER;
    }
  } else if (Address >= D560_SPI1 && Address - D560_SPI1 <= 0x34 && Count == 1) {
    Region = D560_REGISTERS;
    Offset = Address - D560_SPI1;
  } else {
    return EFI_INVALID_PARAMETER;
  }
  if ((Offset % sizeof (UINT32)) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  Channel->Region = Region;
  Channel->Offset = (UINT32)Offset;
  Channel->Count = (UINT32)Count;
  Channel->Write = Write;
  if (Write) {
    CopyMem (Channel->Words, Words, Count * sizeof (*Words));
  }
  UINT32 Sequence = Channel->Request + 1;
  MemoryFence ();
  Channel->Request = Sequence;
  UINT64 Start = GetPerformanceCounter ();
  while (Channel->Complete != Sequence) {
    if (Channel->Ready != 1 || Expired (Start)) {
      return Channel->Ready == 1 ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
    }
    CpuPause ();
  }
  MemoryFence ();
  if (!Write) {
    CopyMem (Words, Channel->Words, Count * sizeof (*Words));
  }
  return EFI_SUCCESS;
}

EFI_STATUS
D560StopRemote (D560_REMOTE *Remote)
{
  DIE1_CHANNEL *Channel = Remote->Channel;
  if (Remote->Started) {
    UINT64 Start = GetPerformanceCounter ();
    Channel->Stop = 1;
    MemoryFence ();
    for (;;) {
      SBI_RET Ret = SbiCall (D560_SBI_HSM, HSM_STATUS, 1, Remote->Hart);
      if (!Ret.Error && Ret.Value == HSM_STOPPED) {
        break;
      }
      if (Expired (Start)) {
        // A live hart can still reach its private code and mailbox. Both lie
        // in the same runtime allocation, so the OS will not reclaim them.
        return EFI_TIMEOUT;
      }
      MicroSecondDelay (10);
    }
    Remote->Started = FALSE;
  }
  if (Remote->Mapping != NULL) {
    EFI_STATUS Status = DmaUnmap (Remote->Mapping);
    if (EFI_ERROR (Status)) { return Status; }
    Remote->Mapping = NULL;
  }
  if (Channel != NULL) {
    EFI_STATUS Status = DmaFreeBuffer (1, Channel);
    if (EFI_ERROR (Status)) { return Status; }
  }
  ZeroMem (Remote, sizeof (*Remote));
  return EFI_SUCCESS;
}

EFI_STATUS
D560StartRemote (D560_REMOTE *Remote)
{
  PHYSICAL_ADDRESS Physical;
  UINTN Bytes = EFI_PAGE_SIZE;
  UINTN CodeBytes = (UINTN)D560WorkerEnd - (UINTN)D560WorkerStart;
  EFI_STATUS Status;
  SBI_RET Ret;

  ZeroMem (Remote, sizeof (*Remote));
  if (CodeBytes == 0 || CodeBytes > EFI_PAGE_SIZE - WORKER_OFFSET) {
    return EFI_LOAD_ERROR;
  }
  // EIC7702 harts 4..7 share the die 1 boot controller.
  for (Remote->Hart = 4; Remote->Hart <= 7; ++Remote->Hart) {
    Ret = SbiCall (D560_SBI_HSM, HSM_STATUS, 1, Remote->Hart);
    if (!Ret.Error && Ret.Value == HSM_STOPPED) {
      break;
    }
  }
  if (Remote->Hart > 7) {
    return EFI_NOT_READY;
  }

  Status = Eic7700DmaAllocateBuffer (EfiRuntimeServicesData, 1, &Remote->Channel, DIE0_LOW_END);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Eic7700DmaMap (MapOperationBusMasterCommonBuffer, Remote->Channel, &Bytes,
                          &Physical, &Remote->Mapping, DIE0_LOW_END);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  DIE1_CHANNEL *Channel = Remote->Channel;
  ZeroMem (Channel, EFI_PAGE_SIZE);
  CopyMem ((UINT8 *)Channel + WORKER_OFFSET, D560WorkerStart, CodeBytes);
  MemoryFence ();
  Ret = SbiCall (D560_SBI_HSM, HSM_START, 3, Remote->Hart, Physical + WORKER_OFFSET, (UINTN)Channel);
  if (Ret.Error) {
    Status = TranslateError (Ret.Error);
    goto Error;
  }
  Remote->Started = TRUE;
  UINT64 Start = GetPerformanceCounter ();
  while (Channel->Ready == 0 && !Expired (Start)) {
    CpuPause ();
  }
  MemoryFence ();
  if (Channel->Ready != 1) {
    Status = Channel->Ready == 0 ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
    goto Error;
  }
  Remote->Nor.Spi = D560_SPI1;
  Remote->Nor.Data = D560_SPI1_DATA;
  Remote->Nor.Clock = D560_SPI1_CRG;
  Remote->Nor.Transfer = Transfer;
  Remote->Nor.Context = Remote;
  Status = Eic7700SpiNorProbeDevice (&Remote->Nor);
  if (!EFI_ERROR (Status) && Remote->Nor.Size == SIZE_16MB) {
    return EFI_SUCCESS;
  }
  if (!EFI_ERROR (Status)) {
    Status = EFI_UNSUPPORTED;
  }
Error:
  {
    EFI_STATUS StopStatus = D560StopRemote (Remote);
    return EFI_ERROR (StopStatus) ? StopStatus : Status;
  }
}

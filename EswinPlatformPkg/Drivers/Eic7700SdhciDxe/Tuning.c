/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Host.h"
#include <IndustryStandard/Emmc.h>
#include <IndustryStandard/Sd.h>

// Standard SD and eMMC tuning block data.
STATIC CONST UINT8 mPattern4[] = {
  0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc,
  0xc3, 0x3c, 0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef,
  0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb,
  0xbf, 0xff, 0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef,
  0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c,
  0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee,
  0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff,
  0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde,
};

STATIC CONST UINT8 mPattern8[] = {
  0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00,
  0xff, 0xff, 0xcc, 0xcc, 0xcc, 0x33, 0xcc, 0xcc,
  0xcc, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xff, 0xff,
  0xff, 0xee, 0xff, 0xff, 0xff, 0xee, 0xee, 0xff,
  0xff, 0xff, 0xdd, 0xff, 0xff, 0xff, 0xdd, 0xdd,
  0xff, 0xff, 0xff, 0xbb, 0xff, 0xff, 0xff, 0xbb,
  0xbb, 0xff, 0xff, 0xff, 0x77, 0xff, 0xff, 0xff,
  0x77, 0x77, 0xff, 0x77, 0xbb, 0xdd, 0xee, 0xff,
  0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00,
  0x00, 0xff, 0xff, 0xcc, 0xcc, 0xcc, 0x33, 0xcc,
  0xcc, 0xcc, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xff,
  0xff, 0xff, 0xee, 0xff, 0xff, 0xff, 0xee, 0xee,
  0xff, 0xff, 0xff, 0xdd, 0xff, 0xff, 0xff, 0xdd,
  0xdd, 0xff, 0xff, 0xff, 0xbb, 0xff, 0xff, 0xff,
  0xbb, 0xbb, 0xff, 0xff, 0xff, 0x77, 0xff, 0xff,
  0xff, 0x77, 0x77, 0xff, 0x77, 0xbb, 0xdd, 0xee,
};

STATIC EFI_STATUS
Probe (EIC_MMC_PORT *Port, EFI_SD_MMC_PASS_THRU_PROTOCOL *Io, UINT8 Width)
{
  EFI_SD_MMC_COMMAND_BLOCK Command = {0};
  EFI_SD_MMC_STATUS_BLOCK Response = {0};
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet = {0};
  UINT8 Data[128];
  CONST UINT8 *Pattern = Width == 8 ? mPattern8 : mPattern4;
  UINTN Bytes = Width == 8 ? sizeof (mPattern8) : sizeof (mPattern4);
  UINTN Timeout;
  UINT16 Interrupt = 0;
  EFI_STATUS Status, ResetStatus;

  SetMem (Data, sizeof (Data), 0xa5);
  Command.CommandIndex = Port->Emmc ? EMMC_SEND_TUNING_BLOCK : SD_SEND_TUNING_BLOCK;
  Command.CommandType = SdMmcCommandTypeAdtc;
  Command.ResponseType = SdMmcResponseTypeR1;
  Packet.SdMmcCmdBlk = &Command;
  Packet.SdMmcStatusBlk = &Response;
  Packet.InDataBuffer = Data;
  Packet.InTransferLength = Bytes;
  Packet.Timeout = 10000;
  Status = Io->PassThru (Io, 0, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    // PIO completes when the FIFO is drained. Check the final data CRC too.
    for (Timeout = 0; Timeout < 10000; Timeout++) {
      Interrupt = MmioRead16 (Port->Base + HC_INT_STATUS);
      if (Interrupt & (HC_XFER_COMPLETE | HC_ERROR)) { break; }
      MicroSecondDelay (1);
    }
    if (Timeout == 10000) { Status = EFI_TIMEOUT; }
    else if (Interrupt & HC_ERROR) { Status = EFI_CRC_ERROR; }
    else if (CompareMem (Data, Pattern, Bytes)) { Status = EFI_CRC_ERROR; }
  }
  ResetStatus = ResetCommandData (Port);
  return EFI_ERROR (ResetStatus) ? ResetStatus : Status;
}

STATIC EFI_STATUS
Sweep (EIC_MMC_PORT *Port, EFI_SD_MMC_PASS_THRU_PROTOCOL *Io,
       UINT8 Width, BOOLEAN OutputDelay, UINT8 *Selected)
{
  UINTN Code, Limit = OutputDelay ? 127 : 255;
  UINTN RunStart = 0, RunLength = 0, BestStart = 0, BestLength = 0;
  EFI_STATUS Status;

  for (Code = 0; Code <= Limit; Code++) {
    if (OutputDelay) { SetDelay (Port, Code); }
    else { MmioWrite16 (Port->Base + Port->Vendor + DWC_AT_PHASE, Code); }
    Status = Probe (Port, Io, Width);
    if (!EFI_ERROR (Status)) {
      if (!RunLength) { RunStart = Code; }
      RunLength++;
      if (RunLength > BestLength) {
        BestStart = RunStart;
        BestLength = RunLength;
      }
    } else {
      RunLength = 0;
    }
  }
  if (BestLength < 2) { return EFI_DEVICE_ERROR; }
  *Selected = BestStart + (BestLength - 1) / 2;
  DEBUG ((DEBUG_INFO, "EIC7700 %a %a window %u..%u, select %u\n",
          Port->Emmc ? "eMMC" : "SD", OutputDelay ? "delay" : "phase",
          BestStart, BestStart + BestLength - 1, *Selected));
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
ExecuteTuning (EFI_HANDLE Handle, UINT8 Slot,
               EFI_SD_MMC_PASS_THRU_PROTOCOL *Io, BOOLEAN IsEmmc, UINT8 Width)
{
  EIC_MMC_PORT *Port = PortFor (Handle, Slot);
  EFI_STATUS Status;
  UINT8 Delay = 0, Phase = 0;

  if (!Port) { return EFI_UNSUPPORTED; }
  if (!Io || IsEmmc != Port->Emmc || (Width != 4 && Width != 8)) {
    return EFI_INVALID_PARAMETER;
  }
  DEBUG ((DEBUG_INFO, "EIC7700 %a software tuning starts\n", Port->Emmc ? "eMMC" : "SD"));
  MmioAnd16 (Port->Base + HC_HOST_CTRL2, (UINT16)~(HC_EXECUTE_TUNING | HC_TUNED_CLOCK));
  MmioAndThenOr32 (Port->Base + Port->Vendor + DWC_AT_CTRL, ~((UINT32)BIT0), DWC_SW_TUNING);
  MmioWrite16 (Port->Base + Port->Vendor + DWC_AT_PHASE, 0);
  if (Port->Emmc) {
    Status = Sweep (Port, Io, Width, TRUE, &Delay);
    if (EFI_ERROR (Status)) { goto Failed; }
    SetDelay (Port, Delay);
  }
  Status = Sweep (Port, Io, Width, FALSE, &Phase);
  if (EFI_ERROR (Status)) { goto Failed; }
  MmioWrite16 (Port->Base + Port->Vendor + DWC_AT_PHASE, Phase);
  Status = Probe (Port, Io, Width);
  if (EFI_ERROR (Status)) { goto Failed; }
  DEBUG ((DEBUG_INFO, "EIC7700 tuning verified\n"));
  return EFI_SUCCESS;
Failed:
  SetDelay (Port, Port->Delay);
  MmioWrite16 (Port->Base + Port->Vendor + DWC_AT_PHASE, 0);
  DEBUG ((DEBUG_ERROR, "EIC7700 tuning failed: %r\n", Status));
  return Status;
}

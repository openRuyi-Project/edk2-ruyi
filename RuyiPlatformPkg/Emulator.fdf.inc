## SPDX-License-Identifier: BSD-2-Clause-Patent
FILE DRIVER = e6727a5e-cbcd-44c8-b37f-78bc3a0c16c8 {
  SECTION DXE_DEPEX_EXP = {
    gRuyiPlatformSettingsProtocolGuid AND gEfiCpuArchProtocolGuid AND gEfiCpuIo2ProtocolGuid
  }
  SECTION PE32 = $(WORKSPACE)/X64EmulatorBin/EmulatorDxe.efi
}

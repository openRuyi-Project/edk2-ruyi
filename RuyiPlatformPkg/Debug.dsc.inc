## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
!if $(TARGET) == DEBUG
  DEFINE DEBUG_PRINT_ERROR_LEVEL = 0x80000047
!else
  DEFINE DEBUG_PRINT_ERROR_LEVEL = 0x80000002
!endif

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|$(DEBUG_PRINT_ERROR_LEVEL)
  # Status-code progress reporting is independent of DEBUG messages.
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x03
  gEfiMdeModulePkgTokenSpaceGuid.PcdStatusCodeUseSerial|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdStatusCodeUseMemory|FALSE

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/ShellDynamicCommand.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3BoardEepromLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/ShellLib.h>
#include <Library/SpacemitTlvLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC UINT8         mData[256];
STATIC BOOLEAN       mLoaded;
STATIC BOOLEAN       mDirty;
STATIC CONST CHAR16  mHelp[] =
  L"tlv_eeprom -r                    Read and display board TLVs\r\n"
  L"tlv_eeprom -d                    Dump staged 256 bytes\r\n"
  L"tlv_eeprom -s <code> <value>      Stage a named TLV value\r\n"
  L"tlv_eeprom -s <code>             Stage deletion of a TLV\r\n"
  L"tlv_eeprom -x <code> <hexbytes>   Stage opaque TLV bytes\r\n"
  L"tlv_eeprom -w                    Write staged changes and verify\r\n"
  L"tlv_eeprom -b <file>             Save physical 256-byte backup\r\n"
  L"tlv_eeprom -i <file>             Stage a valid 256-byte backup\r\n"
  L"Codes are hexadecimal. MAC count 0x2a is decimal.\r\n"
  L"0x24 is base MAC; 0x2a is a consecutive address count.\r\n"
  L"-s supports text fields, Ethernet/Wi-Fi/Bluetooth MAC and count.\r\n"
  L"Use -x for other SpacemiT fields. -r discards staged changes.\r\n";

STATIC INTN
HexDigit (
  CHAR16  C
  )
{
  if ((C >= L'0') && (C <= L'9')) {
    return C - L'0';
  }

  if ((C >= L'a') && (C <= L'f')) {
    return C - L'a' + 10;
  }

  if ((C >= L'A') && (C <= L'F')) {
    return C - L'A' + 10;
  }

  return -1;
}

STATIC EFI_STATUS
HexBytes (
  CONST CHAR16  *Text,
  UINT8         *Data,
  UINTN         *Length,
  BOOLEAN       Mac
  )
{
  UINTN  Count = 0;

  while (*Text) {
    INTN  Hi = HexDigit (Text[0]);
    if ((Hi < 0) || !Text[1]) {
      return EFI_INVALID_PARAMETER;
    }

    INTN  Lo = HexDigit (Text[1]);
    if ((Lo < 0) || (Count >= 255)) {
      return EFI_INVALID_PARAMETER;
    }

    Data[Count++] = (UINT8)((Hi << 4) | Lo);
    Text         += 2;
    if (*Text == L':') {
      Text++;
      if (!*Text) {
        return EFI_INVALID_PARAMETER;
      }
    } else if (Mac && *Text) {
      return EFI_INVALID_PARAMETER;
    }
  }

  if (!Count || (Mac && (Count != 6))) {
    return EFI_INVALID_PARAMETER;
  }

  if (Mac) {
    UINT8  Any = 0;
    for (UINTN I = 0; I < 6; I++) {
      Any |= Data[I];
    }

    if (!Any || (Data[0] & 1)) {
      return EFI_INVALID_PARAMETER;
    }
  }

  *Length = Count;
  return EFI_SUCCESS;
}

STATIC BOOLEAN
IsText (
  UINT8  Code
  )
{
  return (Code >= 0x21 && Code <= 0x23) || Code == 0x25 ||
         (Code >= 0x27 && Code <= 0x29) ||
         (Code >= 0x2b && Code <= 0x2f) ||
         Code == SPACEMIT_TLV_DDR_PART || Code == 0x80 || Code == 0x83;
}

STATIC CONST CHAR16 *
Name (
  UINT8  Code
  )
{
  switch (Code) {
    case 0x21:
      return L"Product name";
    case 0x22:
      return L"Part number";
    case 0x23:
      return L"Serial number";
    case 0x24:
      return L"Base MAC";
    case 0x2a:
      return L"MAC count";
    case 0x40:
      return L"SDK version";
    case 0x41:
      return L"DDR CS count";
    case 0x42:
      return L"DDR type";
    case 0x43:
      return L"DDR data rate";
    case 0x44:
      return L"DDR TX ODT";
    case 0x45:
      return L"DDR part number";
    case 0x60:
      return L"Wi-Fi MAC";
    case 0x61:
      return L"Bluetooth address";
    case 0x80:
      return L"PMIC type";
    case 0x81:
      return L"EEPROM I2C index";
    case 0x82:
      return L"EEPROM pin group";
    case 0x83:
      return L"Secondary boot device";
    case 0xfe:
      return L"CRC-32";
    default: return L"Opaque TLV";
  }
}

STATIC EFI_STATUS
Display (
  BOOLEAN  Raw
  )
{
  if (Raw) {
    for (UINTN Offset = 0; Offset < sizeof (mData); Offset += 16) {
      ShellPrintEx (-1, -1, L"%02x:", Offset);
      for (UINTN I = 0; I < 16; I++) {
        ShellPrintEx (-1, -1, L" %02x", mData[Offset + I]);
      }

      ShellPrintEx (-1, -1, L"\r\n");
    }

    return EFI_SUCCESS;
  }

  UINTN       Used;
  EFI_STATUS  Status = SpacemitTlvValidate (mData, sizeof (mData), &Used);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  ShellPrintEx (
    -1,
    -1,
    L"TlvInfo v1: %u bytes, CRC valid%s\r\n",
    Used,
    mDirty ? L", staged changes" : L""
    );
  for (UINTN Offset = 11; Offset < Used;
       Offset += 2 + mData[Offset + 1])
  {
    UINT8  Code   = mData[Offset];
    UINTN  Length = mData[Offset + 1];
    UINT8  *Value = mData + Offset + 2;
    ShellPrintEx (-1, -1, L"0x%02x %-22s %3u  ", Code, Name (Code), Length);
    if (IsText (Code)) {
      for (UINTN I = 0; I < Length; I++) {
        ShellPrintEx (
          -1,
          -1,
          L"%c",
          (Value[I] >= 0x20 && Value[I] < 0x7f) ? Value[I] : L'.'
          );
      }
    } else if (((Code == 0x2a) || (Code == 0x43)) && (Length == 2)) {
      ShellPrintEx (-1, -1, L"%u", ((UINTN)Value[0] << 8) | Value[1]);
    } else {
      for (UINTN I = 0; I < Length; I++) {
        ShellPrintEx (-1, -1, I ? L":%02x" : L"%02x", Value[I]);
      }
    }

    ShellPrintEx (-1, -1, L"\r\n");
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
File (
  CONST CHAR16  *Path,
  BOOLEAN       Import
  )
{
  SHELL_FILE_HANDLE  Handle = NULL;
  UINT8              Data[256];
  EFI_STATUS         Status;
  UINT64             Size;

  if (!Import) {
    Status = K3BoardEepromAccess (Data, FALSE, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!EFI_ERROR (ShellFileExists (Path))) {
      return EFI_ALREADY_STARTED;
    }
  }

  Status = ShellOpenFileByName (
             Path,
             &Handle,
             Import ? EFI_FILE_MODE_READ :
             EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
             0
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINTN  Bytes = sizeof (Data);

  if (Import) {
    Status = ShellGetFileSize (Handle, &Size);
    if (!EFI_ERROR (Status) && (Size != sizeof (Data))) {
      Status = EFI_BAD_BUFFER_SIZE;
    }

    if (!EFI_ERROR (Status)) {
      Status = ShellReadFile (Handle, &Bytes, Data);
    }
  } else {
    Status = ShellWriteFile (Handle, &Bytes, Data);
  }

  EFI_STATUS  Close = ShellCloseFile (&Handle);

  if (!EFI_ERROR (Status) && (Bytes != sizeof (Data))) {
    Status = EFI_BAD_BUFFER_SIZE;
  }

  if (!EFI_ERROR (Status)) {
    Status = Close;
  }

  if (!EFI_ERROR (Status) && Import) {
    UINTN  Used;
    Status = SpacemitTlvValidate (Data, sizeof (Data), &Used);
    if (!EFI_ERROR (Status)) {
      CopyMem (mData, Data, sizeof (Data));
      mLoaded = TRUE;
      mDirty  = TRUE;
    }
  }

  return Status;
}

STATIC EFI_STATUS
Run (
  UINTN   Argc,
  CHAR16  **Argv
  )
{
  if ((Argc == 2) && (!StrCmp (Argv[1], L"-h") ||
                      !StrCmp (Argv[1], L"--help")))
  {
    ShellPrintEx (-1, -1, L"%s", mHelp);
    return EFI_SUCCESS;
  }

  if ((Argc == 3) && (!StrCmp (Argv[1], L"-b") ||
                      !StrCmp (Argv[1], L"-i")))
  {
    return File (Argv[2], !StrCmp (Argv[1], L"-i"));
  }

  BOOLEAN  Read  = Argc == 1 || (Argc == 2 && !StrCmp (Argv[1], L"-r"));
  BOOLEAN  Raw   = Argc == 2 && !StrCmp (Argv[1], L"-d");
  BOOLEAN  Write = Argc == 2 && !StrCmp (Argv[1], L"-w");
  BOOLEAN  Set   = (Argc == 3 || Argc == 4) && !StrCmp (Argv[1], L"-s");
  BOOLEAN  Hex   = Argc == 4 && !StrCmp (Argv[1], L"-x");

  if (!Read && !Raw && !Write && !Set && !Hex) {
    return EFI_INVALID_PARAMETER;
  }

  if (Read || !mLoaded) {
    mLoaded = FALSE;
    mDirty  = FALSE;
    EFI_STATUS  Status = K3BoardEepromAccess (mData, FALSE, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    mLoaded = TRUE;
  }

  if (Read || Raw) {
    return Display (Raw);
  }

  UINTN       Used;
  EFI_STATUS  Status = SpacemitTlvValidate (mData, sizeof (mData), &Used);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Write) {
    if (!mDirty) {
      ShellPrintEx (-1, -1, L"No staged changes.\r\n");
      return EFI_SUCCESS;
    }

    Status = K3BoardEepromAccess (mData, TRUE, Used - 4);
    UINT8  Actual[256];
    if (!EFI_ERROR (Status)) {
      Status = K3BoardEepromAccess (Actual, FALSE, 0);
      if (!EFI_ERROR (Status) && CompareMem (Actual, mData, sizeof (mData))) {
        Status = EFI_DEVICE_ERROR;
      }
    }

    if (EFI_ERROR (Status)) {
      mLoaded = FALSE;
      mDirty  = FALSE;
    } else {
      mDirty = FALSE;
      ShellPrintEx (-1, -1, L"EEPROM write and 256-byte readback verified.\r\n");
    }

    return Status;
  }

  CHAR16  *End;
  UINTN   Code;

  Status = StrHexToUintnS (Argv[2], &End, &Code);
  if (EFI_ERROR (Status) || *End || (Code == 0) || (Code >= 0xfe)) {
    return EFI_INVALID_PARAMETER;
  }

  UINT8  Value[255];
  UINTN  Length = 0;

  if (Argc == 4) {
    if (Hex || (Code == 0x24) || (Code == 0x60) || (Code == 0x61)) {
      Status = HexBytes (Argv[3], Value, &Length, !Hex);
    } else if ((Code == 0x2a) || (Code == 0x43) || (Code == 0x26)) {
      UINTN  Count = 0;
      Status = StrDecimalToUintnS (Argv[3], &End, &Count);
      if (!EFI_ERROR (Status) && (*End || ((Code == 0x2a) && !Count) ||
                                  (Count > ((Code == 0x26) ? 255 : 65535))))
      {
        Status = EFI_INVALID_PARAMETER;
      }

      if (Code == 0x26) {
        Value[0] = (UINT8)Count;
        Length   = 1;
      } else {
        Value[0] = (UINT8)(Count >> 8);
        Value[1] = (UINT8)Count;
        Length   = 2;
      }
    } else if (IsText ((UINT8)Code)) {
      Length = StrLen (Argv[3]);
      if (Length > sizeof (Value)) {
        return EFI_BAD_BUFFER_SIZE;
      }

      for (UINTN I = 0; I < Length; I++) {
        if ((Argv[3][I] > 0x7f) || (Argv[3][I] < 0x20)) {
          return EFI_INVALID_PARAMETER;
        }

        Value[I] = (UINT8)Argv[3][I];
      }
    } else {
      return EFI_UNSUPPORTED;
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = SpacemitTlvSet (
             mData,
             sizeof (mData),
             (UINT8)Code,
             Argc == 4 ? Value : NULL,
             Length
             );
  if (!EFI_ERROR (Status)) {
    mDirty = TRUE;
    ShellPrintEx (-1, -1, L"Staged TLV 0x%02x; use -w to persist.\r\n", Code);
  }

  return Status;
}

STATIC SHELL_STATUS EFIAPI
Handler (
  EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  EFI_SYSTEM_TABLE                    *SystemTable,
  EFI_SHELL_PARAMETERS_PROTOCOL       *Parameters,
  EFI_SHELL_PROTOCOL                  *Shell
  )
{
  if (!Parameters || !Parameters->Argc || !Parameters->Argv || !Shell) {
    return SHELL_INVALID_PARAMETER;
  }

  gEfiShellParametersProtocol = Parameters;
  gEfiShellProtocol           = Shell;
  EFI_STATUS  Status = ShellInitialize ();

  gEfiShellParametersProtocol = Parameters;
  gEfiShellProtocol           = Shell;
  if (!EFI_ERROR (Status)) {
    Status = Run (Parameters->Argc, Parameters->Argv);
  }

  if (EFI_ERROR (Status)) {
    ShellPrintEx (-1, -1, L"tlv_eeprom: %r\r\n", Status);
    return Status == EFI_INVALID_PARAMETER ? SHELL_INVALID_PARAMETER :
           Status == EFI_NOT_FOUND ? SHELL_NOT_FOUND : SHELL_DEVICE_ERROR;
  }

  return SHELL_SUCCESS;
}

STATIC CONST CHAR16  mManual[] =
  L".TH tlv_eeprom 0 \"SpacemiT board EEPROM\"\r\n"
  L".SH NAME\r\n"
  L"Read and write the SpacemiT board EEPROM.\r\n"
  L".SH SYNOPSIS\r\n"
  L"tlv_eeprom [-r|-d|-h|-w]\r\n"
  L"tlv_eeprom -s <hexcode> [value]\r\n"
  L"tlv_eeprom -x <hexcode> <hexbytes>\r\n"
  L"tlv_eeprom -b <file>\r\n"
  L"tlv_eeprom -i <file>\r\n"
  L".SH OPTIONS\r\n"
  L"-r  Read physical EEPROM and discard staged changes.\r\n"
  L"-d  Dump the complete staged 256-byte image.\r\n"
  L"-s  Stage a vendor field; omit value to remove it.\r\n"
  L"-x  Stage opaque bytes without interpreting their schema.\r\n"
  L"-w  Persist staged changes, update CRC and verify readback.\r\n"
  L"-b  Save physical EEPROM to a new 256-byte backup file.\r\n"
  L"-i  Stage a valid 256-byte backup for restoration with -w.\r\n"
  L".SH DESCRIPTION\r\n"
  L"0x24 is base MAC; 0x2a is its decimal consecutive-address count.\r\n"
  L"Other TLVs are preserved. -s does not persist until -w.\r\n"
  L"Use tlv_eeprom -h for detailed value syntax.\r\n"
  L".SH EXAMPLES\r\n"
  L"tlv_eeprom -r\r\n"
  L"tlv_eeprom -s 0x24 02:12:34:56:78:90\r\n"
  L"tlv_eeprom -s 0x2a 4\r\n"
  L"tlv_eeprom -w\r\n";

STATIC CHAR16 *EFIAPI
Help (
  EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  CONST CHAR8                         *Language
  )
{
  return AllocateCopyPool (sizeof (mManual), mManual);
}

STATIC EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  mCommand = {
  L"tlv_eeprom", Handler, Help
};

EFI_STATUS EFIAPI
TlvEepromCommandEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->InstallProtocolInterface (
                &Image,
                &gEfiShellDynamicCommandProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &mCommand
                );
}

EFI_STATUS EFIAPI
TlvEepromCommandUnload (
  EFI_HANDLE  Image
  )
{
  return gBS->UninstallProtocolInterface (
                Image,
                &gEfiShellDynamicCommandProtocolGuid,
                &mCommand
                );
}

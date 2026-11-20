/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/Sg2042MacLib.h>

EFI_STATUS
Sg2042PciMac (EFI_HANDLE Controller, EFI_PCI_IO_PROTOCOL *Pci, BOOLEAN Create,
              UINT8 Mac[6], BOOLEAN *NeedsHandoff OPTIONAL)
{
  UINT64 Attributes;
  UINT8 Live[6];
  EFI_STATUS Status = Pci->Attributes (Pci, EfiPciIoAttributeOperationGet, 0, &Attributes);
  if (EFI_ERROR (Status)) { return Status; }
  if (!(Attributes & EFI_PCI_IO_ATTRIBUTE_MEMORY)) {
    Status = Pci->Attributes (Pci, EfiPciIoAttributeOperationEnable, EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
    if (EFI_ERROR (Status)) { return Status; }
  }
  // RTL8125B keeps the factory address in MAC0_BKP.
  Status = Pci->Mem.Read (Pci, EfiPciIoWidthUint8, 2, 0x19e0, 6, Mac);
  if (!EFI_ERROR (Status)) {
    Status = Pci->Mem.Read (Pci, EfiPciIoWidthUint8, 2, 0, 6, Live);
  }
  if (!EFI_ERROR (Status) && NeedsHandoff) { *NeedsHandoff = !Sg2042ValidMac (Mac); }
  if (!EFI_ERROR (Status) && !Sg2042ValidMac (Mac)) {
    CopyMem (Mac, Live, sizeof (Live));
    if (!Sg2042ValidMac (Mac)) {
      EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Controller);
      Status = Path ? Sg2042FallbackMac (Path, GetDevicePathSize (Path), Create, Mac) : EFI_NOT_FOUND;
    }
  }
  if (!EFI_ERROR (Status) && Create && !Sg2042ValidMac (Live) && CompareMem (Live, Mac, 6)) {
    UINT8 Config = 0xc0;
    Status = Pci->Mem.Write (Pci, EfiPciIoWidthUint8, 2, 0x50, 1, &Config);
    if (!EFI_ERROR (Status)) {
      Status = Pci->Mem.Write (Pci, EfiPciIoWidthUint8, 2, 0, 6, Mac);
      Config = 0;
      EFI_STATUS Lock = Pci->Mem.Write (Pci, EfiPciIoWidthUint8, 2, 0x50, 1, &Config);
      if (!EFI_ERROR (Status)) { Status = Lock; }
    }
  }
  if (!(Attributes & EFI_PCI_IO_ATTRIBUTE_MEMORY)) {
    EFI_STATUS Restore = Pci->Attributes (Pci, EfiPciIoAttributeOperationDisable, EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
    if (!EFI_ERROR (Status)) { Status = Restore; }
  }
  return Status;
}

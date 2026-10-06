/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SPACEMIT_K3_HARDWARE_LIB_H
#define SPACEMIT_K3_HARDWARE_LIB_H
#include <Uefi.h>
EFI_STATUS
K3FdtRegister (
  CONST VOID  *Fdt,
  INT32       Node,
  UINTN       Index,
  UINTN       Minimum,
  UINTN       *Base,
  UINTN       *Bytes
  );

EFI_STATUS
K3MapMmio (
  UINTN  Base,
  UINTN  Bytes
  );

EFI_STATUS
K3ApplyPins (
  CONST VOID  *Fdt,
  INT32       Consumer
  );

EFI_STATUS
K3RequireFixedSupply (
  CONST VOID   *Fdt,
  INT32        Consumer,
  CONST CHAR8  *Property,
  UINT32       Microvolts
  );

BOOLEAN
K3FdtEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  );

typedef struct {
  UINTN      Base, Pad, Apbc;
  UINT32     Mask;
  BOOLEAN    ActiveLow;
} K3_GPIO;
EFI_STATUS
K3ParseGpio (
  CONST VOID   *Fdt,
  INT32        Consumer,
  CONST CHAR8  *Property,
  K3_GPIO      *Gpio
  );

EFI_STATUS
K3GpioOutput (
  CONST K3_GPIO  *Gpio,
  BOOLEAN        Active
  );

#endif

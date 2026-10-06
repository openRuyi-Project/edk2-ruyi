/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/DxeServicesTableLib.h>

BOOLEAN
K3FdtEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Status;

  while (Node >= 0) {
    Status = FdtGetProp (Fdt, Node, "status", &Length);
    if (Status && !(((Length == 5) && !AsciiStrCmp (Status, "okay")) ||
                    ((Length == 3) && !AsciiStrCmp (Status, "ok"))))
    {
      return FALSE;
    }

    if (Node == 0) {
      return TRUE;
    }

    Node = FdtParentOffset (Fdt, Node);
  }

  return FALSE;
}

EFI_STATUS
K3FdtRegister (
  CONST VOID  *Fdt,
  INT32       Node,
  UINTN       Index,
  UINTN       Minimum,
  UINTN       *Base,
  UINTN       *Bytes
  )
{
  INT32         Parent, Length;
  CONST UINT32  *Reg;
  UINT64        Address, Size;

  if (!Fdt || (Node < 0) || !Base) {
    return EFI_INVALID_PARAMETER;
  }

  Parent = FdtParentOffset (Fdt, Node);
  if ((Parent < 0) || (FdtAddressCells (Fdt, Parent) != 2) || (FdtSizeCells (Fdt, Parent) != 2)) {
    return EFI_UNSUPPORTED;
  }

  if ((Parent != 0) && ((FdtGetProp (Fdt, Parent, "ranges", &Length) == NULL) || (Length != 0))) {
    return EFI_UNSUPPORTED;
  }

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Reg || (Length < 0) || Length % 16 || (Index >= (UINTN)Length / 16)) {
    return EFI_COMPROMISED_DATA;
  }

  Reg    += Index * 4;
  Address = ((UINT64)Fdt32ToCpu (Reg[0]) << 32) | Fdt32ToCpu (Reg[1]);
  Size    = ((UINT64)Fdt32ToCpu (Reg[2]) << 32) | Fdt32ToCpu (Reg[3]);
  if (!Address || (Address & 3) || (Size < Minimum) || (Address > MAX_UINTN - Size)) {
    return EFI_COMPROMISED_DATA;
  }

  *Base = (UINTN)Address;
  if (Bytes) {
    *Bytes = (UINTN)Size;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3MapMmio (
  UINTN  Base,
  UINTN  Bytes
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  UINT64                           Start, End, Chunk;

  if (!Bytes || (Bytes > MAX_UINTN - EFI_PAGE_MASK) || (Base > MAX_UINTN - Bytes - EFI_PAGE_MASK)) {
    return EFI_INVALID_PARAMETER;
  }

  Start = Base & ~(UINTN)EFI_PAGE_MASK;
  End   = ALIGN_VALUE (Base + Bytes, EFI_PAGE_SIZE);
  while (Start < End) {
    Status = gDS->GetMemorySpaceDescriptor (Start, &Descriptor);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!Descriptor.Length || (Descriptor.BaseAddress > Start) ||
        (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length) ||
        (Descriptor.BaseAddress + Descriptor.Length <= Start))
    {
      return EFI_COMPROMISED_DATA;
    }

    Chunk = MIN (End, Descriptor.BaseAddress + Descriptor.Length) - Start;
    if ((Start | Chunk) & EFI_PAGE_MASK) {
      return EFI_UNSUPPORTED;
    }

    if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Start, Chunk, EFI_MEMORY_UC);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    } else if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
      return EFI_ACCESS_DENIED;
    }

    // A boot consumer may share a register page with a runtime driver.
    Status = gDS->SetMemorySpaceAttributes (
                    Start,
                    Chunk,
                    EFI_MEMORY_UC | (Descriptor.Attributes & EFI_MEMORY_RUNTIME)
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Start += Chunk;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3RequireFixedSupply (
  CONST VOID   *Fdt,
  INT32        Consumer,
  CONST CHAR8  *Property,
  UINT32       Microvolts
  )
{
  INT32         Length, Supply;
  CONST UINT32  *P = FdtGetProp (Fdt, Consumer, Property, &Length);

  if (!P || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Supply = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*P));
  if (!K3FdtEnabled (Fdt, Supply) ||
      (FdtStringListSearch (Fdt, Supply, "compatible", "regulator-fixed") < 0) ||
      !FdtGetProp (Fdt, Supply, "regulator-always-on", &Length) ||
      FdtGetProp (Fdt, Supply, "gpio", &Length) || FdtGetProp (Fdt, Supply, "gpios", &Length))
  {
    return EFI_UNSUPPORTED;
  }

  P = FdtGetProp (Fdt, Supply, "regulator-min-microvolt", &Length);
  if (!P || (Length != 4) || (Fdt32ToCpu (*P) != Microvolts)) {
    return EFI_UNSUPPORTED;
  }

  P = FdtGetProp (Fdt, Supply, "regulator-max-microvolt", &Length);
  return P && Length == 4 && Fdt32ToCpu (*P) == Microvolts ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

typedef struct {
  UINTN     Register;
  UINTN     VoltageRegister;
  UINT32    Value;
  UINT32    Millivolts;
} PIN_PLAN;

EFI_STATUS
K3ParseGpio (
  CONST VOID   *Fdt,
  INT32        Consumer,
  CONST CHAR8  *Property,
  K3_GPIO      *Gpio
  )
{
  INT32                Length, Provider, Clock, Pinctrl;
  UINTN                Base, Pad, Apbc;
  static CONST UINT32  Offsets[] = { 0, 0x40, 0x80, 0x100 };

  if (!Fdt || !Property || !Gpio) {
    return EFI_INVALID_PARAMETER;
  }

  CONST UINT32  *P = FdtGetProp (Fdt, Consumer, Property, &Length);

  if (!P || (Length != 16)) {
    return EFI_COMPROMISED_DATA;
  }

  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (P[0]));
  UINT32  Bank = Fdt32ToCpu (P[1]), Pin = Fdt32ToCpu (P[2]), Flags = Fdt32ToCpu (P[3]);

  if ((Bank >= 4) || (Pin >= 32) || (Flags > 1) || !K3FdtEnabled (Fdt, Provider) ||
      (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-gpio") < 0))
  {
    return EFI_UNSUPPORTED;
  }

  P = FdtGetProp (Fdt, Provider, "#gpio-cells", &Length);
  if (!P || (Length != 4) || (Fdt32ToCpu (*P) != 3)) {
    return EFI_COMPROMISED_DATA;
  }

  EFI_STATUS  Status = K3FdtRegister (Fdt, Provider, 0, Offsets[Bank] + 0x3c, &Base, NULL);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  P = FdtGetProp (Fdt, Provider, "clocks", &Length);
  if (!P || (Length != 16) || (P[0] != P[2]) || (Fdt32ToCpu (P[1]) != 20) || (Fdt32ToCpu (P[3]) != 21) ||
      (FdtStringListSearch (Fdt, Provider, "clock-names", "core") != 0) ||
      (FdtStringListSearch (Fdt, Provider, "clock-names", "bus") != 1))
  {
    return EFI_COMPROMISED_DATA;
  }

  Clock = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (P[0]));
  if (!K3FdtEnabled (Fdt, Clock) ||
      (FdtStringListSearch (Fdt, Clock, "compatible", "spacemit,k3-syscon-apbc") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Clock, 0, 0xc, &Apbc, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  P = FdtGetProp (Fdt, Provider, "gpio-ranges", &Length);
  if (!P || (Length <= 0) || Length % 20) {
    return EFI_COMPROMISED_DATA;
  }

  Pinctrl = -1;
  for (INT32 I = 0; I < Length / 20; I++) {
    if (Fdt32ToCpu (P[I * 5 + 1]) != Bank) {
      continue;
    }

    if ((Pinctrl >= 0) || (Fdt32ToCpu (P[I * 5 + 2]) != 0) || (Fdt32ToCpu (P[I * 5 + 3]) != Bank * 32) ||
        (Fdt32ToCpu (P[I * 5 + 4]) != 32))
    {
      return EFI_UNSUPPORTED;
    }

    Pinctrl = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (P[I * 5]));
  }

  if (!K3FdtEnabled (Fdt, Pinctrl) ||
      (FdtStringListSearch (Fdt, Pinctrl, "compatible", "spacemit,k3-pinctrl") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Pinctrl, 0, (Bank * 32 + Pin + 1) * 4, &Pad, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Base + Offsets[Bank], 0x3c);
  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Apbc, 0xc);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Pad + (Bank * 32 + Pin) * 4, 4);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Gpio = (K3_GPIO) {
    Base + Offsets[Bank], Pad + (Bank * 32 + Pin) * 4, Apbc, 1U << Pin, Flags != 0
  };
  return EFI_SUCCESS;
}

EFI_STATUS
K3GpioOutput (
  CONST K3_GPIO  *Gpio,
  BOOLEAN        Active
  )
{
  if (!Gpio || !Gpio->Base || !Gpio->Mask || (Gpio->Mask & (Gpio->Mask - 1))) {
    return EFI_INVALID_PARAMETER;
  }

  BOOLEAN  High = Active != Gpio->ActiveLow;

  // GPIO clocks are shared: enable/deassert without pulsing global reset.
  MmioAndThenOr32 (Gpio->Apbc + 8, ~BIT2, BIT0 | BIT1);
  if ((MmioRead32 (Gpio->Apbc + 8) & 7) != 3) {
    return EFI_DEVICE_ERROR;
  }

  // Establish the output latch before connecting the pad/output driver.
  MmioWrite32 (Gpio->Base + (High ? 8 : 0xc), Gpio->Mask);
  MmioAndThenOr32 (Gpio->Pad, ~7U, BIT6);
  MmioWrite32 (Gpio->Base + 0x1c, Gpio->Mask);
  // GPLR samples the pad and can still report the previous level here.
  // The consumer provides any required reset assertion/release delay.
  if (!(MmioRead32 (Gpio->Base + 4) & Gpio->Mask)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3ApplyPins (
  CONST VOID  *Fdt,
  INT32       Consumer
  )
{
  PIN_PLAN      Plan[64];
  UINTN         Count = 0, Mfpr, MfprSize, Apbc, ApbcSize;
  INT32         Length, Group, Child, Pinctrl, ApbcNode;
  CONST UINT32  *States = FdtGetProp (Fdt, Consumer, "pinctrl-0", &Length);

  if (!States || (Length <= 0) || Length % 4) {
    return EFI_COMPROMISED_DATA;
  }

  Pinctrl = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-pinctrl");
  if (!K3FdtEnabled (Fdt, Pinctrl)) {
    return EFI_NOT_FOUND;
  }

  EFI_STATUS  Status = K3FdtRegister (Fdt, Pinctrl, 0, 0x1000, &Mfpr, &MfprSize);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  INT32         StateLength = Length;
  CONST UINT32  *Reference  = FdtGetProp (Fdt, Pinctrl, "spacemit,apbc", &Length);

  if (!Reference || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  ApbcNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Reference));
  if (!K3FdtEnabled (
         Fdt,
         ApbcNode
         ) || (FdtStringListSearch (Fdt, ApbcNode, "compatible", "spacemit,k3-syscon-apbc") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, ApbcNode, 0, 0x58, &Apbc, &ApbcSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (INT32 S = 0; S < StateLength / 4; S++) {
    Group = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (States[S]));
    if (!K3FdtEnabled (Fdt, Group) || (FdtParentOffset (Fdt, Group) != Pinctrl)) {
      return EFI_COMPROMISED_DATA;
    }

    for (Child = FdtFirstSubnode (Fdt, Group); Child >= 0; Child = FdtNextSubnode (Fdt, Child)) {
      CONST UINT32  *Pins = FdtGetProp (Fdt, Child, "pinmux", &Length);
      if (!Pins || (Length <= 0) || Length % 4) {
        return EFI_COMPROMISED_DATA;
      }

      INT32         PinBytes = Length;
      CONST UINT32  *Voltage = FdtGetProp (Fdt, Child, "power-source", &Length);
      if (!Voltage || (Length != 4)) {
        return EFI_UNSUPPORTED;
      }

      UINT32  Mv = Fdt32ToCpu (*Voltage);
      if ((Mv != 1800) && (Mv != 3300)) {
        return EFI_UNSUPPORTED;
      }

      CONST UINT32  *Strength = FdtGetProp (Fdt, Child, "drive-strength", &Length);
      if (!Strength || (Length != 4)) {
        return EFI_UNSUPPORTED;
      }

      UINT32               Ma = Fdt32ToCpu (*Strength), Drive = 0;
      static CONST UINT32  Drive18[] = { 2, 4, 6, 7, 9, 11, 13, 14, 21, 23, 25, 26, 28, 30, 31, 33 };
      static CONST UINT32  Drive33[] = { 3, 5, 7, 9, 11, 13, 15, 17, 25, 27, 29, 31, 33, 35, 37, 38 };
      while (Drive < 15 && Ma > (Mv == 1800 ? Drive18[Drive] : Drive33[Drive])) {
        Drive++;
      }

      if (!Ma || (Ma > ((Mv == 1800) ? 33U : 38U))) {
        return EFI_UNSUPPORTED;
      }

      UINT32        Pull  = 0;
      CONST UINT32  *Bias = FdtGetProp (Fdt, Child, "bias-pull-up", &Length);
      if (Bias) {
        if ((Length != 0) && (Length != 4)) {
          return EFI_UNSUPPORTED;
        }

        Pull = BIT15 | BIT14 | ((!Length || Fdt32ToCpu (*Bias) == 1) ? BIT3 : 0);
      }

      if (FdtGetProp (Fdt, Child, "bias-pull-down", &Length)) {
        if (Bias) {
          return EFI_COMPROMISED_DATA;
        }

        Pull = BIT15 | BIT13;
      }

      for (INT32 P = 0; P < PinBytes / 4; P++) {
        UINT32  Encoded = Fdt32ToCpu (Pins[P]), Pin = Encoded >> 16, Function = Encoded & 0xffff;
        UINT32  Offset;
        if ((Function > 7) || (Count >= ARRAY_SIZE (Plan))) {
          return EFI_UNSUPPORTED;
        }

        if (Pin <= 20) {
          Offset = 0x804;
        } else if (Pin <= 41) {
          Offset = 0x80c;
        } else if ((Pin >= 76) && (Pin <= 98)) {
          Offset = 0x820;
        } else if ((Pin >= 99) && (Pin <= 127)) {
          Offset = 0x810;
        } else if ((Pin >= 138) && (Pin <= 144)) {
          Offset = 0x82c;
        } else {
          return EFI_UNSUPPORTED;
        }

        Plan[Count] = (PIN_PLAN) {
          // K3 MFPR has two reserved words after pad 130. DT pad IDs
          // remain consecutive, matching the Linux pinctrl binding.
          Mfpr + (Pin > 130 ? Pin + 2 : Pin) * 4, Mfpr + Offset,
          Function | (Drive << 9) | Pull | BIT6, Mv
        };
        for (UINTN Prev = 0; Prev < Count; Prev++) {
          if ((Plan[Prev].Register == Plan[Count].Register) ||
              ((Plan[Prev].VoltageRegister == Plan[Count].VoltageRegister) && (Plan[Prev].Millivolts != Mv)))
          {
            return EFI_COMPROMISED_DATA;
          }
        }

        Count++;
      }
    }
  }

  if (!Count) {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3MapMmio (Mfpr, MfprSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Apbc, ApbcSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioOr32 (Apbc + 0x3c, BIT0 | BIT1);
  for (UINTN I = 0; I < Count; I++) {
    // The protected IO selector permits exactly one access after each key pair.
    // Follow the board DT voltage; do not infer old voltage from its read-zero quirk.
    MmioWrite32 (Apbc + 0x50, 0xbaba);
    MemoryFence ();
    MmioWrite32 (Apbc + 0x54, 0xeb10);
    MemoryFence ();
    MmioWrite32 (Plan[I].VoltageRegister, Plan[I].Millivolts == 1800 ? BIT2 : 0);
    MemoryFence ();
    MmioAndThenOr32 (Plan[I].Register, ~(UINT32)0xff7f, Plan[I].Value);
  }

  return EFI_SUCCESS;
}

/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* K3 CCU field/rate descriptions follow the public K3 register definitions.
   One provider owns divider gates and references shared by all consumers. */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/TimerLib.h>
#include "Core.h"

enum {
  P1D4, P1D5, P1D6, P1D8, P1D48, P1D52, P1D96, P1D32,
  P2D4, P2D5, P2D6, P2D7, P2D8, P6D6, P7D5, P1D128, P1D10, P1DX,
  EXTERNAL_DP, SYSTEM_AXI
};

typedef struct {
  UINT8     Pll;
  UINT8     Bit;
  UINT16    Div;
  UINT32    MpmuGate;
  UINT32    DividerMask;
  UINT32    DividerValue;
} ROOT;
STATIC CONST ROOT  mRoots[] = {
  { 0, 3,  4,   BIT15, 0,         0         },
  { 0, 4,  5,   BIT21, 0,         0         },
  { 0, 5,  6,   BIT0,  0,         0         },
  { 0, 7,  8,   BIT13, 0,         0         },
  { 0, 7,  48,  BIT11, 0,         0         },
  { 0, 3,  52,  BIT10, 0,         0         },
  { 0, 7,  96,  BIT4,  0,         0         },
  { 0, 7,  32,  BIT13, 0,         0         },
  { 1, 3,  4,   0,     0,         0         },
  { 1, 4,  5,   0,     0,         0         },
  { 1, 5,  6,   0,     0,         0         },
  { 1, 6,  7,   0,     0,         0         },
  { 1, 7,  8,   0,     0,         0         },
  { 2, 5,  6,   0,     0,         0         },
  { 3, 4,  5,   0,     0,         0         },
  { 0, 31, 128, 0,     0,         0         },
  { 0, 4,  10,  BIT18, 0,         0         },
  { 0, 22, 32,  0,     31U << 23, 31U << 23 }
};
typedef struct {
  UINT32    Id;
  UINT32    Reg;
  UINT32    Gate;
  UINT32    FcReg;
  UINT32    Fc;
  UINT8     MuxShift;
  UINT8     MuxWidth;
  UINT8     DivShift;
  UINT8     DivWidth;
  UINT8     Count;
  UINT8     Parents[8];
} LEAF;
STATIC CONST LEAF  mLeaves[] = {
  {
    35, 0x60, BIT4, 0x60, BIT12, 6, 3, 9, 3, 7,
    { P1D6,        P2D8,  P1D8,  P1D10, 0xff, P1DX, P1D5  }
  },
  {
    36, 0x60, BIT3, 0, 0, 0, 0, 0, 0, 1,
    { SYSTEM_AXI }
  },
  {
    47, 0x26c, BIT2, 0, 0, 0, 2, 0, 0, 4,
    { P1D48,       P1D52, P1D96, P1D32 }
  },
  {
    48, 0x270, BIT14, 0x26c, BIT26, 29, 3, 25, 3, 7,
    { P1D4,        P1D5,  P7D5,  P6D6,  P2D7, P1D6, P1D48 }
  },
  {
    49, 0x270, BIT16, 0x26c, BIT30, 21, 3, 17, 3, 8,
    { P1D4,        P1D5,  P7D5,  P6D6,  P2D7, P2D4, P1D48, P2D8}
  },
  {
    50, 0x270, BIT0, 0x26c, BIT29, 5, 3, 1, 4, 4,
    { P1D6,        P1D5,  P1D4,  P1D8 }
  },
  {
    51, 0x274, BIT1, 0x274, BIT30, 5, 3, 2, 3, 5,
    { P1D6,        P1D5,  P1D4,  P1D8,  P2D4 }
  },
  {
    53, 0x268, BIT1, 0x268, BIT8, 2, 3, 5, 3, 4,
    { P1D5,        P1D6,  P2D6,  P2D5 }
  },
  {
    55, 0x23c, BIT17, 0, 0, 18, 1, 0, 0, 2,
    { EXTERNAL_DP, EXTERNAL_DP }
  },
  {
    94, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    { P1D128 }
  }
};
STATIC CONST struct {
  UINT32    Id;
  UINT32    Reg;
  UINT32    Bit;
} mResets[] = {
  { 21, 0x60,  BIT1  },
  { 22, 0x60,  BIT0  },
  { 53, 0x26c, BIT3  },
  { 54, 0x26c, BIT4  },
  { 55, 0x270, BIT9  },
  { 56, 0x270, BIT15 },
  { 57, 0x274, BIT0  },
  { 59, 0x268, BIT0  },
  { 61, 0x23c, BIT16 }
};
STATIC UINTN         mApmu;
STATIC UINTN         mPll;
STATIC UINTN         mMpmu;
STATIC UINT32        mPhandle;
STATIC UINT16        mUsers[ARRAY_SIZE (mLeaves)];
STATIC UINT16        mPllUsers[4][32];
STATIC UINT16        mMpmuUsers[32];
STATIC BOOLEAN       mPllKeep[4][32];
STATIC UINT32        mPllDividerKeep[4][32];
STATIC BOOLEAN       mMpmuKeep[32];
STATIC CONST UINT32  mPllOff[] = { 0x100, 0x118, 0x148, 0x158 };
STATIC INTN
Find (
  UINT32  Id
  )
{
  for (UINTN I = 0; I < ARRAY_SIZE (mLeaves); I++) {
    if (mLeaves[I].Id == Id) {
      return (INTN)I;
    }
  }

  return -1;
}

STATIC UINT32
Mask (
  UINT8  Width,
  UINT8  Shift
  )
{
  return Width ? ((1U << Width) - 1) << Shift : 0;
}

STATIC UINT64
RootRate (
  UINT32   Index,
  BOOLEAN  Gates
  )
{
  STATIC CONST UINT32  W1[]    = { 0x0b330ccc, 0x0b3e2000, 0x0b422aaa, 0x0b3a1555 };
  STATIC CONST UINT32  Frac[]  = { 0xcd00, 0, 0xab00, 0x5500 };
  STATIC CONST UINT32  W3[]    = { 0xa0558989, 0xa0558c8c, 0xa0558e8e, 0xa0558b8b };
  STATIC CONST UINT32  Locks[] = { BIT24, BIT25, BIT29, BIT30 };
  STATIC CONST UINT64  Rate[]  = { 2457600000ULL, 3000000000ULL, 3200000000ULL, 2800000000ULL };

  if (Index == SYSTEM_AXI) {
    UINT32  Control = MmioRead32 (mApmu + 0x388);
    if (Control & BIT4) {
      return 0;
    }

    return RootRate ((Control & BIT0) ? P1D6 : P1D8, TRUE) /
           (((Control >> 1) & 3) + 1);
  }

  if (Index >= ARRAY_SIZE (mRoots)) {
    return 0;
  }

  CONST ROOT  *R = &mRoots[Index];
  UINTN       A  = mPll + mPllOff[R->Pll];
  UINT32      V  = MmioRead32 (A + 4);

  // A live programmable output belongs to its existing consumers. Only an
  // inactive output can be configured when acquiring its first reference.
  if (R->DividerMask && (Gates || (V & (1U << R->Bit))) &&
      ((V & R->DividerMask) != R->DividerValue))
  {
    return 0;
  }

  if ((MmioRead32 (A) != W1[R->Pll]) || (MmioRead32 (A + 8) != W3[R->Pll]) ||
      ((V & 0xff00) != Frac[R->Pll]) || !(V & BIT16) || !(MmioRead32 (mMpmu + 0x10) & Locks[R->Pll]))
  {
    return 0;
  }

  if (Gates && (!(V & (1U << R->Bit)) || ((MmioRead32 (mMpmu + 0x1024) & R->MpmuGate) != R->MpmuGate))) {
    return 0;
  }

  return Rate[R->Pll] / R->Div;
}

// CPU clocks are read-only here. Mux/divider and PLLA encodings follow
// the K3 clock-provider register definitions; no frequency change is made.
STATIC EFI_STATUS
CpuRate (
  UINT32  Id,
  UINT64  *Rate
  )
{
  STATIC CONST struct {
    UINT64    Rate;
    UINT32    Swcr1, Fraction, Swcr3;
  } CpuPllRates[] = {
    { 1050000000ULL, 0x0b2b3000, 0x00000000, 0xa0458080 },
    { 1100000000ULL, 0x0b2d3555, 0x00005500, 0xa0458181 },
    { 1150000000ULL, 0x0b2f3aaa, 0x0000ab00, 0xa0458181 },
    { 1200000000ULL, 0x0b320000, 0x00000000, 0xa0458181 },
    { 1250000000ULL, 0x0b340555, 0x00005500, 0xa0458181 },
    { 1300000000ULL, 0x0b360aaa, 0x0000ab00, 0xa0458282 },
    { 1350000000ULL, 0x0b381000, 0x00000000, 0xa0458282 },
    { 1400000000ULL, 0x0b3a1555, 0x00005500, 0xa0458282 },
    { 1450000000ULL, 0x0b3c1aaa, 0x0000ab00, 0xa0458383 },
    { 1500000000ULL, 0x0b3e2000, 0x00000000, 0xa0458383 },
    { 1550000000ULL, 0x0b402555, 0x00005500, 0xa0458383 },
    { 1600000000ULL, 0x0b422aaa, 0x0000ab00, 0xa0458484 },
    { 1650000000ULL, 0x0b443000, 0x00000000, 0xa0458484 },
    { 1700000000ULL, 0x0b463555, 0x00005500, 0xa0458484 },
    { 1750000000ULL, 0x0b483aaa, 0x0000ab00, 0xa0458585 },
    { 1800000000ULL, 0x0b4b0000, 0x00000000, 0xa0458585 },
    { 1850000000ULL, 0x0b4d0555, 0x00005500, 0xa0458585 },
    { 1900000000ULL, 0x0b4f0aaa, 0x0000ab00, 0xa0458686 },
    { 1950000000ULL, 0x0b511000, 0x00000000, 0xa0458686 },
    { 2000000000ULL, 0x0b292aaa, 0x0000ab00, 0xa0558686 },
    { 2050000000ULL, 0x0b2a2d55, 0x00005500, 0xa0558686 },
    { 2100000000ULL, 0x0b2b3000, 0x00000000, 0xa0558787 },
    { 2150000000ULL, 0x0b2c32aa, 0x0000ab00, 0xa0558787 },
    { 2200000000ULL, 0x0b2d3555, 0x00005500, 0xa0558787 },
    { 2250000000ULL, 0x0b2e3800, 0x00000000, 0xa0558888 },
    { 2300000000ULL, 0x0b2f3aaa, 0x0000ab00, 0xa0558888 },
    { 2350000000ULL, 0x0b303d55, 0x00005500, 0xa0558888 },
    { 2400000000ULL, 0x0b320000, 0x00000000, 0xa0558989 },
  };

  if (!Rate) {
    return EFI_INVALID_PARAMETER;
  }

  UINT32  Control = MmioRead32 (mApmu + 0x38c + (Id - 2) * 4);
  UINT32  Parent  = Control & 7;
  UINT64  Hz      = 0;

  if (Control & BIT12) {
    return EFI_NOT_READY;
  }

  switch (Parent) {
    case 0: Hz = RootRate (P1D4, FALSE) * 4 / 3;
      break;
    case 1: Hz = RootRate (P1D5, FALSE);
      break;
    case 2: Hz = RootRate (P1D4, FALSE);
      break;
    case 3: Hz = RootRate (P2D4, FALSE) * 4 / 3;
      break;
    case 5: Hz = RootRate (P1D4, FALSE) * 2;
      break;
    case 6: Hz = RootRate (P2D4, FALSE) * 2;
      break;
    default:
    {
      UINT32  PllIndex = Id - 2;
      if ((Id == 3) && (Parent == 7) && (Control & BIT13)) {
        PllIndex = 0;
      }

      UINTN   Pll   = mPll + 0x124 + PllIndex * 12;
      UINT32  Swcr1 = MmioRead32 (Pll);
      UINT32  Swcr2 = MmioRead32 (Pll + 4);
      UINT32  Swcr3 = MmioRead32 (Pll + 8);
      if (!(Swcr2 & BIT16) || !(MmioRead32 (mMpmu + 0x10) & (BIT26 << PllIndex))) {
        return EFI_NOT_READY;
      }

      for (UINTN I = 0; I < ARRAY_SIZE (CpuPllRates); I++) {
        if ((Swcr1 == CpuPllRates[I].Swcr1) &&
            ((Swcr2 & 0xff00) == CpuPllRates[I].Fraction) &&
            (Swcr3 == CpuPllRates[I].Swcr3))
        {
          Hz = CpuPllRates[I].Rate / (Parent == 4 ? 2 : 1);
          break;
        }
      }
    }
  }

  if (Hz == 0) {
    return EFI_NOT_READY;
  }

  // The divider applies only to mux inputs 0 through 3.
  *Rate = Parent < 4 ? Hz / (((Control >> 3) & 7) + 1) : Hz;
  return EFI_SUCCESS;
}

STATIC UINT32
ParentIndex (
  CONST LEAF  *L
  )
{
  return L->MuxWidth ? (MmioRead32 (mApmu + L->Reg) >> L->MuxShift) & ((1U << L->MuxWidth) - 1) : 0;
}

STATIC EFI_STATUS
RootEnable (
  UINT32   Index,
  BOOLEAN  Enable
  )
{
  // The PHY provider owns this parent.
  if (Index == EXTERNAL_DP) {
    return EFI_SUCCESS;
  }

  if (Index == SYSTEM_AXI) {
    // The system-critical AXI parent is inherited, never retuned or gated.
    return !Enable || RootRate (Index, TRUE) ? EFI_SUCCESS : EFI_NOT_READY;
  }

  if (Index >= ARRAY_SIZE (mRoots)) {
    return EFI_UNSUPPORTED;
  }

  CONST ROOT  *R  = &mRoots[Index];
  UINTN       A   = mPll + mPllOff[R->Pll] + 4;
  UINT32      Bit = 1U << R->Bit;

  if (Enable) {
    if (!RootRate (Index, FALSE)) {
      return EFI_NOT_READY;
    }

    if (!mPllUsers[R->Pll][R->Bit]++) {
      mPllKeep[R->Pll][R->Bit]        = (MmioRead32 (A) & Bit) != 0;
      mPllDividerKeep[R->Pll][R->Bit] = MmioRead32 (A) & R->DividerMask;
      if (!mPllKeep[R->Pll][R->Bit]) {
        MmioAndThenOr32 (A, ~R->DividerMask, R->DividerValue | Bit);
      }
    }

    for (UINT32 B = 0; B < 32; B++) {
      if (R->MpmuGate & (1U << B)) {
        if (!mMpmuUsers[B]++) {
          mMpmuKeep[B] = (MmioRead32 (mMpmu + 0x1024) & (1U << B)) != 0;
          if (!mMpmuKeep[B]) {
            MmioOr32 (mMpmu + 0x1024, 1U << B);
          }
        }
      }
    }

    if (RootRate (Index, TRUE)) {
      return EFI_SUCCESS;
    }

    RootEnable (Index, FALSE);
    return EFI_DEVICE_ERROR;
  }

  for (UINT32 B = 0; B < 32; B++) {
    if ((R->MpmuGate & (1U << B)) && mMpmuUsers[B]) {
      if (!--mMpmuUsers[B] && !mMpmuKeep[B]) {
        MmioAnd32 (mMpmu + 0x1024, ~(1U << B));
      }
    }
  }

  if (mPllUsers[R->Pll][R->Bit] && !--mPllUsers[R->Pll][R->Bit] && !mPllKeep[R->Pll][R->Bit]) {
    MmioAnd32 (A, ~Bit);
    if (R->DividerMask) {
      MmioAndThenOr32 (A, ~R->DividerMask, mPllDividerKeep[R->Pll][R->Bit]);
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
Resolve (
  CONST VOID   *F,
  INT32        Node,
  CONST CHAR8  *Name,
  BOOLEAN      Reset,
  UINT32       *Id
  )
{
  if (!F || !Id || FdtCheckHeader (F) || !K3FdtEnabled (F, Node)) {
    return EFI_INVALID_PARAMETER;
  }

  INT32  Index = Name ? FdtStringListSearch (F, Node, Reset ? "reset-names" : "clock-names", Name) : 0, Length;

  if (Index < 0) {
    return EFI_NOT_FOUND;
  }

  CONST UINT32  *P = FdtGetProp (F, Node, Reset ? "resets" : "clocks", &Length);

  if (!P || (Length < 4) || (Length & 3)) {
    return EFI_COMPROMISED_DATA;
  }

  for (INT32 Pos = 0, Entry = 0; Pos < Length / 4; Entry++) {
    UINT32        Phandle = Fdt32ToCpu (P[Pos++]);
    INT32         Provider = FdtNodeOffsetByPhandle (F, Phandle), L;
    CONST UINT32  *Cells = FdtGetProp (F, Provider, Reset ? "#reset-cells" : "#clock-cells", &L);
    if (!K3FdtEnabled (F, Provider) || !Cells || (L != 4) || (Fdt32ToCpu (*Cells) > 1)) {
      return EFI_UNSUPPORTED;
    }

    UINT32  Count = Fdt32ToCpu (*Cells);
    if (Pos + (INT32)Count > Length / 4) {
      return EFI_COMPROMISED_DATA;
    }

    if (Entry == Index) {
      if ((Phandle != mPhandle) || (Count != 1)) {
        return EFI_UNSUPPORTED;
      }

      UINT32   Value = Fdt32ToCpu (P[Pos]);
      BOOLEAN  Known = Find (Value) >= 0;
      if (Reset) {
        Known = FALSE;
        for (UINTN I = 0; I < ARRAY_SIZE (mResets); I++) {
          if (mResets[I].Id == Value) {
            Known = TRUE;
          }
        }
      }

      if (!Known) {
        return EFI_UNSUPPORTED;
      }

      *Id = Value;
      return EFI_SUCCESS;
    }

    Pos += Count;
  }

  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
EFIAPI
RoundRate (
  UINT32  Id,
  UINT64  Requested,
  UINT64  *Actual,
  UINT32  *Parent,
  UINT32  *Divider
  )
{
  INTN  N = Find (Id);

  if ((N < 0) || !Requested || !Actual || !Parent || !Divider) {
    return EFI_INVALID_PARAMETER;
  }

  CONST LEAF  *L = &mLeaves[N];
  UINT64      Best = MAX_UINT64, Rate = 0;
  UINT32      Sel = 0, Div = 1;

  for (UINT32 P = 0; P < L->Count; P++) {
    UINT64  Hz = RootRate (L->Parents[P], FALSE);
    if (!Hz) {
      continue;
    }

    for (UINT32 D = 1; D <= (1U << L->DivWidth); D++) {
      UINT64  R = Hz / D, Error = R > Requested ? R - Requested : Requested - R;
      if (Error < Best) {
        Best = Error;
        Rate = R;
        Sel  = P;
        Div  = D;
      }
    }
  }

  if (Best == MAX_UINT64) {
    return EFI_NOT_READY;
  }

  *Actual  = Rate;
  *Parent  = Sel;
  *Divider = Div;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
GetRate (
  UINT32  Id,
  UINT64  *Rate
  )
{
  if ((Id == 2) || (Id == 3)) {
    return CpuRate (Id, Rate);
  }

  INTN  N = Find (Id);

  if ((N < 0) || !Rate) {
    return EFI_INVALID_PARAMETER;
  }

  CONST LEAF  *L = &mLeaves[N];
  UINT32      P  = ParentIndex (L);

  if (L->Fc && (MmioRead32 (mApmu + L->FcReg) & L->Fc)) {
    return EFI_NOT_READY;
  }

  if (P >= L->Count) {
    return EFI_DEVICE_ERROR;
  }

  if ((Id == 55) && (P == 0)) {
    return GetRate (49, Rate);
  }

  UINT64  Hz = RootRate (L->Parents[P], TRUE);

  if (!Hz) {
    return EFI_NOT_READY;
  }

  UINT32  Div = L->DivWidth ? ((MmioRead32 (mApmu + L->Reg) >> L->DivShift) & ((1U << L->DivWidth) - 1)) + 1 : 1;

  *Rate = Hz / Div;
  return EFI_SUCCESS;
}

STATIC BOOLEAN
EFIAPI
IsEnabled (
  UINT32  Id
  )
{
  INTN  N = Find (Id);

  return N >= 0 && (mLeaves[N].Gate ? (MmioRead32 (mApmu + mLeaves[N].Reg) & mLeaves[N].Gate) != 0 : mUsers[N] != 0);
}

STATIC EFI_STATUS
Configure (
  INTN    N,
  UINT32  Parent,
  UINT32  Divider
  )
{
  CONST LEAF  *L = &mLeaves[N];

  if ((Parent >= L->Count) || !Divider || (Divider > (1U << L->DivWidth))) {
    return EFI_INVALID_PARAMETER;
  }

  if (mUsers[N] || IsEnabled (L->Id)) {
    return EFI_ACCESS_DENIED;
  }

  if (!L->MuxWidth && !L->DivWidth) {
    return EFI_SUCCESS;
  }

  if (L->Fc && (MmioRead32 (mApmu + L->FcReg) & L->Fc)) {
    return EFI_NOT_READY;
  }

  UINT32  Bits  = Mask (L->MuxWidth, L->MuxShift) | Mask (L->DivWidth, L->DivShift);
  UINT32  Value = (Parent << L->MuxShift) | ((Divider - 1) << L->DivShift);

  if (L->Fc) {
    EFI_STATUS  Status = RootEnable (L->Parents[Parent], TRUE);

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  MmioAndThenOr32 (mApmu + L->Reg, ~Bits, Value & Bits);
  if (L->Fc) {
    // FC needs the selected parent and the leaf clock running, even while
    // the consumer is held in reset. Retain this reference on timeout so
    // an unfinished transition cannot lose its clock source.
    MmioOr32 (mApmu + L->Reg, L->Gate);
    mUsers[N] = 1;
    if (!IsEnabled (L->Id)) {
      mUsers[N] = 0;
      RootEnable (L->Parents[Parent], FALSE);
      return EFI_DEVICE_ERROR;
    }

    MmioOr32 (mApmu + L->FcReg, L->Fc);
    UINTN  Us;
    for (Us = 0; Us < 10000; Us += 5) {
      if (!(MmioRead32 (mApmu + L->FcReg) & L->Fc)) {
        break;
      }

      MicroSecondDelay (5);
    }

    if (Us == 10000) {
      return EFI_TIMEOUT;
    }

    MmioAnd32 (mApmu + L->Reg, ~L->Gate);
    if (IsEnabled (L->Id)) {
      return EFI_DEVICE_ERROR;
    }

    mUsers[N] = 0;
    RootEnable (L->Parents[Parent], FALSE);
  }

  return (MmioRead32 (mApmu + L->Reg) & Bits) == (Value & Bits) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
EFIAPI
SetRate (
  UINT32  Id,
  UINT64  Requested,
  UINT64  *Actual
  )
{
  UINT64      Rate;
  UINT32      P;
  UINT32      D;
  EFI_STATUS  S = RoundRate (Id, Requested, &Rate, &P, &D);

  if (EFI_ERROR (S)) {
    return S;
  }

  S = Configure (Find (Id), P, D);
  if (!EFI_ERROR (S) && Actual) {
    *Actual = Rate;
  }

  return S;
}

STATIC EFI_STATUS
EFIAPI
SetParent (
  UINT32  Id,
  UINT32  Parent
  )
{
  INTN  N = Find (Id);

  if (N < 0) {
    return EFI_UNSUPPORTED;
  }

  CONST LEAF  *L = &mLeaves[N];
  UINT32      D  = L->DivWidth ? ((MmioRead32 (mApmu + L->Reg) >> L->DivShift) & ((1U << L->DivWidth) - 1)) + 1 : 1;

  return Configure (N, Parent, D);
}

STATIC EFI_STATUS
EFIAPI
Enable (
  UINT32   Id,
  BOOLEAN  On
  )
{
  INTN  N = Find (Id);

  if (N < 0) {
    return EFI_UNSUPPORTED;
  }

  CONST LEAF  *L = &mLeaves[N];
  UINT32      P  = ParentIndex (L);

  if (P >= L->Count) {
    return EFI_DEVICE_ERROR;
  }

  if (On) {
    if (L->Fc && (MmioRead32 (mApmu + L->FcReg) & L->Fc)) {
      return EFI_NOT_READY;
    }

    if (mUsers[N] == MAX_UINT16) {
      return EFI_OUT_OF_RESOURCES;
    }

    if (mUsers[N]) {
      mUsers[N]++;
      return EFI_SUCCESS;
    }

    EFI_STATUS  S = (Id == 55 && P == 0) ? Enable (49, TRUE) : RootEnable (L->Parents[P], TRUE);
    if (EFI_ERROR (S)) {
      return S;
    }

    if (L->Gate) {
      MmioOr32 (mApmu + L->Reg, L->Gate);
    }

    if (L->Gate && !IsEnabled (Id)) {
      if ((Id == 55) && (P == 0)) {
        Enable (49, FALSE);
      } else {
        RootEnable (L->Parents[P], FALSE);
      }

      return EFI_DEVICE_ERROR;
    }

    mUsers[N] = 1;
    return EFI_SUCCESS;
  }

  if (mUsers[N] > 1) {
    mUsers[N]--;
    return EFI_SUCCESS;
  }

  if (L->Fc && (MmioRead32 (mApmu + L->FcReg) & L->Fc)) {
    return EFI_NOT_READY;
  }

  // An explicit disable can quiesce an unclaimed firmware boot-state leaf.
  if (L->Gate) {
    MmioAnd32 (mApmu + L->Reg, ~L->Gate);
  }

  if (L->Gate && IsEnabled (Id)) {
    return EFI_DEVICE_ERROR;
  }

  if (mUsers[N]) {
    mUsers[N] = 0;
    return (Id == 55 && P == 0) ? Enable (49, FALSE) : RootEnable (L->Parents[P], FALSE);
  }

  return EFI_SUCCESS;
}

STATIC BOOLEAN
EFIAPI
IsResetAsserted (
  UINT32  Id
  )
{
  for (UINTN I = 0; I < ARRAY_SIZE (mResets); I++) {
    if (mResets[I].Id == Id) {
      return !(MmioRead32 (mApmu + mResets[I].Reg) & mResets[I].Bit);
    }
  }

  return TRUE;
}

STATIC EFI_STATUS
EFIAPI
Reset (
  UINT32   Id,
  BOOLEAN  Assert
  )
{
  for (UINTN I = 0; I < ARRAY_SIZE (mResets); I++) {
    if (mResets[I].Id == Id) {
      MmioAndThenOr32 (mApmu + mResets[I].Reg, ~mResets[I].Bit, Assert ? 0 : mResets[I].Bit);
      return IsResetAsserted (Id) == Assert ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
K3ClockCoreInitialize (
  CONST VOID               *Fdt,
  SPACEMIT_CLOCK_PROTOCOL  **Protocol
  )
{
  if (!Fdt || !Protocol || FdtCheckHeader (Fdt) || (FdtTotalSize (Fdt) > SIZE_1MB)) {
    return EFI_INVALID_PARAMETER;
  }

  INT32  A = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-syscon-apmu");
  INT32  P = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-pll"), Length;

  if (!K3FdtEnabled (Fdt, A) || !K3FdtEnabled (Fdt, P)) {
    return EFI_NOT_FOUND;
  }

  CONST UINT32  *Ref = FdtGetProp (Fdt, P, "clocks", &Length);

  if (!Ref || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  INT32  X = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Ref));

  Ref = FdtGetProp (Fdt, X, "clock-frequency", &Length);
  if (!K3FdtEnabled (Fdt, X) || !Ref || (Length != 4) || (Fdt32ToCpu (*Ref) != 24000000)) {
    return EFI_UNSUPPORTED;
  }

  Ref = FdtGetProp (Fdt, P, "spacemit,mpmu", &Length);
  if (!Ref || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  INT32  M = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Ref));

  if (!K3FdtEnabled (Fdt, M) || (FdtStringListSearch (Fdt, M, "compatible", "spacemit,k3-syscon-mpmu") < 0)) {
    return EFI_UNSUPPORTED;
  }

  EFI_STATUS  S = K3FdtRegister (Fdt, A, 0, 0x394, &mApmu, NULL);

  if (!EFI_ERROR (S)) {
    S = K3FdtRegister (Fdt, P, 0, 0x164, &mPll, NULL);
  }

  if (!EFI_ERROR (S)) {
    S = K3FdtRegister (Fdt, M, 0, 0x1028, &mMpmu, NULL);
  }

  if (EFI_ERROR (S)) {
    return S;
  }

  if ((mApmu != 0xd4282800) || (mPll != 0xd4090000) || (mMpmu != 0xd4050000)) {
    return EFI_UNSUPPORTED;
  }

  S = K3MapMmio (mApmu, 0x394);
  if (!EFI_ERROR (S)) {
    S = K3MapMmio (mPll, 0x164);
  }

  if (!EFI_ERROR (S)) {
    S = K3MapMmio (mMpmu, 0x1028);
  }

  if (EFI_ERROR (S)) {
    return S;
  }

  mPhandle = FdtGetPhandle (Fdt, A);
  if (!mPhandle) {
    return EFI_COMPROMISED_DATA;
  }

  ZeroMem (mUsers, sizeof (mUsers));
  ZeroMem (mPllUsers, sizeof (mPllUsers));
  ZeroMem (mMpmuUsers, sizeof (mMpmuUsers));
  STATIC SPACEMIT_CLOCK_PROTOCOL  Api = {
    1,
    Resolve,
    RoundRate,
    SetRate,
    GetRate,
    Enable,
    Reset,
    SetParent,
    IsEnabled,
    IsResetAsserted
  };

  *Protocol = &Api;
  return EFI_SUCCESS;
}

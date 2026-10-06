/**
*
*  Copyright (c) 2025, SpacemiT Co., Ltd. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>

#include "InnoDpReg.h"
#include <Library/K3InnoDpLib.h>
#include "DpErrors.h"

#define SOC_DP_VCO_MIN_KHZ        1000000
#define SOC_DP_VCO_MAX_KHZ        3000000
#define SOC_DP_PLL_FRAC_MOD       16777216
#define SOC_DP_PLL_ERR_TOLERANCE  10

typedef struct {
  UINT32     VcoFreqKhz;
  UINT8      Prediv;
  UINT16     Fbdiv;
  UINT32     Frac;
  UINT8      PostdivReg;
  UINT8      FracPd;
  UINT8      VcoclkDiv8En;
  UINT8      PostdivEn;
  UINT8      Clk16mdiv;
  UINT32     ActualRateKhz;
  BOOLEAN    Valid;
} SOC_DP_CORE_PLL_CFG;

typedef struct {
  UINT32     VcoFreqKhz;
  UINT8      Prediv;
  UINT16     Fbdiv;
  UINT32     Frac;
  UINT8      Div5En;
  UINT8      Divm;
  UINT8      Divaux;
  UINT8      Divp;
  UINT8      FracPd;
  UINT32     ActualPclkKhz;
  BOOLEAN    Valid;
} SOC_DP_PIXEL_PLL_CFG;

STATIC
INTN
SocDpRegWrite (
  IN SOC_DP_PHY  *Phy,
  IN UINT32      Offset,
  IN UINT32      BitWide,
  IN UINT32      Mask,
  IN UINT32      Val
  )
{
  UINT32  RegVal;

  RegVal  = MmioRead32 ((UINTN)Phy->Regs + Offset);
  RegVal &= ~Mask;
  RegVal |= Val & Mask;
  MmioWrite32 ((UINTN)Phy->Regs + Offset, RegVal);

  return 0;
}

STATIC
INTN
SocDpRegWriteRange (
  IN SOC_DP_PHY  *Phy,
  IN UINT32      Offset,
  IN UINT32      High,
  IN UINT32      Low,
  IN UINT32      Val
  )
{
  UINT32  Mask;

  Mask = (UINT32)(((((UINT64)1) << (High - Low + 1)) - 1) << Low);
  return SocDpRegWrite (Phy, Offset, 32, Mask, (Val << Low) & Mask);
}

STATIC
INTN
SocDpRegRead (
  IN SOC_DP_PHY  *Phy,
  IN UINT32      Offset,
  IN UINT32      BitWide,
  IN UINT32      Mask,
  OUT UINT32     *Val
  )
{
  *Val = (MmioRead32 ((UINTN)Phy->Regs + Offset)) & Mask;
  return 0;
}

STATIC
INTN
SocDpRegReadRange (
  IN SOC_DP_PHY  *Phy,
  IN UINT32      Offset,
  IN UINT32      High,
  IN UINT32      Low,
  OUT UINT32     *Val
  )
{
  INTN    Ret;
  UINT32  Mask;

  Mask = (UINT32)(((((UINT64)1) << (High - Low + 1)) - 1) << Low);
  Ret  = SocDpRegRead (Phy, Offset, 32, Mask, Val);
  *Val = *Val >> Low;

  return Ret;
}

STATIC
UINT32
SocDpDiv64 (
  IN OUT UINT64  *N,
  IN UINT32      Base
  )
{
  UINT32  Rem;

  Rem = *N % Base;
  *N  = *N / Base;
  return Rem;
}

STATIC
UINT64
SocDpAbsDiff (
  IN UINT64  A,
  IN UINT64  B
  )
{
  return (A > B) ? (A - B) : (B - A);
}

STATIC
INTN
SocDpIsBetterConfig (
  IN BOOLEAN  NewValid,
  IN BOOLEAN  NewIsInt,
  IN UINT8    NewPre,
  IN UINT32   NewVco,
  IN BOOLEAN  BestValid,
  IN BOOLEAN  BestIsInt,
  IN UINT8    BestPre,
  IN UINT32   BestVco
  )
{
  if (!NewValid) {
    return 0;
  }

  if (!BestValid) {
    return 1;
  }

  if (NewIsInt && !BestIsInt) {
    return 1;
  }

  if (!NewIsInt && BestIsInt) {
    return 0;
  }

  if (NewPre < BestPre) {
    return 1;
  }

  if (NewPre > BestPre) {
    return 0;
  }

  if (NewVco > BestVco) {
    return 1;
  }

  return 0;
}

STATIC
INTN
SocDpSolvePllFrac (
  IN UINT32   TargetVcoKhz,
  IN UINT32   RefClkKhz,
  OUT UINT8   *BestPre,
  OUT UINT16  *BestFb,
  OUT UINT32  *BestFrac
  )
{
  UINT64   MinErr;
  INTN     Found;
  BOOLEAN  BestIsInt;
  INTN     Pre;

  MinErr    = ~0ULL;
  Found     = 0;
  BestIsInt = FALSE;

  for (Pre = 1; Pre <= 63; Pre++) {
    UINT64   RefClkHz;
    UINT64   TargetVcoHz;
    UINT64   Num;
    UINT64   Den;
    UINT64   Remainder;
    UINT64   FbVal;
    UINT64   FracVal;
    UINT64   ActualVco;
    UINT64   Diff;
    BOOLEAN  CurrentIsInt;

    RefClkHz    = (UINT64)RefClkKhz * 1000;
    TargetVcoHz = (UINT64)TargetVcoKhz * 1000;

    Num = TargetVcoHz * Pre;
    Den = RefClkHz;

    FbVal     = Num;
    Remainder = SocDpDiv64 (&FbVal, (UINT32)Den);

    if (FbVal > 4095) {
      continue;
    }

    FracVal  = Remainder * SOC_DP_PLL_FRAC_MOD;
    FracVal += (Den / 2);
    SocDpDiv64 (&FracVal, (UINT32)Den);

    if (FracVal > 0xFFFFFF) {
      FracVal = 0xFFFFFF;
    }

    {
      UINT64  VcoInt;
      UINT64  VcoFrac;

      VcoInt = RefClkHz * FbVal;
      SocDpDiv64 (&VcoInt, Pre);

      VcoFrac = RefClkHz * FracVal;
      SocDpDiv64 (&VcoFrac, Pre);
      SocDpDiv64 (&VcoFrac, SOC_DP_PLL_FRAC_MOD);

      ActualVco = VcoInt + VcoFrac;
    }

    Diff         = SocDpAbsDiff (ActualVco, TargetVcoHz);
    CurrentIsInt = (FracVal == 0);

    if (Diff < MinErr) {
      MinErr    = Diff;
      *BestPre  = Pre;
      *BestFb   = (UINT16)FbVal;
      *BestFrac = (UINT32)FracVal;
      BestIsInt = CurrentIsInt;
      Found     = 1;
    } else if (Diff == MinErr) {
      if (CurrentIsInt && !BestIsInt) {
        *BestPre  = Pre;
        *BestFb   = (UINT16)FbVal;
        *BestFrac = (UINT32)FracVal;
        BestIsInt = TRUE;
        Found     = 1;
      }
    }
  }

  return Found ? 0 : -1;
}

STATIC
UINT32
SocDpGetRateKhz (
  IN UINT8   Pre,
  IN UINT16  Fb,
  IN UINT32  Frac,
  IN UINT32  RefClkKhz,
  IN UINT32  TotalDiv
  )
{
  UINT64  VcoHz;
  UINT64  RefHz;
  UINT64  IntPart;
  UINT64  FracPart;

  RefHz = (UINT64)RefClkKhz * 1000;

  IntPart  = RefHz * Fb;
  IntPart += (Pre / 2);
  SocDpDiv64 (&IntPart, Pre);

  FracPart  = RefHz * Frac;
  FracPart += (Pre / 2);
  SocDpDiv64 (&FracPart, Pre);

  FracPart += (SOC_DP_PLL_FRAC_MOD / 2);
  SocDpDiv64 (&FracPart, SOC_DP_PLL_FRAC_MOD);

  VcoHz = IntPart + FracPart;

  VcoHz += (TotalDiv / 2);
  SocDpDiv64 (&VcoHz, TotalDiv);

  VcoHz += 500;
  SocDpDiv64 (&VcoHz, 1000);

  return (UINT32)VcoHz;
}

STATIC
INTN
SocDpCalcCorePll (
  IN UINT32                TargetRateKbps,
  IN UINT32                RefClkKhz,
  OUT SOC_DP_CORE_PLL_CFG  *Cfg
  )
{
  SOC_DP_CORE_PLL_CFG  Best;
  UINT32               TargetBitclkBase;
  INTN                 I;
  UINT32               ValidPostdivs[] = { 1, 2, 4, 8, 16, 32 };

  ZeroMem (Cfg, sizeof (*Cfg));
  ZeroMem (&Best, sizeof (Best));

  TargetBitclkBase = TargetRateKbps / 2;

  for (I = 0; I < 6; I++) {
    SOC_DP_CORE_PLL_CFG  Curr;
    UINT32               PostDivRatio;
    UINT32               TargetVco;
    UINT8                Pre;
    UINT16               Fb;
    UINT32               Frac;
    UINT32               ActualVco;
    UINT32               ActualRate;

    ZeroMem (&Curr, sizeof (Curr));

    PostDivRatio = ValidPostdivs[I];
    TargetVco    = TargetBitclkBase * PostDivRatio;

    if ((TargetVco < SOC_DP_VCO_MIN_KHZ) || (TargetVco > SOC_DP_VCO_MAX_KHZ)) {
      continue;
    }

    if (SocDpSolvePllFrac (TargetVco, RefClkKhz, &Pre, &Fb, &Frac) == 0) {
      ActualVco  = SocDpGetRateKhz (Pre, Fb, Frac, RefClkKhz, 1);
      ActualRate = (ActualVco / PostDivRatio) * 2;

      if (SocDpAbsDiff (ActualRate, TargetRateKbps) > SOC_DP_PLL_ERR_TOLERANCE) {
        continue;
      }

      Curr.Valid         = TRUE;
      Curr.VcoFreqKhz    = TargetVco;
      Curr.ActualRateKhz = ActualRate;
      Curr.Prediv        = Pre;
      Curr.Fbdiv         = Fb;
      Curr.Frac          = Frac;
      Curr.FracPd        = (Frac == 0) ? 3 : 0;

      if (PostDivRatio == 1) {
        Curr.PostdivReg   = 0;
        Curr.PostdivEn    = 0;
        Curr.VcoclkDiv8En = 0;
      } else {
        if (PostDivRatio == 2) {
          Curr.PostdivReg = 0;
        } else if (PostDivRatio == 4) {
          Curr.PostdivReg = 1;
        } else if (PostDivRatio == 8) {
          Curr.PostdivReg = 3;
        } else if (PostDivRatio == 16) {
          Curr.PostdivReg = 5;
        } else if (PostDivRatio == 32) {
          Curr.PostdivReg = 7;
        } else {
          Curr.PostdivReg = 0;
        }

        Curr.PostdivEn    = 1;
        Curr.VcoclkDiv8En = 1;
      }

      Curr.Clk16mdiv = (ActualRate / 2) / 64000;

      if (SocDpIsBetterConfig (
            Curr.Valid,
            (Curr.Frac == 0),
            Curr.Prediv,
            Curr.VcoFreqKhz,
            Best.Valid,
            (Best.Frac == 0),
            Best.Prediv,
            Best.VcoFreqKhz
            ))
      {
        Best = Curr;
      }
    }
  }

  if (!Best.Valid) {
    return -EINVAL;
  }

  *Cfg = Best;
  return 0;
}

STATIC
VOID
SocDpCalcCorePllToReg (
  IN SOC_DP_PHY           *Phy,
  IN SOC_DP_CORE_PLL_CFG  *Cfg
  )
{
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_PD, 1);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_PREDIV, Cfg->Prediv);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_FBDIV_LBIT, Cfg->Fbdiv & 0xFF);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_FBDIV_HBIT, (Cfg->Fbdiv >> 8) & 0xF);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_DACPD, (Cfg->FracPd >> 1) & 0x1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_DSMPD, Cfg->FracPd & 0x1);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_FRAC_LBIT, Cfg->Frac & 0xFF);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_FRAC_MBIT, (Cfg->Frac >> 8) & 0xFF);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_FRAC_HBIT, (Cfg->Frac >> 16) & 0xFF);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_POSTDIV, Cfg->PostdivReg);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_POSTDIVEN, Cfg->PostdivEn);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_VCOCLK_DIV8_EN, Cfg->VcoclkDiv8En);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_CLKDIV_16M, Cfg->Clk16mdiv);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_LOCK_BYPEN, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_PD, 0);
  MicroSecondDelay (2000);
}

STATIC
INTN
SocDpCalcPixelPll (
  IN UINT32                 TargetPclkKhz,
  IN UINT32                 RefClkKhz,
  OUT SOC_DP_PIXEL_PLL_CFG  *Cfg
  )
{
  SOC_DP_PIXEL_PLL_CFG  Best;
  INTN                  PclkDiv;

  ZeroMem (Cfg, sizeof (*Cfg));
  ZeroMem (&Best, sizeof (Best));

  {
    SOC_DP_PIXEL_PLL_CFG  Curr;
    UINT32                DivTotal;
    UINT32                TargetVco;
    UINT8                 Pre;
    UINT16                Fb;
    UINT32                Frac;
    UINT32                ActualPclk;

    ZeroMem (&Curr, sizeof (Curr));

    DivTotal  = 5;
    TargetVco = TargetPclkKhz * DivTotal;

    if ((TargetVco >= SOC_DP_VCO_MIN_KHZ) && (TargetVco <= SOC_DP_VCO_MAX_KHZ)) {
      if (SocDpSolvePllFrac (TargetVco, RefClkKhz, &Pre, &Fb, &Frac) == 0) {
        ActualPclk = SocDpGetRateKhz (Pre, Fb, Frac, RefClkKhz, DivTotal);

        if (SocDpAbsDiff (ActualPclk, TargetPclkKhz) <= SOC_DP_PLL_ERR_TOLERANCE) {
          Curr.Valid         = TRUE;
          Curr.VcoFreqKhz    = TargetVco;
          Curr.ActualPclkKhz = ActualPclk;
          Curr.Prediv        = Pre;
          Curr.Fbdiv         = Fb;
          Curr.Frac          = Frac;

          Curr.FracPd = (Frac == 0) ? 3 : 0;

          Curr.Div5En = 1;
          Curr.Divaux = 0;
          Curr.Divm   = 0;
          Curr.Divp   = 0;

          if (SocDpIsBetterConfig (
                Curr.Valid,
                (Curr.Frac == 0),
                Curr.Prediv,
                Curr.VcoFreqKhz,
                Best.Valid,
                (Best.Frac == 0),
                Best.Prediv,
                Best.VcoFreqKhz
                ))
          {
            Best = Curr;
          }
        }
      }
    }
  }

  for (PclkDiv = 1; PclkDiv <= 31; PclkDiv++) {
    INTN  DimFactors[] = { 1, 2, 3, 5 };
    INTN  DimRegs[]    = { 0, 1, 2, 3 };
    INTN  I;

    for (I = 0; I < 4; I++) {
      SOC_DP_PIXEL_PLL_CFG  Curr;
      INTN                  MVal;
      UINT32                DivTotal;
      UINT32                TargetVco;
      UINT8                 Pre;
      UINT16                Fb;
      UINT32                Frac;
      UINT32                ActualPclk;

      ZeroMem (&Curr, sizeof (Curr));

      MVal      = DimFactors[I];
      DivTotal  = 2 * MVal * PclkDiv;
      TargetVco = TargetPclkKhz * DivTotal;

      if ((TargetVco < SOC_DP_VCO_MIN_KHZ) || (TargetVco > SOC_DP_VCO_MAX_KHZ)) {
        continue;
      }

      if (SocDpSolvePllFrac (TargetVco, RefClkKhz, &Pre, &Fb, &Frac) == 0) {
        ActualPclk = SocDpGetRateKhz (Pre, Fb, Frac, RefClkKhz, DivTotal);

        if (SocDpAbsDiff (ActualPclk, TargetPclkKhz) > SOC_DP_PLL_ERR_TOLERANCE) {
          continue;
        }

        Curr.Valid         = TRUE;
        Curr.VcoFreqKhz    = TargetVco;
        Curr.ActualPclkKhz = ActualPclk;
        Curr.Prediv        = Pre;
        Curr.Fbdiv         = Fb;
        Curr.Frac          = Frac;

        Curr.FracPd = (Frac == 0) ? 3 : 0;

        Curr.Div5En = 0;
        Curr.Divaux = 1;
        Curr.Divm   = DimRegs[I];
        Curr.Divp   = PclkDiv;

        if (SocDpIsBetterConfig (
              Curr.Valid,
              (Curr.Frac == 0),
              Curr.Prediv,
              Curr.VcoFreqKhz,
              Best.Valid,
              (Best.Frac == 0),
              Best.Prediv,
              Best.VcoFreqKhz
              ))
        {
          Best = Curr;
        }
      }
    }

    {
      INTN  Aux;

      for (Aux = 2; Aux <= 31; Aux++) {
        SOC_DP_PIXEL_PLL_CFG  Curr;
        UINT32                DivTotal;
        UINT32                TargetVco;
        UINT8                 Pre;
        UINT16                Fb;
        UINT32                Frac;
        UINT32                ActualPclk;

        ZeroMem (&Curr, sizeof (Curr));

        DivTotal  = 2 * Aux * PclkDiv;
        TargetVco = TargetPclkKhz * DivTotal;

        if ((TargetVco < SOC_DP_VCO_MIN_KHZ) || (TargetVco > SOC_DP_VCO_MAX_KHZ)) {
          continue;
        }

        if (SocDpSolvePllFrac (TargetVco, RefClkKhz, &Pre, &Fb, &Frac) == 0) {
          ActualPclk = SocDpGetRateKhz (Pre, Fb, Frac, RefClkKhz, DivTotal);

          if (SocDpAbsDiff (ActualPclk, TargetPclkKhz) > SOC_DP_PLL_ERR_TOLERANCE) {
            continue;
          }

          Curr.Valid         = TRUE;
          Curr.VcoFreqKhz    = TargetVco;
          Curr.ActualPclkKhz = ActualPclk;
          Curr.Prediv        = Pre;
          Curr.Fbdiv         = Fb;
          Curr.Frac          = Frac;

          Curr.FracPd = (Frac == 0) ? 3 : 0;

          Curr.Div5En = 0;
          Curr.Divaux = Aux;
          Curr.Divm   = 0;
          Curr.Divp   = PclkDiv;

          if (SocDpIsBetterConfig (
                Curr.Valid,
                (Curr.Frac == 0),
                Curr.Prediv,
                Curr.VcoFreqKhz,
                Best.Valid,
                (Best.Frac == 0),
                Best.Prediv,
                Best.VcoFreqKhz
                ))
          {
            Best = Curr;
          }
        }
      }
    }
  }

  if (!Best.Valid) {
    return -EINVAL;
  }

  *Cfg = Best;
  return 0;
}

STATIC
VOID
SocDpCalcPixelPllToReg (
  IN SOC_DP_PHY            *Phy,
  IN SOC_DP_PIXEL_PLL_CFG  *Cfg
  )
{
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PD, 1);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PREDIV, Cfg->Prediv);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_FBDIV2_LBIT, Cfg->Fbdiv & 0xFF);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_FBDIV2_HBIT, (Cfg->Fbdiv >> 8) & 0xF);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_DACPD, (Cfg->FracPd >> 1) & 0x1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_DSMPD, Cfg->FracPd & 0x1);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_FRAC2_LBIT, Cfg->Frac & 0xFF);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_FRAC2_MBIT, (Cfg->Frac >> 8) & 0xFF);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_FRAC2_HBIT, (Cfg->Frac >> 16) & 0xFF);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PRECLK_DIVM, Cfg->Divm);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PRECLK_DIVAUX, Cfg->Divaux);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PCLKDIV5_EN, Cfg->Div5En);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PCLK_DIVAUX, Cfg->Divp);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PD, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_REG_PCLK_OUTPUT_NORMAL, 1);
  MicroSecondDelay (2000);
}

STATIC
INTN
SocDpCheckPllLock (
  IN SOC_DP_PHY  *Phy
  )
{
  UINT32  PllLocked;

  SocDpRegReadRange (Phy, SOC_DPTX_AD_LOCK_PIXELPLL, &PllLocked);

  if (!PllLocked) {
    DP_DEBUG (Phy, (DEBUG_ERROR, "Pre_pll unlocked\n"));
    return -ETIMEDOUT;
  }

  SocDpRegReadRange (Phy, SOC_DPTX_AD_LOCK_COREPLL, &PllLocked);

  if (!PllLocked) {
    DP_DEBUG (Phy, (DEBUG_ERROR, "Post_pll unlocked\n"));
    return -ETIMEDOUT;
  }

  return 0;
}

INTN
SocDpPhyExit (
  IN SOC_DP_PHY  *Phy
  )
{
  UINT32  Value;

  if (!Phy || !Phy->Regs) {
    return -EINVAL;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_XMIT_ENABLE, 0);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_PD, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PD, 1);
  MicroSecondDelay (2000);

  SocDpRegReadRange (Phy, SOC_DPTX_XMIT_ENABLE, &Value);
  if (Value) {
    return -EIO;
  }

  SocDpRegReadRange (Phy, SOC_DPTX_ANA_MPLL_PD, &Value);
  if (!Value) {
    return -EIO;
  }

  SocDpRegReadRange (Phy, SOC_DPTX_ANA_PREPLL_PD, &Value);
  if (!Value) {
    return -EIO;
  }

  return 0;
}

INTN
SocDpPhyPowerOff (
  IN SOC_DP_PHY  *Phy
  )
{
  UINT32  Value;

  if (!Phy || !Phy->Regs) {
    return -EINVAL;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_XMIT_ENABLE, 0);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PD, 1);
  MicroSecondDelay (2000);

  SocDpRegReadRange (Phy, SOC_DPTX_XMIT_ENABLE, &Value);
  if (Value) {
    return -EIO;
  }

  SocDpRegReadRange (Phy, SOC_DPTX_ANA_PREPLL_PD, &Value);
  if (!Value) {
    return -EIO;
  }

  return 0;
}

INTN
SocDpPhyPowerOn (
  IN SOC_DP_PHY  *Phy
  )
{
  INTN    Ret;
  INTN    Retry;
  UINT32  LaneEn;

  if (!Phy || !Phy->Regs) {
    return -EINVAL;
  }

  switch (Phy->LaneCount) {
    case SOC_DP_LANE_1:
      LaneEn = 0x1;
      break;
    case SOC_DP_LANE_2:
      LaneEn = 0x3;
      break;
    case SOC_DP_LANE_4:
      LaneEn = 0xF;
      break;
    default:
      return -EINVAL;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_MPLL_PD, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_PD, 0);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Phy, SOC_DPTX_XMIT_ENABLE, LaneEn);
  MicroSecondDelay (2000);

  for (Retry = 0; Retry < 3; Retry++) {
    Ret = SocDpCheckPllLock (Phy);
    if (!Ret) {
      break;
    }

    MicroSecondDelay (2000);
  }

  return Ret;
}

STATIC
VOID
SocDpPhyConfigLanes (
  IN SOC_DP_PHY  *Phy,
  IN INTN        LaneCount
  )
{
  UINT32  PhyLanesVal;

  switch (LaneCount) {
    case SOC_DP_LANE_1:
      PhyLanesVal = 0;
      break;
    case SOC_DP_LANE_2:
      PhyLanesVal = 1;
      break;
    case SOC_DP_LANE_4:
    default:
      PhyLanesVal = 2;
      break;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_PHY_NUM_LANES, PhyLanesVal);
  Phy->LaneCount = LaneCount;
}

STATIC
INTN
SocDpPhyConfigRate (
  IN SOC_DP_PHY  *Phy,
  IN INTN        RateKhz
  )
{
  UINT32               RateVal;
  INTN                 Ret;
  SOC_DP_CORE_PLL_CFG  CorePllCfg;

  RateVal = 0;

  switch (RateKhz) {
    case SOC_DP_LINK_RATE_1_62:
      RateVal = 0;
      break;
    case SOC_DP_LINK_RATE_2_70:
      RateVal = 1;
      break;
    case SOC_DP_LINK_RATE_5_40:
      RateVal = 2;
      break;
    case SOC_DP_LINK_RATE_8_10:
    default:
      RateVal = 3;
      break;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_PHY_RATE, RateVal);

  Ret = SocDpCalcCorePll (RateKhz, Phy->RefClkKhz, &CorePllCfg);
  if (Ret) {
    DP_DEBUG (Phy, (DEBUG_ERROR, "Failed to calc core PLL\n"));
    return Ret;
  }

  SocDpCalcCorePllToReg (Phy, &CorePllCfg);
  Phy->LinkRateKhz = RateKhz;
  return 0;
}

STATIC
VOID
SocDpPhySetVoltages (
  IN SOC_DP_PHY                 *Phy,
  IN SOC_DP_PHY_CONFIGURE_OPTS  *Opts
  )
{
  for (UINTN Lane = 0; Lane < Phy->LaneCount; Lane++) {
    UINT32  Swing  = Opts->Voltage[Lane] & 0x3;
    UINT32  Preemp = Opts->Pre[Lane] & 0x3;
    UINT32  Shift  = (UINT32)Lane * 6;
    UINT32  Mask   = 0xfU << Shift;
    UINT32  Value  = (Swing << 2 | Preemp) << Shift;

    /* K3 uses the digital lane trim, as in its Linux PHY driver. */
    SocDpRegWrite (Phy, 0x104, 32, Mask, Value);
  }
}

INTN
SocDpPhyConfigure (
  IN SOC_DP_PHY                 *Phy,
  IN SOC_DP_PHY_CONFIGURE_OPTS  *Opts
  )
{
  INTN  Ret;

  if (!Phy || !Phy->Regs || !Opts ||
      ((Phy->RefClkKhz != 24000) && (Phy->RefClkKhz != 50000)) ||
      (Opts->SetLanes > 1) || (Opts->SetRate > 1) || (Opts->SetVoltages > 1))
  {
    return -EINVAL;
  }

  UINT8  Lanes = Opts->SetLanes ? Opts->Lanes : Phy->LaneCount;

  if ((Lanes != 1) && (Lanes != 2) && (Lanes != 4)) {
    return -EINVAL;
  }

  if (Opts->SetRate && (Opts->LinkRate != 1620) && (Opts->LinkRate != 2700) &&
      (Opts->LinkRate != 5400) && (Opts->LinkRate != 8100))
  {
    return -EINVAL;
  }

  if (Opts->SetVoltages) {
    for (UINTN Lane = 0; Lane < Lanes; Lane++) {
      if ((Opts->Voltage[Lane] > 3) || (Opts->Pre[Lane] > 3) ||
          (Opts->Voltage[Lane] + Opts->Pre[Lane] > 3))
      {
        return -EINVAL;
      }
    }
  }

  if (Opts->SetLanes) {
    SocDpPhyConfigLanes (Phy, Opts->Lanes);
  }

  if (Opts->SetRate) {
    Ret = SocDpPhyConfigRate (Phy, Opts->LinkRate * 1000);
    if (Ret) {
      return Ret;
    }
  }

  if (Opts->SetVoltages) {
    SocDpPhySetVoltages (Phy, Opts);
  }

  return 0;
}

INTN
SocDpPhyRoundPixelClk (
  IN UINT32   RefClkKhz,
  IN UINT32   RequestedKhz,
  OUT UINT32  *ActualKhz
  )
{
  SOC_DP_PIXEL_PLL_CFG  Config;

  if (!ActualKhz || !RequestedKhz || (RequestedKhz > 1500000) ||
      ((RefClkKhz != 24000) && (RefClkKhz != 50000)))
  {
    return -EINVAL;
  }

  INTN  Result = SocDpCalcPixelPll (RequestedKhz, RefClkKhz, &Config);

  if (!Result) {
    *ActualKhz = Config.ActualPclkKhz;
  }

  return Result;
}

INTN
SocDpPhySetPixelClk (
  IN SOC_DP_PHY  *Phy,
  IN UINT32      PixelClkKhz
  )
{
  SOC_DP_PIXEL_PLL_CFG  PixelPllCfg;
  INTN                  Ret;

  if (!Phy || !Phy->Regs || !PixelClkKhz || (PixelClkKhz > 1500000) ||
      ((Phy->RefClkKhz != 24000) && (Phy->RefClkKhz != 50000)))
  {
    return -EINVAL;
  }

  DP_DEBUG (Phy, (DEBUG_INFO, "Setting Pixel PLL to %d kHz\n", PixelClkKhz));
  Ret = SocDpCalcPixelPll (PixelClkKhz, Phy->RefClkKhz, &PixelPllCfg);
  if (Ret) {
    return Ret;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_DP_EN, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_PREPLL_HDMI_EN, 0);

  SocDpCalcPixelPllToReg (Phy, &PixelPllCfg);

  return 0;
}

INTN
SocDpPhyInit (
  IN SOC_DP_PHY  *Phy,
  IN UINTN       BaseAddr,
  IN UINT32      RefClkKhz
  )
{
  INTN                       Ret;
  UINT32                     ClkDiv;
  UINT32                     MIsel;
  UINT32                     MMainsel;
  UINT32                     MPre;
  UINT32                     MPost;
  UINT32                     TxMode;
  UINT32                     TxPre;
  SOC_DP_PHY_CONFIGURE_OPTS  PhyOpts;

  if (!Phy || !BaseAddr || (BaseAddr & 3) || (BaseAddr > MAX_UINTN - 0x4000) ||
      ((RefClkKhz != 24000) && (RefClkKhz != 50000)))
  {
    return -EINVAL;
  }

  ZeroMem (Phy, sizeof (*Phy));
  Phy->Regs      = BaseAddr;
  Phy->RefClkKhz = RefClkKhz;

  ClkDiv = RefClkKhz / 100;

  MIsel    = 0x5;
  MMainsel = 0x19;
  MPre     = 0x0;
  MPost    = 0x2;
  TxMode   = 0x1;
  TxPre    = 0x0;

  Ret = SocDpPhyExit (Phy);
  if (Ret) {
    return Ret;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_PHY_RESET, 0x1);
  MicroSecondDelay (5000);
  SocDpRegWriteRange (Phy, SOC_DPTX_PHY_RESET, 0x0);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Phy, SOC_DPTX_PHY_BUSY_BYP, 0x1);

  SocDpRegWriteRange (Phy, SOC_DPTX_ENHANCE_FRAMING_EN, 0x1);

  ZeroMem (&PhyOpts, sizeof (PhyOpts));
  PhyOpts.Lanes    = SOC_DP_LANE_2;
  PhyOpts.LinkRate = SOC_DP_LINK_RATE_2_70 / 1000;
  PhyOpts.SetLanes = 1;
  PhyOpts.SetRate  = 1;

  Ret = SocDpPhyConfigure (Phy, &PhyOpts);
  if (Ret) {
    return Ret;
  }

  Ret = SocDpPhySetPixelClk (Phy, 148500);
  if (Ret) {
    return Ret;
  }

  Ret = SocDpPhyPowerOn (Phy);
  if (Ret) {
    return Ret;
  }

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D0, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D1, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D2, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D3, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_RTCAL_FREQDIV_HBIT, (ClkDiv >> 8) & 0x7f);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_RTCAL_BYPASS, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_RTCAL_FREQDIV_LBIT, ClkDiv & 0xff);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_BG_RCAL_SEL, 0);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_RTM_D3, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_RTM_D2, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_RTM_D1, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_RTM_D0, 0);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_RTCAL_BYPASS, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_RTCAL_BYPASS, 0);
  MicroSecondDelay (100000);

  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_PRE_D3, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_PRE_D2, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_PRE_D1, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_PRE_D0, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_DE_D3, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_DE_D2, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_DE_D1, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_DE_D0, 1);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_PRE_D3, TxPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_PRE_D2, TxPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_PRE_D1, TxPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_PRE_D0, TxPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_ISEL_DRV_D3, MIsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_ISEL_DRV_D2, MIsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MAINSEL_D2, MMainsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MAINSEL_D3, MMainsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_ISEL_DRV_D1, MIsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_ISEL_DRV_D0, MIsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_D1, MPost);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_D0, MPost);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_D3, MPost);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_POSTSEL_D2, MPost);
  SocDpRegWriteRange (Phy, SOC_DPTX_DA_TX_MAINSEL_D0_4_0, MMainsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MAINSEL_D1, MMainsel);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_PRESEL_D1, MPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_PRESEL_D0, MPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_PRESEL_D3, MPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_PRESEL_D2, MPre);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D3, TxMode);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D2, TxMode);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D1, TxMode);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_MODE_D0, TxMode);
  SocDpRegWriteRange (Phy, SOC_DPTX_ANA_TX_AUX_RX_VSEL, 0x0);

  return 0;
}

/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* EDID/DisplayID field definitions and public timing values reviewed against
   the pinned Linux DRM/EDID reference. This is an independent byte parser. */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3DisplayModeLib.h>
typedef struct {
  UINT32     Vmin, Vmax, Hmin, Hmax, Width;
  UINT64     MaxClock;
  BOOLEAN    Valid;
} RANGE;
typedef struct {
  K3_DISPLAY_MODE_SET     Set;
  RANGE                   Range;
  BOOLEAN                 Continuous;
  UINT32                  Rate, Lanes;
  K3_MODE_CLOCK_SELECT    Select;
  VOID                    *Priv;
} PARSER;
STATIC UINT32
U16 (
  CONST UINT8  *P
  )
{
  return P[0] | (UINT32)P[1] << 8;
}

STATIC BOOLEAN
Checksum (
  CONST UINT8  *P,
  UINTN        N
  )
{
  UINT8  Sum = 0;

  for (UINTN I = 0; I < N; I++) {
    Sum += P[I];
  }

  return Sum == 0;
}

STATIC BOOLEAN
ExtensionValid (
  CONST UINT8  *B
  )
{
  if (B[0] == 2) {
    UINTN  End = B[2];
    if (!End) {
      return TRUE;
    }

    if ((End < 4) || (End > 127)) {
      return FALSE;
    }

    for (UINTN I = 4; I < End;) {
      UINTN  N = B[I] & 31;
      if (N > End - I - 1) {
        return FALSE;
      }

      I += N + 1;
    }
  } else if (B[0] == 0x70) {
    UINTN  End = 5U + B[2];
    if ((End >= 127) || !Checksum (B + 1, End)) {
      return FALSE;
    }

    for (UINTN I = 5; I < End;) {
      if ((End - I < 3) || (B[I + 2] > End - I - 3)) {
        return FALSE;
      }

      if ((((B[I] == 3) && (B[1] < 0x20)) || ((B[I] == 0x22) && (B[1] == 0x20))) && B[I + 2] % 20) {
        return FALSE;
      }

      I += B[I + 2] + 3;
    }
  }

  return TRUE;
}

STATIC BOOLEAN
Geometry (
  CONST SOC_DP_VIDEO_MODE  *M
  )
{
  return SocDpModeValid (M) && M->Hdisplay <= 3840 && M->Vdisplay <= 2160 &&
         M->HsyncEnd - M->HsyncStart <= 1023 && M->VsyncEnd - M->VsyncStart <= 1023 &&
         M->Htotal - M->HsyncEnd <= 4095 && M->HsyncStart - M->Hdisplay <= 4095 && M->Vtotal - M->VsyncEnd <= 4095;
}

STATIC BOOLEAN
InRange (
  PARSER                   *P,
  CONST SOC_DP_VIDEO_MODE  *M,
  UINT64                   Hz
  )
{
  RANGE   *R     = &P->Range;
  UINT64  Pixels = (UINT64)M->Htotal * M->Vtotal;

  return R->Valid && Hz >= (UINT64)R->Hmin * 1000 * M->Htotal &&
         Hz <= (UINT64)R->Hmax * 1000 * M->Htotal && Hz >= (UINT64)R->Vmin * Pixels &&
         Hz <= (UINT64)R->Vmax * Pixels && Hz <= R->MaxClock && (!R->Width || M->Hdisplay <= R->Width);
}

STATIC EFI_STATUS
Add (
  PARSER             *P,
  SOC_DP_VIDEO_MODE  M,
  BOOLEAN            Preferred
  )
{
  if (!Geometry (&M)) {
    P->Set.Rejected++;
    return EFI_SUCCESS;
  }

  K3_DISPLAY_MODE  Entry;

  ZeroMem (&Entry, sizeof (Entry));
  Entry.AdvertisedClockKhz = M.Clock;
  UINT64      Requested = (UINT64)M.Clock * 1000;
  EFI_STATUS  S         = P->Select (P->Priv, Requested, &Entry.PixelClock);

  if (S == EFI_UNSUPPORTED) {
    P->Set.Rejected++;
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (S)) {
    return S;
  }

  if ((Entry.PixelClock.RequestedHz != Requested) || !Entry.PixelClock.ActualHz ||
      (Entry.PixelClock.ActualHz > 750000000))
  {
    return EFI_COMPROMISED_DATA;
  }

  Entry.Adjusted = Entry.PixelClock.ActualHz != Requested;
  if (Entry.Adjusted && (!P->Continuous || !InRange (P, &M, Entry.PixelClock.ActualHz))) {
    P->Set.Rejected++;
    return EFI_SUCCESS;
  }

  M.Clock = (UINT32)((Entry.PixelClock.ActualHz + 500) / 1000);
  UINT64  Demand = (UINT64)M.Clock * 24, Serial = (UINT64)P->Rate * P->Lanes;

  if ((Entry.PixelClock.ActualHz * 24 > Serial * 800) || ((Demand * 800 + Serial / 2) / Serial >= 640)) {
    P->Set.Rejected++;
    return EFI_SUCCESS;
  }

  Entry.Timing    = M;
  Entry.Preferred = Preferred;
  for (UINT32 I = 0; I < P->Set.Count; I++) {
    if (!CompareMem (&P->Set.Modes[I].Timing, &M, sizeof (M))) {
      P->Set.Modes[I].Preferred |= Preferred;
      return EFI_SUCCESS;
    }
  }

  if (P->Set.Count == K3_DISPLAY_MAX_MODES) {
    return EFI_BUFFER_TOO_SMALL;
  }

  P->Set.Modes[P->Set.Count++] = Entry;
  return EFI_SUCCESS;
}

#include "DmtModes.h"
STATIC EFI_STATUS
DmtTiming (
  PARSER  *P,
  UINT32  Width,
  UINT32  Height,
  UINT32  Refresh
  )
{
  BOOLEAN  Found = FALSE;

  for (UINTN I = 0; I < ARRAY_SIZE (Dmt); I++) {
    CONST SOC_DP_VIDEO_MODE  *M     = &Dmt[I];
    UINT64                   Pixels = (UINT64)M->Htotal * M->Vtotal;
    UINT32                   Hz     = ((UINT64)M->Clock * 1000 + Pixels / 2) / Pixels;
    if ((M->Hdisplay != Width) || (M->Vdisplay != Height) || (Hz != Refresh)) {
      continue;
    }

    Found = TRUE;
    EFI_STATUS  S = Add (P, *M, FALSE);
    if (EFI_ERROR (S)) {
      return S;
    }
  }

  if (!Found) {
    P->Set.UnhandledTiming = TRUE;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Legacy (
  PARSER       *P,
  CONST UINT8  *E
  )
{
  static CONST struct {
    UINT8     Byte, Bit;
    UINT16    W, H, R;
  } Established[] = {
    { 35, 5, 640,  480, 60 }, { 35, 3, 640,  480, 72 }, { 35, 2, 640,  480, 75 },
    { 35, 1, 800,  600, 56 }, { 35, 0, 800,  600, 60 }, { 36, 7, 800,  600, 72 }, { 36, 6, 800,  600,  75 },
    { 36, 3, 1024, 768, 60 }, { 36, 2, 1024, 768, 70 }, { 36, 1, 1024, 768, 75 }, { 36, 0, 1280, 1024, 75 },
    { 37, 7, 1152, 870, 75 }
  };
  UINT8  Handled[3] = { 0 };

  for (UINTN I = 0; I < ARRAY_SIZE (Established); I++) {
    UINT8  B = Established[I].Byte, Bit = 1U << Established[I].Bit;
    Handled[B - 35] |= Bit;
    if (!(E[B] & Bit)) {
      continue;
    }

    EFI_STATUS  S = DmtTiming (P, Established[I].W, Established[I].H, Established[I].R);
    if (EFI_ERROR (S)) {
      return S;
    }
  }

  for (UINTN I = 0; I < 3; I++) {
    if (E[35 + I] & ~Handled[I]) {
      P->Set.UnhandledTiming = TRUE;
    }
  }

  for (UINTN I = 38; I < 54; I += 2) {
    if ((E[I] == 1) && (E[I + 1] == 1)) {
      continue;
    }

    UINT32  W = (E[I] + 31U) * 8, H;
    switch (E[I + 1] >> 6) {
      case 0: H = W * 10 / 16;
        break;
      case 1: H = W * 3 / 4;
        break;
      case 2: H = W * 4 / 5;
        break;
      default: H = W * 9 / 16;
        break;
    }

    EFI_STATUS  S = DmtTiming (P, W, H, (E[I + 1] & 63) + 60);
    if (EFI_ERROR (S)) {
      return S;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Dtd (
  PARSER       *P,
  CONST UINT8  *D,
  BOOLEAN      Preferred
  )
{
  if (!U16 (D)) {
    return EFI_SUCCESS;
  }

  if ((D[17] & 0xe1) || ((D[17] & 0x18) != 0x18) || D[15] || D[16]) {
    P->Set.Rejected++;
    return EFI_SUCCESS;
  }

  SOC_DP_VIDEO_MODE  M;

  ZeroMem (&M, sizeof (M));
  M.Clock = U16 (D) * 10;

  M.Hdisplay   = D[2] | (UINT16)(D[4] & 0xf0) << 4;
  M.Htotal     = M.Hdisplay + (D[3] | (UINT16)(D[4] & 15) << 8);
  M.Vdisplay   = D[5] | (UINT16)(D[7] & 0xf0) << 4;
  M.Vtotal     = M.Vdisplay + (D[6] | (UINT16)(D[7] & 15) << 8);
  M.HsyncStart = M.Hdisplay + (D[8] | (UINT16)(D[11] & 0xc0) << 2);
  M.HsyncEnd   = M.HsyncStart + (D[9] | (UINT16)(D[11] & 0x30) << 4);
  M.VsyncStart = M.Vdisplay + ((D[10] >> 4) | (UINT16)(D[11] & 0xc) << 2);
  M.VsyncEnd   = M.VsyncStart + ((D[10] & 15) | (UINT16)(D[11] & 3) << 4);
  M.Flags      = ((D[17] & BIT1) ? SOC_DP_MODE_FLAG_PHSYNC : 0) | ((D[17] & BIT2) ? SOC_DP_MODE_FLAG_PVSYNC : 0);
  return Add (P, M, Preferred);
}

/* CTA timing values are data, in the public order clock/active/total/sync. */
typedef struct {
  UINT8                Vic;
  SOC_DP_VIDEO_MODE    M;
} CTA_MODE;
#define PP  (SOC_DP_MODE_FLAG_PHSYNC | SOC_DP_MODE_FLAG_PVSYNC)
STATIC CONST CTA_MODE  Cta[] = {
  { 1,  { 25175,  640,  800,  656,  752,  480,  525,  490,  492,  0  }
  },
  { 2,  { 27000,  720,  858,  736,  798,  480,  525,  489,  495,  0  }
  },
  { 3,  { 27000,  720,  858,  736,  798,  480,  525,  489,  495,  0  }
  },
  { 4,  { 74250,  1280, 1650, 1390, 1430, 720,  750,  725,  730,  PP }
  },
  { 16, { 148500, 1920, 2200, 2008, 2052, 1080, 1125, 1084, 1089, PP }
  },
  { 17, { 27000,  720,  864,  732,  796,  576,  625,  581,  586,  0  }
  },
  { 18, { 27000,  720,  864,  732,  796,  576,  625,  581,  586,  0  }
  },
  { 19, { 74250,  1280, 1980, 1720, 1760, 720,  750,  725,  730,  PP }
  },
  { 31, { 148500, 1920, 2640, 2448, 2492, 1080, 1125, 1084, 1089, PP }
  },
  { 32, { 74250,  1920, 2750, 2558, 2602, 1080, 1125, 1084, 1089, PP }
  },
  { 33, { 74250,  1920, 2640, 2448, 2492, 1080, 1125, 1084, 1089, PP }
  },
  { 34, { 74250,  1920, 2200, 2008, 2052, 1080, 1125, 1084, 1089, PP }
  },
  { 93, { 297000, 3840, 5500, 5116, 5204, 2160, 2250, 2168, 2178, PP }
  },
  { 94, { 297000, 3840, 5280, 4896, 4984, 2160, 2250, 2168, 2178, PP }
  },
  { 95, { 297000, 3840, 4400, 4016, 4104, 2160, 2250, 2168, 2178, PP }
  },
  { 96, { 594000, 3840, 5280, 4896, 4984, 2160, 2250, 2168, 2178, PP }
  },
  { 97, { 594000, 3840, 4400, 4016, 4104, 2160, 2250, 2168, 2178, PP }
  },
};
STATIC EFI_STATUS
CtaBlock (
  PARSER       *P,
  CONST UINT8  *B
  )
{
  UINTN  End = B[2];

  if (!End) {
    return EFI_SUCCESS;
  }

  if ((End < 4) || (End > 127)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 4; I < End;) {
    UINTN  N   = B[I] & 31;
    UINT8  Tag = B[I] >> 5;
    if (N > End - I - 1) {
      return EFI_COMPROMISED_DATA;
    }

    if (Tag == 2) {
      for (UINTN V = I + 1; V <= I + N; V++) {
        UINT8    Vic   = B[V] & 127;
        BOOLEAN  Found = FALSE;
        for (UINTN T = 0; T < ARRAY_SIZE (Cta); T++) {
          if (Cta[T].Vic != Vic) {
            continue;
          }

          Found = TRUE;
          EFI_STATUS  S = Add (P, Cta[T].M, (B[V] & 128) != 0);
          if (EFI_ERROR (S)) {
            return S;
          }
        }

        if (!Found) {
          P->Set.UnhandledTiming = TRUE;
        }
      }
    } else if ((Tag == 7) && N && ((B[I + 1] == 0xd) || (B[I + 1] == 0xe) || (B[I + 1] == 0xf))) {
      P->Set.UnhandledTiming = TRUE; /* Video preferences/Y420-only extensions are not RGB timings. */
    }

    I += N + 1;
  }

  UINTN  Index = 0;

  for (UINTN I = End; I + 18 <= 127; I += 18) {
    EFI_STATUS  S = Dtd (P, B + I, Index++ < (B[3] & 15));
    if (EFI_ERROR (S)) {
      return S;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
DisplayId (
  PARSER       *P,
  CONST UINT8  *B
  )
{
  UINTN  End = 5U + B[2];

  if ((End >= 127) || !Checksum (B + 1, End)) {
    return EFI_COMPROMISED_DATA;
  }

  if ((B[1] != 0x10) && (B[1] != 0x12) && (B[1] != 0x13) && (B[1] != 0x20)) {
    P->Set.UnhandledTiming = TRUE;
    return EFI_SUCCESS;
  }

  for (UINTN I = 5; I < End;) {
    if ((End - I < 3) || (B[I + 2] > End - I - 3)) {
      return EFI_COMPROMISED_DATA;
    }

    UINTN    N     = B[I + 2];
    BOOLEAN  Seven = B[I] == 0x22 && B[1] == 0x20;
    if (((B[I] == 3) && (B[1] < 0x20)) || Seven) {
      if (N % 20) {
        return EFI_COMPROMISED_DATA;
      }

      for (UINTN O = I + 3; O < I + 3 + N; O += 20) {
        CONST UINT8  *D = B + O;
        if (D[3] & 0x70) {
          P->Set.Rejected++;
          continue;
        }

        UINT32  Clock = (D[0] | (UINT32)D[1] << 8 | (UINT32)D[2] << 16) + 1;
        UINT32  H = U16 (D + 4) + 1, Hb = U16 (D + 6) + 1, Ho = (U16 (D + 8) & 0x7fff) + 1, Hw = U16 (D + 10) + 1;
        UINT32  V = U16 (D + 12) + 1, Vb = U16 (D + 14) + 1, Vo = (U16 (D + 16) & 0x7fff) + 1, Vw = U16 (D + 18) + 1;
        if ((H + Hb > 65535) || (H + Ho + Hw > 65535) || (V + Vb > 65535) || (V + Vo + Vw > 65535)) {
          P->Set.Rejected++;
          continue;
        }

        SOC_DP_VIDEO_MODE  M = {
          Seven ? Clock : Clock * 10, H, H + Hb, H + Ho, H + Ho + Hw, V, V + Vb, V + Vo, V + Vo + Vw,
          ((D[9] & 128) ? SOC_DP_MODE_FLAG_PHSYNC : 0) | ((D[17] & 128) ? SOC_DP_MODE_FLAG_PVSYNC : 0)
        };
        EFI_STATUS         S = Add (P, M, (D[3] & 128) != 0);
        if (EFI_ERROR (S)) {
          return S;
        }
      }
    } else {
      P->Set.UnhandledTiming = TRUE;
    }

    I += N + 3;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3DisplayModes (
  CONST UINT8           *E,
  UINTN                 Bytes,
  UINT32                Rate,
  UINT32                Lanes,
  K3_MODE_CLOCK_SELECT  Select,
  VOID                  *Priv,
  K3_DISPLAY_MODE_SET   *Out
  )
{
  static CONST UINT8  Header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };

  if (!E || !Out || !Select || (Bytes < 128) || (Bytes > 32768) || Bytes % 128 ||
      ((Lanes != 1) && (Lanes != 2) && (Lanes != 4)) ||
      ((Rate != 1620000) && (Rate != 2700000) && (Rate != 5400000)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (CompareMem (E, Header, 8) || (Bytes != (UINTN)(E[126] + 1) * 128)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 0; I < Bytes; I += 128) {
    if (!Checksum (E + I, 128)) {
      return EFI_COMPROMISED_DATA;
    }
  }

  for (UINTN I = 128; I < Bytes; I += 128) {
    if (!ExtensionValid (E + I)) {
      return EFI_COMPROMISED_DATA;
    }
  }

  if ((E[18] != 1) || ((E[19] != 3) && (E[19] != 4)) || !(E[20] & 128)) {
    return EFI_UNSUPPORTED;
  }

  PARSER  P;

  ZeroMem (&P, sizeof (P));
  P.Rate   = Rate;
  P.Lanes  = Lanes;
  P.Select = Select;
  P.Priv   = Priv;

  P.Continuous = E[19] == 4 && (E[24] & BIT0);
  BOOLEAN  RangeSeen = FALSE;

  for (UINTN I = 54; I < 126; I += 18) {
    CONST UINT8  *D = E + I;
    if (U16 (D) || D[2] || (D[3] != 0xfd)) {
      continue;
    }

    if (RangeSeen) {
      return EFI_COMPROMISED_DATA;
    }

    RangeSeen        = TRUE;
    P.Range.Vmin     = D[5] + ((E[19] == 4 && (D[4] & 1)) ? 255 : 0);
    P.Range.Vmax     = D[6] + ((E[19] == 4 && (D[4] & 2)) ? 255 : 0);
    P.Range.Hmin     = D[7] + ((E[19] == 4 && (D[4] & 4)) ? 255 : 0);
    P.Range.Hmax     = D[8] + ((E[19] == 4 && (D[4] & 8)) ? 255 : 0);
    P.Range.MaxClock = (UINT64)D[9] * 10000000;
    if (D[10] == 4) {
      UINT64  Offset = (UINT64)(D[12] >> 2) * 250000;
      if (Offset >= P.Range.MaxClock) {
        continue;
      }

      P.Range.MaxClock -= Offset;
      P.Range.Width     = 8 * (D[13] | (UINT32)(D[12] & 3) << 8);
    }

    P.Range.Valid = D[9] && D[9] != 255 && P.Range.Vmin && P.Range.Vmin <= P.Range.Vmax &&
                    P.Range.Hmin && P.Range.Hmin <= P.Range.Hmax &&
                    (D[10] == 0 || D[10] == 1 || D[10] == 2 || D[10] == 4);
  }

  for (UINTN I = 54; I < 126; I += 18) {
    EFI_STATUS  S = Dtd (&P, E + I, I == 54 && (E[24] & BIT1));
    if (EFI_ERROR (S)) {
      return S;
    }
  }

  for (UINTN I = 128; I < Bytes; I += 128) {
    EFI_STATUS  S = EFI_SUCCESS;
    if (E[I] == 2) {
      S = CtaBlock (&P, E + I);
    } else if (E[I] == 0x70) {
      S = DisplayId (&P, E + I);
    } else {
      P.Set.UnhandledTiming = TRUE;
    }

    if (EFI_ERROR (S)) {
      return S;
    }
  }

  EFI_STATUS  LegacyStatus = Legacy (&P, E);

  if (EFI_ERROR (LegacyStatus)) {
    return LegacyStatus;
  }

  for (UINT32 I = 1; I < P.Set.Count; I++) {
    if (P.Set.Modes[I].Preferred && !P.Set.Modes[0].Preferred) {
      K3_DISPLAY_MODE  T = P.Set.Modes[0];
      P.Set.Modes[0] = P.Set.Modes[I];
      P.Set.Modes[I] = T;
    }
  }

  if (!P.Set.Count) {
    return EFI_UNSUPPORTED;
  }

  *Out = P.Set;
  return EFI_SUCCESS;
}

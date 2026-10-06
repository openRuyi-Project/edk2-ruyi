/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_GOP_LIB_H
#define K3_GOP_LIB_H
#include <Uefi.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Library/K3DisplayModeLib.h>
#include <Library/K3DisplaySurfaceLib.h>
typedef struct K3_GOP K3_GOP;
typedef struct {
  /* Start must apply Clock.ActualHz/Timing and pin Surface via the real
     scan engine. Stop must prove DMA drained and release that pin. Check
     must inspect actual device/link readiness, including no-op SetMode.
     EFI_NO_MEDIA means the link is temporarily unavailable with all DMA
     resources retained; subsequent drawing retries Check. Other errors
     remain fatal until explicit cleanup. */
  EFI_STATUS (*Start)(
    VOID                   *Priv,
    K3_DISPLAY_SURFACE     *Surface,
    CONST K3_DISPLAY_MODE  *Mode
    );
  EFI_STATUS (*Stop)(
    VOID  *Priv
    );
  EFI_STATUS (*Check)(
    VOID  *Priv
    );
} K3_GOP_OPS;
EFI_STATUS
K3GopCreate (
  CONST K3_DISPLAY_MODE_SET  *Modes,
  CONST UINT8                *Edid,
  UINTN                      Bytes,
  CONST K3_GOP_OPS           *Ops,
  VOID                       *Priv,
  K3_GOP                     **Context
  );

/* Returns the standard GOP interface; caller publishes only after a real
   successful initial SetMode. The library does not install handles. */
EFI_GRAPHICS_OUTPUT_PROTOCOL *
K3GopProtocol (
  K3_GOP  *Context
  );

VOID
K3GopEdid (
  K3_GOP                        *Context,
  EFI_EDID_DISCOVERED_PROTOCOL  **Discovered,
  EFI_EDID_ACTIVE_PROTOCOL      **Active
  );

/* Stop/free may fail and retain the complete context for retry. */
EFI_STATUS
K3GopDestroy (
  K3_GOP  *Context
  );

#endif

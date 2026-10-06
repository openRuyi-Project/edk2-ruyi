/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SPACEMIT_CLOCK_PROTOCOL_H
#define SPACEMIT_CLOCK_PROTOCOL_H

#include <Uefi.h>

typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_RESOLVE)(
  CONST VOID *Fdt,
  INT32 Consumer,
  CONST CHAR8 *Name,
  BOOLEAN Reset,
  UINT32 *Id
  );

typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_ROUND_RATE)(
  UINT32 Id,
  UINT64 Requested,
  UINT64 *Actual,
  UINT32 *Parent,
  UINT32 *Divider
  );

// Quiesce the consumer first; FC can temporarily enable the leaf clock.
// On failure, Enable (Id, FALSE) releases references once FC has completed.
typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_SET_RATE)(
  UINT32 Id,
  UINT64 Requested,
  UINT64 *Actual
  );

typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_GET_RATE)(
  UINT32 Id,
  UINT64 *Rate
  );

typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_ENABLE)(
  UINT32 Id,
  BOOLEAN Enable
  );

typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_RESET)(
  UINT32 Id,
  BOOLEAN Assert
  );

typedef
EFI_STATUS
(EFIAPI *SPACEMIT_CLOCK_SET_PARENT)(
  UINT32 Id,
  UINT32 Parent
  );

typedef
BOOLEAN
(EFIAPI *SPACEMIT_CLOCK_IS_ENABLED)(
  UINT32 Id
  );

typedef
BOOLEAN
(EFIAPI *SPACEMIT_CLOCK_IS_RESET_ASSERTED)(
  UINT32 Id
  );

typedef struct {
  UINT32                              Revision;
  SPACEMIT_CLOCK_RESOLVE              Resolve;
  SPACEMIT_CLOCK_ROUND_RATE           RoundRate;
  SPACEMIT_CLOCK_SET_RATE             SetRate;
  SPACEMIT_CLOCK_GET_RATE             GetRate;
  SPACEMIT_CLOCK_ENABLE               Enable;
  SPACEMIT_CLOCK_RESET                Reset;
  SPACEMIT_CLOCK_SET_PARENT           SetParent;
  SPACEMIT_CLOCK_IS_ENABLED           IsEnabled;
  SPACEMIT_CLOCK_IS_RESET_ASSERTED    IsResetAsserted;
} SPACEMIT_CLOCK_PROTOCOL;

extern EFI_GUID  gSpacemitClockProtocolGuid;

#endif

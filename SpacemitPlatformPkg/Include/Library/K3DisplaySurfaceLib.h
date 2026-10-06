/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_DISPLAY_SURFACE_LIB_H
#define K3_DISPLAY_SURFACE_LIB_H
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/FrameBufferBltLib.h>
#define K3_DPU_LINEAR_MAX  0x3ffffffffULL
typedef struct {
  VOID                                    *Buffer, *Mapping;
  FRAME_BUFFER_CONFIGURE                  *Blt;
  UINTN                                   Pages, Bytes, MappedBytes, Users;
  EFI_PHYSICAL_ADDRESS                    Device;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    Info;
  EFI_STATUS                              CleanupError;
} K3_DISPLAY_SURFACE;

/* Initialize a zeroed object. Failure may retain resources if cleanup fails;
   inspect/retry Free instead of discarding the object. Pages are runtime data
   and require the platform's actual uncached common-buffer policy. */
EFI_STATUS
K3SurfaceCreate (
  K3_DISPLAY_SURFACE  *Surface,
  UINT32              Width,
  UINT32              Height
  );

EFI_STATUS
K3SurfaceFree (
  K3_DISPLAY_SURFACE  *Surface
  );

EFI_STATUS
K3SurfaceBlt (
  K3_DISPLAY_SURFACE                 *Surface,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *Buffer,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  Operation,
  UINTN                              SourceX,
  UINTN                              SourceY,
  UINTN                              DestinationX,
  UINTN                              DestinationY,
  UINTN                              Width,
  UINTN                              Height,
  UINTN                              Delta
  );

#endif

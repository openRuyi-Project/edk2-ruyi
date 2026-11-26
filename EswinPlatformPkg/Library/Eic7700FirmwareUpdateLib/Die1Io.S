/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Die1Io.h"

.text
.balign 4
.option push
.option norelax
.option norvc
.global D560WorkerStart
.global D560WorkerEnd
// SBI starts this hart with paging and interrupts disabled. This routine is
// copied into the shared allocation and uses no stack, globals or EFI services.
D560WorkerStart:
  fence.i
  mv s0, a1
  lla t0, .Lfault
  csrw stvec, t0
  li t0, 1
  fence rw, rw
  sw t0, D560_READY(s0)
.Lidle:
  lw t0, D560_STOP(s0)
  bnez t0, .Lstop
  lw s1, D560_REQ(s0)
  lw t0, D560_ACK(s0)
  beq t0, s1, .Lidle
  fence r, rw
  lw t0, D560_REGION(s0)
  lwu t1, D560_OFFSET(s0)
  lwu t2, D560_WORDS(s0)
  andi t3, t1, 3
  bnez t3, .Lfault
  beqz t2, .Lfault
  li t3, D560_WINDOW
  beq t0, t3, .Lwindow
  li t3, 1
  bne t2, t3, .Lfault
  beqz t0, .Lspi
  li t3, D560_CLOCKS
  bne t0, t3, .Lfault
  li t4, D560_SPI1_CRG
  li t3, 0x104
  beq t1, t3, .Laccess
  li t3, 0x108
  beq t1, t3, .Laccess
  li t3, 0x1e4
  beq t1, t3, .Laccess
  li t3, 0x490
  bne t1, t3, .Lfault
  j .Laccess
.Lspi:
  li t4, D560_SPI1
  li t3, 0x34
  bltu t3, t1, .Lfault
  j .Laccess
.Lwindow:
  li t4, D560_SPI1_DATA
  slli t3, t2, 2
  add t3, t3, t1
  li t5, 256
  bltu t5, t3, .Lfault
.Laccess:
  add t4, t4, t1
  addi t5, s0, D560_BUFFER
  lw t0, D560_WRITE(s0)
  fence iorw, iorw
  beqz t0, .Lread
.Lwrite:
  lw t3, 0(t5)
  sw t3, 0(t4)
  addi t5, t5, 4
  addi t4, t4, 4
  addi t2, t2, -1
  bnez t2, .Lwrite
  j .Lack
.Lread:
  lw t3, 0(t4)
  sw t3, 0(t5)
  addi t5, t5, 4
  addi t4, t4, 4
  addi t2, t2, -1
  bnez t2, .Lread
.Lack:
  fence iorw, iorw
  sw s1, D560_ACK(s0)
  j .Lidle
.balign 4
.Lfault:
  li t0, 2
  sw t0, D560_READY(s0)
.Lstop:
  fence rw, rw
  li a7, D560_SBI_HSM
  li a6, 1
  ecall
.Lpark:
  wfi
  j .Lpark
D560WorkerEnd:
.option pop

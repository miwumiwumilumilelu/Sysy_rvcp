[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [DEBUG] Phase1 Phi: dst=v0, in block=bb2
  [DEBUG] Phase1 Phi: dst=v1, in block=bb4
  [DEBUG] Phi (Phase2): dst=v0, in block=bb2
  [DEBUG] Phi (Phase2): dst=v1, in block=bb4

[Debug] ----- Running Phi Elimination -----
  [PhiElim] findPos called for block bb4 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v1)
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb4
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: mv v0, v1
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] findPos called for block bb6 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=61
    [1] opc=23
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb6
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v1, v17
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] findPos called for block bb0 with 1 instructions
  [PhiElim] Instructions in block:
    [0] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb0
  [PhiElim] Block has 1 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v0, 7
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 0
  [PhiElim] Block now has 2 instructions after insertion
  [PhiElim] findPos called for block bb5 with 4 instructions
  [PhiElim] Instructions in block:
    [0] opc=61
    [1] opc=22
    [2] opc=5
    [3] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb5
  [PhiElim] Block has 4 instructions before insertion
  [PhiElim] Generated simple copy: mv v1, v16
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 0 [0,33] isFloat=0 -> 10
  [Alloc] vreg 3 [2,33] isFloat=0 -> 11
  [Alloc] vreg 2 [3,33] isFloat=0 -> 12
  [Alloc] vreg 5 [7,33] isFloat=0 -> 13
  [Alloc] vreg 4 [8,33] isFloat=0 -> 14
  [Alloc] vreg 7 [11,12] isFloat=0 -> 15
  [Alloc] vreg 6 [12,13] isFloat=0 -> 16
  [Alloc] vreg 8 [13,15] isFloat=0 -> 15
  [Alloc] vreg 9 [14,15] isFloat=0 -> 16
  [Alloc] vreg 11 [16,17] isFloat=0 -> 15
  [Alloc] vreg 10 [17,18] isFloat=0 -> 16
  [Alloc] vreg 13 [18,19] isFloat=0 -> 15
  [Alloc] vreg 12 [19,20] isFloat=0 -> 16
  [Alloc] vreg 1 [22,33] isFloat=0 -> 15
  [Alloc] vreg 15 [25,26] isFloat=0 -> 16
  [Alloc] vreg 14 [26,27] isFloat=0 -> 17
  [Alloc] vreg 16 [27,28] isFloat=0 -> 16
  [Alloc] vreg 18 [30,31] isFloat=0 -> 16
  [Alloc] vreg 17 [31,32] isFloat=0 -> 17

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 15, 15

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

.bss
  .align 4
  .globl count
count:
  .space 4

  .text

  .globl main
main:
    li a0, 7
    j .Lbb2
.Lbb1:
    la a1, count
    lw a2, 0(a1)
    mv a0, a2
    ret
.Lbb2:
    addi a3, a0, -1
    sltu a4, zero, a3
    bne a4, zero, .Lbb3
    j .Lbb1
.Lbb3:
    la a5, count
    lw a6, 0(a5)
    addi a5, a6, 1
    la a6, count
    sw a5, 0(a6)
    li a5, 2
    remw a6, a0, a5
    addi a5, a6, 0
    sltiu a6, a5, 1
    bne a6, zero, .Lbb6
    j .Lbb5
.Lbb4:
    mv a0, a5
    j .Lbb2
.Lbb5:
    li a6, 3
    mulw a7, a0, a6
    addi a6, a7, 1
    mv a5, a6
    j .Lbb4
.Lbb6:
    li a6, 2
    divw a7, a0, a6
    mv a5, a7
    j .Lbb4

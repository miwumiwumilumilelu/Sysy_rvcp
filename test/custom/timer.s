[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [DEBUG] Phase1 Phi: dst=v0, in block=bb2
  [DEBUG] Phase1 Phi: dst=v1, in block=bb2
  [DEBUG] Phase1 Phi: dst=v2, in block=bb4
  [DEBUG] Phi (Phase2): dst=v0, in block=bb2
  [DEBUG] Phi (Phase2): dst=v1, in block=bb2
  [DEBUG] Phi (Phase2): dst=v2, in block=bb4

[Debug] ----- Running Phi Elimination -----
  [PhiElim] findPos called for block bb4 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v2)
    [1] opc=5
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb4
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v1, v9
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] Generated simple copy: mv v0, v2
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion
  [PhiElim] findPos called for block bb0 with 1 instructions
  [PhiElim] Instructions in block:
    [0] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb0
  [PhiElim] Block has 1 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v1, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 0
  [PhiElim] Block now has 2 instructions after insertion
  [PhiElim] Generated simple copy: fmv.s v0, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] findPos called for block bb6 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=0
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb6
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v11
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] findPos called for block bb5 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=1
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb5
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v10
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 1 [0,27] isFloat=0 -> 10
  [Alloc] vreg 0 [1,27] isFloat=0 -> 11
  [Alloc] vreg 4 [7,27] isFloat=0 -> 12
  [Alloc] vreg 3 [8,27] isFloat=0 -> 13
  [Alloc] vreg 6 [11,12] isFloat=0 -> 14
  [Alloc] vreg 5 [12,13] isFloat=0 -> 15
  [Alloc] vreg 8 [13,14] isFloat=0 -> 14
  [Alloc] vreg 7 [14,15] isFloat=0 -> 15
  [Alloc] vreg 2 [17,27] isFloat=0 -> 14
  [Alloc] vreg 9 [18,27] isFloat=0 -> 15
  [Alloc] vreg 10 [22,23] isFloat=0 -> 16
  [Alloc] vreg 11 [25,26] isFloat=0 -> 16
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 11, 11
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 14, 14

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl loop
loop:
    li a0, 0
    li a1, 0
    j .Lbb2
.Lbb1:
    mv a0, a1
    ret
.Lbb2:
    li a2, 10000
    slt a3, a0, a2
    bne a3, zero, .Lbb3
    j .Lbb1
.Lbb3:
    li a4, 2
    remw a5, a0, a4
    addi a4, a5, 0
    sltiu a5, a4, 1
    bne a5, zero, .Lbb6
    j .Lbb5
.Lbb4:
    addi a5, a0, 1
    mv a0, a5
    mv a1, a4
    j .Lbb2
.Lbb5:
    sub a6, a1, a0
    mv a4, a6
    j .Lbb4
.Lbb6:
    add a6, a1, a0
    mv a4, a6
    j .Lbb4

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 0
    call _sysy_starttime
    call loop
    li a0, 0
    call _sysy_stoptime
    call putint
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [DEBUG] Phase1 Phi: dst=v0, in block=bb2
  [DEBUG] Phase1 Phi: dst=v1, in block=bb2
  [DEBUG] Phi (Phase2): dst=v0, in block=bb2
  [DEBUG] Phi (Phase2): dst=v1, in block=bb2

[Debug] ----- Running Phi Elimination -----
  [PhiElim] findPos called for block bb0 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=46
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb0
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v1, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] Generated simple copy: fmv.s v0, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] findPos called for block bb3 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=0
    [1] opc=5
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb3
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v1, v4
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] Generated simple copy: mv v0, v3
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [1,17] isFloat=0 -> 10
  [Alloc] vreg 0 [2,17] isFloat=0 -> 11
  [Alloc] vreg 2 [10,17] isFloat=0 -> 12
  [Alloc] vreg 3 [13,16] isFloat=0 -> 13
  [Alloc] vreg 4 [14,15] isFloat=0 -> 14

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 11, 11
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    call getint
    li a0, 0
    li a1, 0
    j .Lbb2
.Lbb1:
    mv a0, a1
    call putint
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret
.Lbb2:
    slt a2, a0, a0
    bne a2, zero, .Lbb3
    j .Lbb1
.Lbb3:
    add a3, a1, a0
    addi a4, a0, 1
    mv a0, a4
    mv a1, a3
    j .Lbb2

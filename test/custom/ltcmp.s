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
  [PhiElim] findPos called for block bb4 with 6 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v2)
    [1] opc=61
    [2] opc=22
    [3] opc=0
    [4] opc=5
    [5] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb4
  [PhiElim] Block has 6 instructions before insertion
  [PhiElim] Generated simple copy: mv v1, v8
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 5
  [PhiElim] Block now has 7 instructions after insertion
  [PhiElim] Generated simple copy: mv v0, v7
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 6
  [PhiElim] Block now has 8 instructions after insertion
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
  [PhiElim] findPos called for block bb5 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=0
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb5
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v9
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] findPos called for block bb3 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=35
    [1] opc=40
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb3
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [1,27] isFloat=0 -> 10
  [Alloc] vreg 0 [2,27] isFloat=0 -> 11
  [Alloc] vreg 3 [10,27] isFloat=0 -> 12
  [Alloc] vreg 4 [13,14] isFloat=0 -> 13
  [Alloc] vreg 2 [15,27] isFloat=0 -> 13
  [Alloc] vreg 6 [18,27] isFloat=0 -> 14
  [Alloc] vreg 5 [19,27] isFloat=0 -> 15
  [Alloc] vreg 7 [20,27] isFloat=0 -> 16
  [Alloc] vreg 8 [21,27] isFloat=0 -> 17
  [Alloc] vreg 9 [25,26] isFloat=0 -> 5

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 11, 11
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 13, 13

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
    slti a3, a0, 30
    bne a3, zero, .Lbb5
    mv a3, a1
    j .Lbb4
.Lbb4:
    li a4, 2
    mulw a5, a0, a4
    add a6, a3, a5
    addi a7, a0, 1
    mv a0, a7
    mv a1, a6
    j .Lbb2
.Lbb5:
    add t0, a1, a0
    mv a3, t0
    j .Lbb4

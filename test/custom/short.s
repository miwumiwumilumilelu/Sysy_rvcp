[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [DEBUG] Phase1 Phi: dst=v0, in block=bb2
  [DEBUG] Phase1 Phi: dst=v1, in block=bb3
  [DEBUG] Phase1 Phi: dst=v2, in block=bb6
  [DEBUG] Phase1 Phi: dst=v3, in block=bb8
  [DEBUG] Phi (Phase2): dst=v0, in block=bb2
  [DEBUG] Phi (Phase2): dst=v1, in block=bb3
  [DEBUG] Phi (Phase2): dst=v2, in block=bb6
  [DEBUG] Phi (Phase2): dst=v3, in block=bb8

[Debug] ----- Running Phi Elimination -----
  [PhiElim] findPos called for block bb2 with 4 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v0)
    [1] opc=34
    [2] opc=40
    [3] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb2
  [PhiElim] Block has 4 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v1, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion
  [PhiElim] findPos called for block bb1 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=46
    [1] opc=34
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb1
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v0, v5
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] findPos called for block bb3 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v1)
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb3
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v1
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] findPos called for block bb9 with 2 instructions
  [PhiElim] Instructions in block:
    [0] opc=5
    [1] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb9
  [PhiElim] Block has 2 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v10
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] findPos called for block bb0 with 4 instructions
  [PhiElim] Instructions in block:
    [0] opc=46
    [1] opc=34
    [2] opc=40
    [3] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb0
  [PhiElim] Block has 4 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v0, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion
  [PhiElim] findPos called for block bb4 with 1 instructions
  [PhiElim] Instructions in block:
    [0] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb4
  [PhiElim] Block has 1 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v1, 1
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 0
  [PhiElim] Block now has 2 instructions after insertion
  [PhiElim] findPos called for block bb6 with 5 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v2)
    [1] opc=46
    [2] opc=34
    [3] opc=40
    [4] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb6
  [PhiElim] Block has 5 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v3, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 4
  [PhiElim] Block now has 6 instructions after insertion
  [PhiElim] findPos called for block bb7 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=46
    [1] opc=34
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 1 simple copies in block bb7
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v3, v8
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 4 [1,2] isFloat=0 -> 11
  [Alloc] vreg 0 [3,10] isFloat=0 -> 10
  [Alloc] vreg 5 [6,7] isFloat=0 -> 11
  [Alloc] vreg 6 [10,11] isFloat=0 -> 11
  [Alloc] vreg 1 [12,18] isFloat=0 -> 10
  [Alloc] vreg 2 [15,37] isFloat=0 -> 11
  [Alloc] vreg 7 [23,37] isFloat=0 -> 12
  [Alloc] vreg 3 [25,37] isFloat=0 -> 10
  [Alloc] vreg 8 [28,29] isFloat=0 -> 13
  [Alloc] vreg 9 [32,37] isFloat=0 -> 13
  [Alloc] vreg 10 [35,36] isFloat=0 -> 14

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 11, 11
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl f
f:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 1
    call putint
    li a0, 1
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

  .globl g
g:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 2
    call putint
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    call f
    sltu a1, zero, a0
    bne a1, zero, .Lbb1
    li a0, 0
    j .Lbb2
.Lbb1:
    call g
    sltu a1, zero, a0
    mv a0, a1
    j .Lbb2
.Lbb2:
    sltu a1, zero, a0
    bne a1, zero, .Lbb4
    li a0, 0
    j .Lbb3
.Lbb3:
    mv a1, a0
    j .Lbb6
.Lbb4:
    li a0, 1
    j .Lbb3
.Lbb5:
    mv a0, a1
    ld ra, 0(sp)
    addi sp, sp, 16
    ret
.Lbb6:
    call g
    sltu a2, zero, a0
    bne a2, zero, .Lbb7
    li a0, 0
    j .Lbb8
.Lbb7:
    call f
    sltu a3, zero, a0
    mv a0, a3
    j .Lbb8
.Lbb8:
    sltu a3, zero, a0
    bne a3, zero, .Lbb9
    j .Lbb5
.Lbb9:
    addi a4, a1, 1
    mv a1, a4
    j .Lbb6

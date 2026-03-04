[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [DEBUG] Phase1 Phi: dst=v0, in block=bb2
  [DEBUG] Phase1 Phi: dst=v1, in block=bb2
  [DEBUG] Phase1 Phi: dst=v2, in block=bb2
  [DEBUG] Phase1 Phi: dst=v3, in block=bb6
  [DEBUG] Phase1 Phi: dst=v4, in block=bb6
  [DEBUG] Phi (Phase2): dst=v0, in block=bb2
  [DEBUG] Phi (Phase2): dst=v1, in block=bb2
  [DEBUG] Phi (Phase2): dst=v2, in block=bb2
  [DEBUG] Phi (Phase2): dst=v3, in block=bb6
  [DEBUG] Phi (Phase2): dst=v4, in block=bb6

[Debug] ----- Running Phi Elimination -----
  [PhiElim] findPos called for block bb8 with 3 instructions
  [PhiElim] Instructions in block:
    [0] opc=5
    [1] opc=5
    [2] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb8
  [PhiElim] Block has 3 instructions before insertion
  [PhiElim] Generated simple copy: mv v4, v15
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] Generated simple copy: mv v3, v16
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion
  [PhiElim] findPos called for block bb4 with 4 instructions
  [PhiElim] Instructions in block:
    [0] opc=5
    [1] opc=36
    [2] opc=40
    [3] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 0 simple copies in block bb4
  [PhiElim] Block has 4 instructions before insertion
  [PhiElim] Processing 5 cyclic copies in block bb4
  [PhiElim] Created temporary vreg 17 for dst v2
  [PhiElim] Created temporary vreg 18 for dst v1
  [PhiElim] Created temporary vreg 19 for dst v0
  [PhiElim] Created temporary vreg 20 for dst v4
  [PhiElim] Created temporary vreg 21 for dst v3
  [PhiElim] Generated save: mv v17, v2
  [PhiElim] Generated save: mv v18, v1
  [PhiElim] Generated save: mv v19, v0
  [PhiElim] Generated save: mv v20, v4
  [PhiElim] Generated save: mv v21, v3
  [PhiElim] Generated copy: mv v2, v17
  [PhiElim] Generated copy: mv v1, v18
  [PhiElim] Generated copy: mv v0, v19
  [PhiElim] Generated copy: mv v4, v18
  [PhiElim] Generated copy: mv v3, v19
  [PhiElim] findPos called for block bb0 with 1 instructions
  [PhiElim] Instructions in block:
    [0] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 3 simple copies in block bb0
  [PhiElim] Block has 1 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v2, 1
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 0
  [PhiElim] Block now has 2 instructions after insertion
  [PhiElim] Generated simple copy: fmv.s v1, 10
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 1
  [PhiElim] Block now has 3 instructions after insertion
  [PhiElim] Generated simple copy: fmv.s v0, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 2
  [PhiElim] Block now has 4 instructions after insertion
  [PhiElim] findPos called for block bb5 with 4 instructions
  [PhiElim] Instructions in block:
    [0] opc=5
    [1] opc=5
    [2] opc=5
    [3] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 3 simple copies in block bb5
  [PhiElim] Block has 4 instructions before insertion
  [PhiElim] Generated simple copy: mv v2, v12
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 3
  [PhiElim] Block now has 5 instructions after insertion
  [PhiElim] Generated simple copy: mv v1, v11
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 4
  [PhiElim] Block now has 6 instructions after insertion
  [PhiElim] Generated simple copy: mv v0, v10
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 5
  [PhiElim] Block now has 7 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [BuildIntervals] WARNING: vreg 4 is used but not defined in instruction opc=63 in block bb4
  [BuildIntervals] WARNING: vreg 3 is used but not defined in instruction opc=63 in block bb4
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 2 [0,49] isFloat=0 -> 10
  [Alloc] vreg 1 [1,49] isFloat=0 -> 11
  [Alloc] vreg 0 [2,49] isFloat=0 -> 12
  [Alloc] vreg 5 [9,49] isFloat=0 -> 13
  [Alloc] vreg 7 [12,13] isFloat=0 -> 14
  [Alloc] vreg 6 [13,14] isFloat=0 -> 15
  [Alloc] vreg 9 [16,17] isFloat=0 -> 14
  [Alloc] vreg 8 [17,18] isFloat=0 -> 15
  [Alloc] vreg 17 [19,24] isFloat=0 -> 14
  [Alloc] vreg 18 [20,27] isFloat=0 -> 15
  [Alloc] vreg 19 [21,28] isFloat=0 -> 16
  [Alloc] vreg 20 [22,22] isFloat=0 -> 17
  [Alloc] vreg 21 [23,23] isFloat=0 -> 17
  [Alloc] vreg 4 [27,49] isFloat=0 -> 14
  [Alloc] vreg 3 [28,49] isFloat=0 -> 15
  [Alloc] vreg 10 [30,49] isFloat=0 -> 16
  [Alloc] vreg 11 [31,49] isFloat=0 -> 17
  [Alloc] vreg 12 [32,49] isFloat=0 -> 6
  [Alloc] vreg 13 [39,49] isFloat=0 -> 7
  [Alloc] vreg 14 [42,43] isFloat=0 -> 28
  [Alloc] vreg 15 [45,47] isFloat=0 -> 28
  [Alloc] vreg 16 [46,48] isFloat=0 -> 29

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 12, 12
  [Peephole] Eliminating redundant move: mv 11, 11
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 15, 15
  [Peephole] Eliminating redundant move: mv 14, 14

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl main
main:
    li a0, 1
    li a1, 10
    li a2, 0
    j .Lbb2
.Lbb1:
    mv a0, a2
    ret
.Lbb2:
    slti a3, a0, 10
    bne a3, zero, .Lbb3
    j .Lbb1
.Lbb3:
    li a4, 5
    slt a5, a4, a0
    bne a5, zero, .Lbb1
    j .Lbb4
.Lbb4:
    addi a4, a0, -7
    sltiu a5, a4, 1
    bne a5, zero, .Lbb2
    mv a4, a0
    mv a5, a1
    mv a6, a2
    mv a7, a4
    mv a7, a5
    mv a0, a4
    mv a1, a5
    mv a2, a6
    mv a4, a5
    mv a5, a6
    j .Lbb6
.Lbb5:
    addi a6, a5, 1
    addi a7, a0, 5
    addi t1, a0, 1
    mv a0, t1
    mv a1, a7
    mv a2, a6
    j .Lbb2
.Lbb6:
    slt t2, zero, a4
    bne t2, zero, .Lbb7
    j .Lbb5
.Lbb7:
    slti t3, a4, 5
    bne t3, zero, .Lbb5
    j .Lbb8
.Lbb8:
    addi t3, a4, -1
    addi t4, a5, 1
    mv a4, t3
    mv a5, t4
    j .Lbb6

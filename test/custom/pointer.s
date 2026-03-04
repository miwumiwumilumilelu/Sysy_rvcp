[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [DEBUG] Phase1 Phi: dst=v0, in block=bb1
  [DEBUG] Phase1 Phi: dst=v1, in block=bb1
  [DEBUG] Phase1 Phi: dst=v2, in block=bb4
  [DEBUG] Phase1 Phi: dst=v3, in block=bb4
  [DEBUG] Phi (Phase2): dst=v0, in block=bb1
  [DEBUG] Phi (Phase2): dst=v1, in block=bb1
  [DEBUG] Phi (Phase2): dst=v2, in block=bb4
  [DEBUG] Phi (Phase2): dst=v3, in block=bb4

[Debug] ----- Running Phi Elimination -----
  [PhiElim] findPos called for block bb0 with 15 instructions
  [PhiElim] Instructions in block:
    [0] opc=66
    [1] opc=63 (mv v5, v4)
    [2] opc=63 (mv v6, v5)
    [3] opc=61
    [4] opc=26
    [5] opc=5
    [6] opc=61
    [7] opc=26
    [8] opc=5
    [9] opc=26
    [10] opc=63 (mv v11, v4)
    [11] opc=5
    [12] opc=61
    [13] opc=26
    [14] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb0
  [PhiElim] Block has 15 instructions before insertion
  [PhiElim] Generated simple copy: fmv.s v1, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 14
  [PhiElim] Block now has 16 instructions after insertion
  [PhiElim] Generated simple copy: fmv.s v0, 0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 15
  [PhiElim] Block now has 17 instructions after insertion
  [PhiElim] findPos called for block bb1 with 5 instructions
  [PhiElim] Instructions in block:
    [0] opc=65 (PHI v1)
    [1] opc=65 (PHI v0)
    [2] opc=35
    [3] opc=40
    [4] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb1
  [PhiElim] Block has 5 instructions before insertion
  [PhiElim] Generated simple copy: mv v3, v1
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 4
  [PhiElim] Block now has 6 instructions after insertion
  [PhiElim] Generated simple copy: mv v2, v0
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 5
  [PhiElim] Block now has 7 instructions after insertion
  [PhiElim] findPos called for block bb5 with 8 instructions
  [PhiElim] Instructions in block:
    [0] opc=63 (mv v21, v4)
    [1] opc=5
    [2] opc=19
    [3] opc=0
    [4] opc=25
    [5] opc=0
    [6] opc=5
    [7] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb5
  [PhiElim] Block has 8 instructions before insertion
  [PhiElim] Generated simple copy: mv v3, v26
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 7
  [PhiElim] Block now has 9 instructions after insertion
  [PhiElim] Generated simple copy: mv v2, v27
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 8
  [PhiElim] Block now has 10 instructions after insertion
  [PhiElim] findPos called for block bb2 with 6 instructions
  [PhiElim] Instructions in block:
    [0] opc=19
    [1] opc=0
    [2] opc=25
    [3] opc=0
    [4] opc=5
    [5] opc=45 (j)
  [PhiElim] Last instruction opc=45
  [PhiElim] Found terminator, inserting before it
  [PhiElim] Processing 2 simple copies in block bb2
  [PhiElim] Block has 6 instructions before insertion
  [PhiElim] Generated simple copy: mv v1, v18
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 5
  [PhiElim] Block now has 7 instructions after insertion
  [PhiElim] Generated simple copy: mv v0, v19
  [PhiElim] Inserting before instruction opc=45
  [PhiElim] insertPos is at index 6
  [PhiElim] Block now has 8 instructions after insertion

[Debug] ----- Running Register Allocation -----
  [BuildIntervals] WARNING: vreg 4 is used but not defined in instruction opc=63 in block bb0
  [BuildIntervals] WARNING: vreg 4 is used but not defined in instruction opc=63 in block bb0
  [BuildIntervals] WARNING: vreg 4 is used but not defined in instruction opc=63 in block bb5
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 5 [1,8] isFloat=0 -> 10
  [Alloc] vreg 6 [2,4] isFloat=0 -> 11
  [Alloc] vreg 7 [3,4] isFloat=0 -> 12
  [Alloc] vreg 8 [5,7] isFloat=0 -> 11
  [Alloc] vreg 9 [6,7] isFloat=0 -> 12
  [Alloc] vreg 10 [8,9] isFloat=0 -> 11
  [Alloc] vreg 11 [10,11] isFloat=0 -> 10
  [Alloc] vreg 12 [11,13] isFloat=0 -> 11
  [Alloc] vreg 13 [12,13] isFloat=0 -> 10
  [Alloc] vreg 1 [14,31] isFloat=0 -> 10
  [Alloc] vreg 0 [15,31] isFloat=0 -> 11
  [Alloc] vreg 14 [19,31] isFloat=0 -> 12
  [Alloc] vreg 3 [21,48] isFloat=0 -> 13
  [Alloc] vreg 2 [22,48] isFloat=0 -> 14
  [Alloc] vreg 16 [24,25] isFloat=0 -> 15
  [Alloc] vreg 15 [25,26] isFloat=0 -> 16
  [Alloc] vreg 17 [26,27] isFloat=0 -> 15
  [Alloc] vreg 18 [27,29] isFloat=0 -> 16
  [Alloc] vreg 19 [28,30] isFloat=0 -> 15
  [Alloc] vreg 20 [36,48] isFloat=0 -> 10
  [Alloc] vreg 21 [39,42] isFloat=0 -> 11
  [Alloc] vreg 22 [40,41] isFloat=0 -> 12
  [Alloc] vreg 24 [41,42] isFloat=0 -> 15
  [Alloc] vreg 23 [42,43] isFloat=0 -> 12
  [Alloc] vreg 25 [43,44] isFloat=0 -> 11
  [Alloc] vreg 26 [44,46] isFloat=0 -> 12
  [Alloc] vreg 27 [45,47] isFloat=0 -> 11
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [1,29] isFloat=0 -> 10
  [Alloc] vreg 2 [2,4] isFloat=0 -> 11
  [Alloc] vreg 3 [3,4] isFloat=0 -> 12
  [Alloc] vreg 4 [5,7] isFloat=0 -> 11
  [Alloc] vreg 5 [6,7] isFloat=0 -> 12
  [Alloc] vreg 6 [8,10] isFloat=0 -> 11
  [Alloc] vreg 7 [9,10] isFloat=0 -> 12
  [Alloc] vreg 8 [11,13] isFloat=0 -> 11
  [Alloc] vreg 9 [12,13] isFloat=0 -> 12
  [Alloc] vreg 10 [14,16] isFloat=0 -> 11
  [Alloc] vreg 11 [15,16] isFloat=0 -> 12
  [Alloc] vreg 12 [17,19] isFloat=0 -> 11
  [Alloc] vreg 13 [18,19] isFloat=0 -> 12
  [Alloc] vreg 14 [20,22] isFloat=0 -> 11
  [Alloc] vreg 15 [21,22] isFloat=0 -> 12
  [Alloc] vreg 16 [23,25] isFloat=0 -> 11
  [Alloc] vreg 17 [24,25] isFloat=0 -> 12
  [Alloc] vreg 18 [26,28] isFloat=0 -> 11
  [Alloc] vreg 19 [27,28] isFloat=0 -> 12
  [Alloc] vreg 20 [29,31] isFloat=0 -> 11
  [Alloc] vreg 21 [30,31] isFloat=0 -> 10
  [Alloc] vreg 22 [32,33] isFloat=0 -> 10
  [Alloc] vreg 23 [33,34] isFloat=0 -> 11

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Folding addi+load/store: addi 11, 10, 8 + sw -> sw ..., 8(10)
  [Peephole] Eliminating redundant move: mv 11, 11
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 14, 14
  [Peephole] Eliminating redundant move: mv 13, 13

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl f
f:
    addi sp, sp, -16
    addi t0, sp, 0
    mv a0, t0
    mv a1, a0
    li a2, 11
    sw a2, 0(a1)
    addi a1, a0, 4
    li a2, 12
    sw a2, 0(a1)
    sw zero, 8(a0)
    addi t0, sp, 0
    mv a0, t0
    addi a1, a0, 8
    li a0, 13
    sw a0, 0(a1)
    li a0, 0
    li a1, 0
    j .Lbb1
.Lbb1:
    slti a2, a1, 10
    bne a2, zero, .Lbb2
    mv a3, a0
    mv a4, a1
    j .Lbb4
.Lbb2:
    slliw a5, a1, 2
    add a6, a0, a5
    lw a5, 0(a6)
    add a6, a0, a5
    addi a5, a1, 1
    mv a0, a6
    mv a1, a5
    j .Lbb1
.Lbb3:
    mv a0, a3
    addi sp, sp, 16
    ret
.Lbb4:
    slti a0, a4, 13
    bne a0, zero, .Lbb5
    j .Lbb3
.Lbb5:
    addi t0, sp, 0
    mv a1, t0
    addi a2, a4, -10
    slliw a5, a2, 2
    add a2, a1, a5
    lw a1, 0(a2)
    add a2, a3, a1
    addi a1, a4, 1
    mv a3, a2
    mv a4, a1
    j .Lbb4

  .globl main
main:
    addi sp, sp, -48
    sd ra, 40(sp)
    addi t0, sp, 8
    mv a0, t0
    mv a1, a0
    li a2, 1
    sw a2, 0(a1)
    addi a1, a0, 4
    li a2, 2
    sw a2, 0(a1)
    addi a1, a0, 8
    li a2, 3
    sw a2, 0(a1)
    addi a1, a0, 12
    li a2, 4
    sw a2, 0(a1)
    addi a1, a0, 16
    li a2, 5
    sw a2, 0(a1)
    addi a1, a0, 20
    li a2, 6
    sw a2, 0(a1)
    addi a1, a0, 24
    li a2, 7
    sw a2, 0(a1)
    addi a1, a0, 28
    li a2, 8
    sw a2, 0(a1)
    addi a1, a0, 32
    li a2, 9
    sw a2, 0(a1)
    addi a1, a0, 36
    li a0, 10
    sw a0, 0(a1)
    addi t0, sp, 8
    mv a0, t0
    mv a1, a0
    mv a0, a1
    call f
    ld ra, 40(sp)
    addi sp, sp, 48
    ret

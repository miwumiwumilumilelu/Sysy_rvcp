[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 0 [0,1] isFloat=0 -> 10
  [Alloc] vreg 1 [1,2] isFloat=1 -> 32f
  [Alloc] vreg 2 [2,3] isFloat=0 -> 10
  [Alloc] vreg 3 [5,6] isFloat=0 -> 10
  [Alloc] vreg 4 [6,7] isFloat=1 -> 32f
  [Alloc] vreg 5 [7,8] isFloat=0 -> 10
  [Alloc] vreg 6 [10,11] isFloat=0 -> 10
  [Alloc] vreg 7 [11,14] isFloat=1 -> 32f
  [Alloc] vreg 8 [12,13] isFloat=0 -> 10
  [Alloc] vreg 9 [13,14] isFloat=1 -> 33f
  [Alloc] vreg 10 [14,15] isFloat=1 -> 34f
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [1,6] isFloat=0 -> 10
  [Alloc] vreg 2 [2,5] isFloat=0 -> 11
  [Alloc] vreg 4 [3,4] isFloat=0 -> 12
  [Alloc] vreg 3 [4,5] isFloat=1 -> 32f
  [Alloc] vreg 5 [6,9] isFloat=0 -> 11
  [Alloc] vreg 7 [7,8] isFloat=0 -> 10
  [Alloc] vreg 6 [8,9] isFloat=1 -> 32f
  [Alloc] vreg 8 [10,11] isFloat=0 -> 10
  [Alloc] vreg 9 [11,12] isFloat=0 -> 11
  [Alloc] vreg 10 [12,17] isFloat=1 -> 32f
  [Alloc] vreg 11 [13,14] isFloat=0 -> 10
  [Alloc] vreg 12 [14,15] isFloat=0 -> 11
  [Alloc] vreg 13 [17,18] isFloat=1 -> 33f
  [Alloc] vreg 14 [18,19] isFloat=0 -> 10

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Folding addi+load/store: addi 10, 10, 4 + lw -> lw ..., 4(10)
  [Peephole] Folding addi+load/store: addi 10, 10, 4 + lw -> lw ..., 4(10)
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl f
f:
    addi sp, sp, -16
    sd ra, 0(sp)
    flw ft0, 0(a0)
    fcvt.w.s a0, ft0, rtz
    call putint
    flw ft0, 4(a0)
    fcvt.w.s a0, ft0, rtz
    call putint
    flw ft0, 0(a0)
    flw ft1, 4(a0)
    fadd.s ft2, ft0, ft1
    fmv.s fa0, ft2
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 8(sp)
    addi t0, sp, 8
    mv a0, t0
    mv a1, a0
    li a2, 1066192077
    fmv.w.x ft0, a2
    fsw ft0, 0(a1)
    addi a1, a0, 4
    li a0, 1074580685
    fmv.w.x ft0, a0
    fsw ft0, 0(a1)
    addi t0, sp, 8
    mv a0, t0
    mv a1, a0
    flw ft0, 0(a1)
    addi t0, sp, 8
    mv a0, t0
    mv a1, a0
    mv a0, a1
    call f
    fadd.s ft1, ft0, fa0
    fcvt.w.s a0, ft1, rtz
    ld ra, 8(sp)
    addi sp, sp, 16
    ret

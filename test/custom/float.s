[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 0 [0,1] isFloat=1 -> 42f
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [0,1] isFloat=0 -> 10
  [Alloc] vreg 0 [1,4] isFloat=1 -> 32f
  [Alloc] vreg 3 [2,3] isFloat=0 -> 10
  [Alloc] vreg 2 [3,4] isFloat=1 -> 33f
  [Alloc] vreg 4 [4,5] isFloat=1 -> 34f
  [Alloc] vreg 5 [5,6] isFloat=0 -> 10
  [Alloc] vreg 6 [6,7] isFloat=1 -> 32f
  [Alloc] vreg 8 [9,10] isFloat=0 -> 10
  [Alloc] vreg 7 [10,11] isFloat=1 -> 32f
  [Alloc] vreg 9 [11,14] isFloat=1 -> 33f
  [Alloc] vreg 11 [12,13] isFloat=0 -> 10
  [Alloc] vreg 10 [13,14] isFloat=1 -> 32f
  [Alloc] vreg 12 [14,15] isFloat=1 -> 34f
  [Alloc] vreg 13 [15,16] isFloat=0 -> 10

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: fmv.s 42, 42
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl square
square:
    fmul.s fa0, fa0, fa0
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 1067282596
    fmv.w.x ft0, a0
    li a0, 1075671204
    fmv.w.x ft1, a0
    fadd.s ft2, ft0, ft1
    fcvt.w.s a0, ft2, rtz
    fcvt.s.w ft0, a0, rne
    fmv.s fa0, ft0
    call square
    li a0, 1082759578
    fmv.w.x ft0, a0
    fmul.s ft1, fa0, ft0
    li a0, 1085276160
    fmv.w.x ft0, a0
    fsub.s ft2, ft1, ft0
    fcvt.w.s a0, ft2, rtz
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

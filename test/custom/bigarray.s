[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [BuildIntervals] WARNING: vreg 0 is used but not defined in instruction opc=63 in block bb0
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 1 [1,3] isFloat=0 -> 10
  [Alloc] vreg 3 [2,3] isFloat=0 -> 11
  [Alloc] vreg 2 [3,5] isFloat=0 -> 12
  [Alloc] vreg 4 [4,5] isFloat=0 -> 10
  [Alloc] vreg 5 [6,8] isFloat=0 -> 10
  [Alloc] vreg 7 [7,8] isFloat=0 -> 11
  [Alloc] vreg 6 [8,9] isFloat=0 -> 12
  [Alloc] vreg 8 [9,10] isFloat=0 -> 10

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl main
main:
    li t0, -40000
    add sp, sp, t0
    addi t0, sp, 0
    mv a0, t0
    li a1, 39996
    add a2, a0, a1
    li a0, 1
    sw a0, 0(a2)
    addi t0, sp, 0
    mv a0, t0
    li a1, 39996
    add a2, a0, a1
    lw a0, 0(a2)
    li t0, 40000
    add sp, sp, t0
    ret

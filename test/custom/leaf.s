[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 
  [Alloc] vreg 0 [0,2] isFloat=0 -> 10
  [Alloc] vreg 1 [1,2] isFloat=0 -> 11
  [Alloc] vreg 2 [2,3] isFloat=0 -> 12
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 

[Debug] ----- Running Peephole Optimization -----

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl leaf_add
leaf_add:
    addi a0, a0, 5
    addi a1, a1, 10
    add a2, a0, a1
    mv a0, a2
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 3
    li a1, 4
    call leaf_add
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

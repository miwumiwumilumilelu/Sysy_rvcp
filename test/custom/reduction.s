[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [1,2] isFloat=0 -> 10
  [Alloc] vreg 0 [2,3] isFloat=0 -> 11
  [Alloc] vreg 3 [7,8] isFloat=0 -> 10
  [Alloc] vreg 2 [8,9] isFloat=0 -> 11
  [Alloc] vreg 5 [13,14] isFloat=0 -> 10
  [Alloc] vreg 4 [14,15] isFloat=0 -> 11

[Debug] ----- Running Peephole Optimization -----

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    call getint
    li a0, 3
    divw a1, a0, a0
    mv a0, a1
    call putint
    li a0, 10
    call putch
    li a0, 5
    divw a1, a0, a0
    mv a0, a1
    call putint
    li a0, 10
    call putch
    li a0, 7
    divw a1, a0, a0
    mv a0, a1
    call putint
    li a0, 10
    call putch
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

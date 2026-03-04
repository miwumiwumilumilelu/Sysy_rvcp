[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 1 [0,1] isFloat=0 -> 10
  [Alloc] vreg 0 [1,2] isFloat=0 -> 11
  [Alloc] vreg 2 [4,8] isFloat=0 -> 10
  [Alloc] vreg 3 [5,6] isFloat=0 -> 12
  [Alloc] vreg 5 [6,7] isFloat=0 -> 11
  [Alloc] vreg 4 [7,9] isFloat=0 -> 12
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl even_odd
even_odd:
    addi sp, sp, -16
    sd ra, 0(sp)
    addi a0, a0, 0
    sltiu a1, a0, 1
    bne a1, zero, .Lbb2
    j .Lbb1
.Lbb1:
    addi a0, a0, -1
    sltu a2, zero, a1
    addi a1, a2, 0
    sltiu a2, a1, 1
    mv a1, a2
    call even_odd
    ld ra, 0(sp)
    addi sp, sp, 16
    ret
.Lbb2:
    mv a0, a1
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 5
    li a1, 0
    call even_odd
    call putint
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

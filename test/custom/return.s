[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 0 [0,1] isFloat=0 -> 11
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 

[Debug] ----- Running Peephole Optimization -----

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl f
f:
    addi sp, sp, -16
    sd ra, 0(sp)
    slt a1, zero, a0
    bne a1, zero, .Lbb2
    j .Lbb1
.Lbb1:
    call putint
    ld ra, 0(sp)
    addi sp, sp, 16
    ret
.Lbb2:
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 3
    call f
    li a0, -5
    call f
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

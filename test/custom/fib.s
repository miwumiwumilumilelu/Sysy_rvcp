[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 
  [Alloc] vreg 0 [0,1] isFloat=0 -> 10
  [Alloc] vreg 1 [3,4] isFloat=0 -> 10
  [Alloc] vreg 2 [6,7] isFloat=0 -> 10
  [Alloc] vreg 3 [9,10] isFloat=0 -> 10
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 1(unavail) 5(avail) 

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl fib
fib:
    addi sp, sp, -16
    sd ra, 0(sp)
    slti a0, a0, 2
    bne a0, zero, .Lbb2
    j .Lbb1
.Lbb1:
    addi a0, a0, -2
    call fib
    addi a0, a0, -1
    call fib
    add a0, a0, a0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret
.Lbb2:
    li a0, 1
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    li a0, 8
    call fib
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

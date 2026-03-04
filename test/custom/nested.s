[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [Alloc] Available int regs (first 10): 10(avail) 11(avail) 12(avail) 13(avail) 14(avail) 15(avail) 16(avail) 17(avail) 6(avail) 7(avail) 

[Debug] ----- Running Peephole Optimization -----

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl main
main:
    li a0, 7
    ret

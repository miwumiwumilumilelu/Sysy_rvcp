[Debug] Running FlattenCFG...
[Debug] Running Mem2Reg (Dominators inside)...

[Debug] ----- Starting Instruction Selection -----
  [InstSelector] Adding param register s0 -> savedReg=8 to savedRegs
  [InstSelector] savedRegs size after insert: 1
  [InstSelector] Adding param register s1 -> savedReg=9 to savedRegs
  [InstSelector] savedRegs size after insert: 2
  [InstSelector] Adding param register s2 -> savedReg=10 to savedRegs
  [InstSelector] savedRegs size after insert: 3
  [InstSelector] Adding param register s3 -> savedReg=11 to savedRegs
  [InstSelector] savedRegs size after insert: 4
  [InstSelector] Adding param register s4 -> savedReg=12 to savedRegs
  [InstSelector] savedRegs size after insert: 5
  [InstSelector] Adding param register s5 -> savedReg=13 to savedRegs
  [InstSelector] savedRegs size after insert: 6
  [InstSelector] Adding param register s6 -> savedReg=14 to savedRegs
  [InstSelector] savedRegs size after insert: 7
  [InstSelector] Adding param register s7 -> savedReg=15 to savedRegs
  [InstSelector] savedRegs size after insert: 8
  [InstSelector] Adding param register s0 -> savedReg=8 to savedRegs
  [InstSelector] savedRegs size after insert: 1
  [InstSelector] Adding param register s1 -> savedReg=9 to savedRegs
  [InstSelector] savedRegs size after insert: 2
  [InstSelector] Adding param register s2 -> savedReg=10 to savedRegs
  [InstSelector] savedRegs size after insert: 3
  [InstSelector] Adding param register s3 -> savedReg=11 to savedRegs
  [InstSelector] savedRegs size after insert: 4
  [InstSelector] Adding param register s4 -> savedReg=12 to savedRegs
  [InstSelector] savedRegs size after insert: 5
  [InstSelector] Adding param register s5 -> savedReg=13 to savedRegs
  [InstSelector] savedRegs size after insert: 6
  [InstSelector] Adding param register s6 -> savedReg=14 to savedRegs
  [InstSelector] savedRegs size after insert: 7
  [InstSelector] Adding param register s7 -> savedReg=15 to savedRegs
  [InstSelector] savedRegs size after insert: 8

[Debug] ----- Running Phi Elimination -----

[Debug] ----- Running Register Allocation -----
  [BuildInterferenceGraph] Built graph with 13 nodes
  [ComputeDegrees] vreg 3 degree=2 cost=3 range=[9,11]
  [ComputeDegrees] vreg 0 degree=10 cost=12 range=[10,21]
  [ComputeDegrees] vreg 4 degree=3 cost=3 range=[11,13]
  [ComputeDegrees] vreg 5 degree=4 cost=3 range=[13,15]
  [ComputeDegrees] vreg 1 degree=9 cost=9 range=[14,22]
  [ComputeDegrees] vreg 6 degree=4 cost=3 range=[15,17]
  [ComputeDegrees] vreg 7 degree=5 cost=3 range=[17,19]
  [ComputeDegrees] vreg 2 degree=8 cost=6 range=[18,23]
  [ComputeDegrees] vreg 8 degree=5 cost=2 range=[19,20]
  [ComputeDegrees] vreg 9 degree=5 cost=2 range=[20,21]
  [ComputeDegrees] vreg 10 degree=5 cost=2 range=[21,22]
  [ComputeDegrees] vreg 11 degree=4 cost=2 range=[22,23]
  [ComputeDegrees] vreg 12 degree=2 cost=2 range=[23,24]
  [Alloc] Allocating int registers with 27 available
    [Simplify] vreg 3 (degree 2 < 27)
    [Simplify] vreg 0 (degree 9 < 27)
    [Simplify] vreg 4 (degree 1 < 27)
    [Simplify] vreg 5 (degree 2 < 27)
    [Simplify] vreg 1 (degree 7 < 27)
    [Simplify] vreg 6 (degree 1 < 27)
    [Simplify] vreg 7 (degree 2 < 27)
    [Simplify] vreg 2 (degree 5 < 27)
    [Simplify] vreg 8 (degree 1 < 27)
    [Simplify] vreg 9 (degree 1 < 27)
    [Simplify] vreg 10 (degree 1 < 27)
    [Simplify] vreg 11 (degree 1 < 27)
    [Simplify] vreg 12 (degree 0 < 27)
  [Alloc] Selection phase with 13 nodes
  [Alloc] vreg 12 -> 16
  [Alloc] vreg 11 -> 17
  [Alloc] vreg 10 -> 16
  [Alloc] vreg 9 -> 17
  [Alloc] vreg 8 -> 16
  [Alloc] vreg 2 -> 6
  [Alloc] vreg 7 -> 17
  [Alloc] vreg 6 -> 16
  [Alloc] vreg 1 -> 7
  [Alloc] vreg 5 -> 17
  [Alloc] vreg 4 -> 16
  [Alloc] vreg 0 -> 28
  [Alloc] vreg 3 -> 17
  [Alloc] Final stack size: 80 bytes (saved=72, spilled=0)
  [BuildInterferenceGraph] Built graph with 21 nodes
  [ComputeDegrees] vreg 3 degree=7 cost=11 range=[9,19]
  [ComputeDegrees] vreg 0 degree=18 cost=21 range=[10,30]
  [ComputeDegrees] vreg 4 degree=3 cost=3 range=[11,13]
  [ComputeDegrees] vreg 5 degree=5 cost=5 range=[13,17]
  [ComputeDegrees] vreg 1 degree=18 cost=18 range=[14,31]
  [ComputeDegrees] vreg 6 degree=4 cost=3 range=[15,17]
  [ComputeDegrees] vreg 2 degree=17 cost=15 range=[18,32]
  [ComputeDegrees] vreg 7 degree=5 cost=2 range=[19,20]
  [ComputeDegrees] vreg 8 degree=5 cost=2 range=[20,21]
  [ComputeDegrees] vreg 9 degree=5 cost=2 range=[21,22]
  [ComputeDegrees] vreg 10 degree=5 cost=2 range=[22,23]
  [ComputeDegrees] vreg 11 degree=5 cost=2 range=[23,24]
  [ComputeDegrees] vreg 12 degree=5 cost=2 range=[24,25]
  [ComputeDegrees] vreg 13 degree=5 cost=2 range=[25,26]
  [ComputeDegrees] vreg 14 degree=5 cost=2 range=[26,27]
  [ComputeDegrees] vreg 15 degree=5 cost=2 range=[27,28]
  [ComputeDegrees] vreg 16 degree=5 cost=2 range=[28,29]
  [ComputeDegrees] vreg 17 degree=5 cost=2 range=[29,30]
  [ComputeDegrees] vreg 18 degree=5 cost=2 range=[30,31]
  [ComputeDegrees] vreg 19 degree=4 cost=2 range=[31,32]
  [ComputeDegrees] vreg 20 degree=2 cost=2 range=[32,33]
  [Alloc] Allocating int registers with 27 available
    [Simplify] vreg 3 (degree 7 < 27)
    [Simplify] vreg 0 (degree 17 < 27)
    [Simplify] vreg 4 (degree 1 < 27)
    [Simplify] vreg 5 (degree 2 < 27)
    [Simplify] vreg 1 (degree 15 < 27)
    [Simplify] vreg 6 (degree 0 < 27)
    [Simplify] vreg 2 (degree 14 < 27)
    [Simplify] vreg 7 (degree 1 < 27)
    [Simplify] vreg 8 (degree 1 < 27)
    [Simplify] vreg 9 (degree 1 < 27)
    [Simplify] vreg 10 (degree 1 < 27)
    [Simplify] vreg 11 (degree 1 < 27)
    [Simplify] vreg 12 (degree 1 < 27)
    [Simplify] vreg 13 (degree 1 < 27)
    [Simplify] vreg 14 (degree 1 < 27)
    [Simplify] vreg 15 (degree 1 < 27)
    [Simplify] vreg 16 (degree 1 < 27)
    [Simplify] vreg 17 (degree 1 < 27)
    [Simplify] vreg 18 (degree 1 < 27)
    [Simplify] vreg 19 (degree 1 < 27)
    [Simplify] vreg 20 (degree 0 < 27)
  [Alloc] Selection phase with 21 nodes
  [Alloc] vreg 20 -> 16
  [Alloc] vreg 19 -> 17
  [Alloc] vreg 18 -> 16
  [Alloc] vreg 17 -> 17
  [Alloc] vreg 16 -> 16
  [Alloc] vreg 15 -> 17
  [Alloc] vreg 14 -> 16
  [Alloc] vreg 13 -> 17
  [Alloc] vreg 12 -> 16
  [Alloc] vreg 11 -> 17
  [Alloc] vreg 10 -> 16
  [Alloc] vreg 9 -> 17
  [Alloc] vreg 8 -> 16
  [Alloc] vreg 7 -> 17
  [Alloc] vreg 2 -> 6
  [Alloc] vreg 6 -> 16
  [Alloc] vreg 1 -> 7
  [Alloc] vreg 5 -> 17
  [Alloc] vreg 4 -> 16
  [Alloc] vreg 0 -> 28
  [Alloc] vreg 3 -> 29
  [Alloc] Final stack size: 880 bytes (saved=72, spilled=800)
  [BuildIntervals-Forward] CALL instruction 15, recentFloatRegs: 
  [BuildIntervals-Forward] CALL instruction 19, recentFloatRegs: 
  [BuildIntervals-Forward] CALL instruction 21, recentFloatRegs: 
  [BuildIntervals-Forward] CALL instruction 37, recentFloatRegs: 
  [BuildIntervals-Forward] CALL instruction 41, recentFloatRegs: 
  [BuildIntervals-Forward] CALL instruction 43, recentFloatRegs: 
  [BuildIntervals] CALL instruction 43, currentLive count: 0
  [BuildIntervals] CALL instruction 41, currentLive count: 0
  [BuildIntervals] CALL instruction 37, currentLive count: 1
  [BuildIntervals] CALL instruction 21, currentLive count: 1
  [BuildIntervals] CALL instruction 19, currentLive count: 1
  [BuildIntervals] CALL instruction 15, currentLive count: 2
  [BuildInterferenceGraph] Built graph with 2 nodes
  [ComputeDegrees] vreg 0 degree=0 cost=2 range=[17,18]
  [ComputeDegrees] vreg 1 degree=0 cost=2 range=[39,40]
  [Alloc] Allocating int registers with 27 available
    [Simplify] vreg 0 (degree 0 < 27)
    [Simplify] vreg 1 (degree 0 < 27)
  [Alloc] Selection phase with 2 nodes
  [Alloc] vreg 1 -> 10
  [Alloc] vreg 0 -> 10
  [Alloc] Final stack size: 16 bytes (saved=8, spilled=0)

[Debug] ----- Running Peephole Optimization -----
  [Peephole] Folding addi+load/store: addi 16, 17, 600 + lw -> lw ..., 600(17)
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10
  [Peephole] Eliminating redundant move: mv 10, 10

[Debug] ----- Machine IR (Virtual Assembly) -----



.data

  .text

  .globl f
f:
    addi sp, sp, -80
    sd a5, 64(sp)
    sd a4, 56(sp)
    sd a3, 48(sp)
    sd a2, 40(sp)
    sd a1, 32(sp)
    sd a0, 24(sp)
    sd s1, 16(sp)
    sd s0, 8(sp)
    mv s0, a0
    mv s1, a1
    mv a0, a2
    mv a1, a3
    mv a2, a4
    mv a3, a5
    mv a4, a6
    mv a5, a7
    ld t5, 80(sp)
    add a7, s0, s1
    mv t3, t5
    add a6, a7, a0
    ld t5, 88(sp)
    add a7, a6, a1
    mv t2, t5
    add a6, a7, a2
    ld t5, 96(sp)
    add a7, a6, a3
    mv t1, t5
    add a6, a7, a4
    add a7, a6, a5
    add a6, a7, t3
    add a7, a6, t2
    add a6, a7, t1
    mv a0, a6
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld a0, 24(sp)
    ld a1, 32(sp)
    ld a2, 40(sp)
    ld a3, 48(sp)
    ld a4, 56(sp)
    ld a5, 64(sp)
    addi sp, sp, 80
    ret

  .globl fWithAlloca
fWithAlloca:
    addi sp, sp, -880
    sd a5, 64(sp)
    sd a4, 56(sp)
    sd a3, 48(sp)
    sd a2, 40(sp)
    sd a1, 32(sp)
    sd a0, 24(sp)
    sd s1, 16(sp)
    sd s0, 8(sp)
    mv s0, a0
    mv s1, a1
    mv a0, a2
    mv a1, a3
    mv a2, a4
    mv a3, a5
    mv a4, a6
    mv a5, a7
    ld t5, 880(sp)
    mv t3, t5
    addi t4, sp, 64
    mv a6, t4
    ld t5, 888(sp)
    addi a7, a6, 600
    mv t2, t5
    li a6, 9
    ld t5, 896(sp)
    sw a6, 0(a7)
    mv t1, t5
    addi t4, sp, 64
    mv a7, t4
    lw a7, 600(a7)
    add a6, a7, s0
    add a7, a6, s1
    add a6, a7, a0
    add a7, a6, a1
    add a6, a7, a2
    add a7, a6, a3
    add a6, a7, a4
    add a7, a6, a5
    add a6, a7, t3
    add a7, a6, t2
    add a6, a7, t1
    mv a0, a6
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld a0, 24(sp)
    ld a1, 32(sp)
    ld a2, 40(sp)
    ld a3, 48(sp)
    ld a4, 56(sp)
    ld a5, 64(sp)
    addi sp, sp, 880
    ret

  .globl main
main:
    addi sp, sp, -16
    sd ra, 0(sp)
    addi sp, sp, -32
    li t5, 11
    sd t5, 16(sp)
    li t5, 10
    sd t5, 8(sp)
    li t5, 9
    sd t5, 0(sp)
    li a7, 8
    li a6, 7
    li a5, 6
    li a4, 5
    li a3, 4
    li a2, 3
    li a1, 2
    li a0, 1
    call f
    addi sp, sp, 32
    call putint
    li a0, 10
    call putch
    addi sp, sp, -32
    li t5, 11
    sd t5, 16(sp)
    li t5, 10
    sd t5, 8(sp)
    li t5, 9
    sd t5, 0(sp)
    li a7, 8
    li a6, 7
    li a5, 6
    li a4, 5
    li a3, 4
    li a2, 3
    li a1, 2
    li a0, 1
    call fWithAlloca
    addi sp, sp, 32
    call putint
    li a0, 10
    call putch
    li a0, 0
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

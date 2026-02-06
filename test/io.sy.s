	.text
	.globl main
	.type main, @function
main:
	addi sp, sp, -48
	sd ra, 40(sp)
	sd s0, 32(sp)
	addi s0, sp, 48
.Lmain_entry_0:
	li t0, 10
	sw t0, -20(s0)
	li t0, 20
	sw t0, -24(s0)
	lw t0, -20(s0)
	sw t0, -28(s0)
	lw t0, -28(s0)
	mv a0, t0
	call putint
	li t0, 10
	mv a0, t0
	call putch
	lw t0, -20(s0)
	sw t0, -32(s0)
	lw t0, -24(s0)
	sw t0, -36(s0)
	lw t0, -32(s0)
	lw t1, -36(s0)
	addw t0, t0, t1
	sw t0, -40(s0)
	lw t0, -40(s0)
	mv a0, t0
	call putint
	li t0, 10
	mv a0, t0
	call putch
	li a0, 0
	ld ra, 40(sp)
	ld s0, 32(sp)
	addi sp, sp, 48
	ret

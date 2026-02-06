	.text
	.globl main
	.type main, @function
main:
	addi sp, sp, -80
	sd ra, 72(sp)
	sd s0, 64(sp)
	addi s0, sp, 80
.Lmain_entry_0:
	li t0, 10
	sw t0, -4(s0)
	li t0, 20
	sw t0, -8(s0)
	li t0, 0
	sw t0, -12(s0)
	lw t0, -4(s0)
	sw t0, -16(s0)
	lw t0, -8(s0)
	sw t0, -20(s0)
	lw t0, -16(s0)
	lw t1, -20(s0)
	slt t0, t0, t1
	sw t0, -24(s0)
	lw t0, -24(s0)
	bnez t0, .Lmain_then_3
	j .Lmain_else_2
.Lmain_entry_cont_1:
	lw t0, -12(s0)
	sw t0, -28(s0)
	lw t0, -28(s0)
	li t1, 1
	subw t0, t0, t1
	seqz t0, t0
	sw t0, -32(s0)
	lw t0, -32(s0)
	bnez t0, .Lmain_then_8
	j .Lmain_entry_cont_cont_7
.Lmain_else_2:
	li t0, 3
	sw t0, -12(s0)
	j .Lmain_entry_cont_1
.Lmain_then_3:
	lw t0, -4(s0)
	sw t0, -36(s0)
	lw t0, -36(s0)
	li t1, 5
	sgt t0, t0, t1
	sw t0, -40(s0)
	lw t0, -40(s0)
	bnez t0, .Lmain_then_6
	j .Lmain_else_5
.Lmain_then_cont_4:
.Lmain_else_5:
	li t0, 2
	sw t0, -12(s0)
	j .Lmain_then_cont_4
.Lmain_then_6:
	li t0, 1
	sw t0, -12(s0)
	j .Lmain_then_cont_4
.Lmain_entry_cont_cont_7:
	lw t0, -12(s0)
	sw t0, -44(s0)
	lw a0, -44(s0)
	ld ra, 72(sp)
	ld s0, 64(sp)
	addi sp, sp, 80
	ret
.Lmain_then_8:
	lw t0, -12(s0)
	sw t0, -48(s0)
	lw t0, -48(s0)
	li t1, 100
	addw t0, t0, t1
	sw t0, -52(s0)
	lw t0, -52(s0)
	sw t0, -12(s0)
	j .Lmain_entry_cont_cont_7

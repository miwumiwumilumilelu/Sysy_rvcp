	.text
	.globl main
	.type main, @function
main:
	addi sp, sp, -96
	sd ra, 88(sp)
	sd s0, 80(sp)
	addi s0, sp, 96
.Lmain_entry_0:
	li t0, 10
	sw t0, -20(s0)
	li t0, 5
	sw t0, -24(s0)
	lw t0, -20(s0)
	sw t0, -32(s0)
	lw t0, -24(s0)
	sw t0, -36(s0)
	lw t0, -32(s0)
	lw t1, -36(s0)
	addw t0, t0, t1
	sw t0, -40(s0)
	lw t0, -40(s0)
	sw t0, -28(s0)
	lw t0, -28(s0)
	sw t0, -44(s0)
	lw t0, -44(s0)
	li t1, 10
	sgt t0, t0, t1
	sw t0, -48(s0)
	lw t0, -48(s0)
	bnez t0, .Lmain_then_3
	j .Lmain_else_2
.Lmain_entry_cont_1:
	j .Lmain_cond_5
.Lmain_else_2:
	lw t0, -28(s0)
	sw t0, -52(s0)
	lw t0, -52(s0)
	li t1, 1
	addw t0, t0, t1
	sw t0, -56(s0)
	lw t0, -56(s0)
	sw t0, -28(s0)
	j .Lmain_entry_cont_1
.Lmain_then_3:
	lw t0, -28(s0)
	sw t0, -60(s0)
	lw t0, -60(s0)
	li t1, 1
	subw t0, t0, t1
	sw t0, -64(s0)
	lw t0, -64(s0)
	sw t0, -28(s0)
	j .Lmain_entry_cont_1
.Lmain_entry_cont_cont_4:
	lw t0, -28(s0)
	sw t0, -68(s0)
	lw a0, -68(s0)
	ld ra, 88(sp)
	ld s0, 80(sp)
	addi sp, sp, 96
	ret
.Lmain_cond_5:
	lw t0, -24(s0)
	sw t0, -72(s0)
	lw t0, -72(s0)
	li t1, 0
	sgt t0, t0, t1
	sw t0, -76(s0)
	lw t0, -76(s0)
	bnez t0, .Lmain_body_6
	j .Lmain_entry_cont_cont_4
.Lmain_body_6:
	lw t0, -28(s0)
	sw t0, -80(s0)
	lw t0, -80(s0)
	li t1, 1
	addw t0, t0, t1
	sw t0, -84(s0)
	lw t0, -84(s0)
	sw t0, -28(s0)
	lw t0, -24(s0)
	sw t0, -88(s0)
	lw t0, -88(s0)
	li t1, 1
	subw t0, t0, t1
	sw t0, -92(s0)
	lw t0, -92(s0)
	sw t0, -24(s0)
	j .Lmain_cond_5

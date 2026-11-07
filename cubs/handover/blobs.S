	.text
	.globl stub_start
stub_start:
	/* XNU calls us with the MMU on: x0 = log PA, x1 = log length, x2 = reason (2 = checkpoint), x3 = code.
	 * The cookie reports the number that follows the 6-byte pattern in the log text (default " pc 0x"), >> 2, 28 bits. */
	b	8f
	.balign 8
stub_pat:
	.ascii	" pc 0x"
	.byte	0, 0
8:
	mov	x9, x2
	mov	x10, x3
	mov	x11, x0
	mov	x12, x1
	mov	x4, #0x0800
	movk	x4, #0x30d0, lsl #16
	msr	sctlr_el1, x4
	isb
	/* copy the whole log into the persistent console (ramoops@95200000: header {sig, start, size}, text at +12) */
	mov	x5, #0x0000
	movk	x5, #0x9540, lsl #16
	movz	x6, #0xfff4
	movk	x6, #0x1f, lsl #16
	cmp	x12, x6
	csel	x13, x12, x6, ls
	add	x7, x5, #12
	mov	x8, x13
	mov	x15, x11
	cbz	x8, 21f
20:
	ldrb	w16, [x15], #1
	strb	w16, [x7], #1
	subs	x8, x8, #1
	b.ne	20b
21:
	mov	w16, #0x4244
	movk	w16, #0x4347, lsl #16
	str	w16, [x5]
	str	wzr, [x5, #4]
	str	w13, [x5, #8]
	dsb	sy
	adr	x14, stub_pat
	ldr	x14, [x14]
	/* offset mode: pattern byte 0 = 0xff, bytes 1..2 = k: report the 4 chars (7 bits each) at log[len - 4*(k+1)] */
	ubfx	x15, x14, #0, #8
	cmp	w15, #0xfe
	b.ne	31f
	/* warm mode: log already copied to ramoops; reset with the kernel-panic reason 0xbaba so the bootloader keeps it */
	mov	x10, #0xbaba
	b	10f
31:
	cmp	w15, #0xff
	b.ne	30f
	ubfx	x15, x14, #8, #16
	add	x15, x15, #1
	lsl	x15, x15, #2
	sub	x5, x12, x15
	cmp	x12, x15
	csel	x5, x5, xzr, hs
	add	x5, x11, x5
	ldrb	w13, [x5, #0]
	and	x4, x13, #0x7f
	ldrb	w13, [x5, #1]
	and	x13, x13, #0x7f
	orr	x4, x13, x4, lsl #7
	ldrb	w13, [x5, #2]
	and	x13, x13, #0x7f
	orr	x4, x13, x4, lsl #7
	ldrb	w13, [x5, #3]
	and	x13, x13, #0x7f
	orr	x4, x13, x4, lsl #7
	orr	x10, x4, #0xc0000000
	b	10f
30:
	mov	x5, x11
	add	x6, x11, x12
	mov	x16, #0
	mov	x17, #0
1:
	sub	x7, x6, x5
	cmp	x7, #8
	b.lo	5f
	ldrb	w13, [x5, #0]
	ubfx	x15, x14, #0, #8
	cmp	w13, w15
	b.ne	2f
	ldrb	w13, [x5, #1]
	ubfx	x15, x14, #8, #8
	cmp	w13, w15
	b.ne	2f
	ldrb	w13, [x5, #2]
	ubfx	x15, x14, #16, #8
	cmp	w13, w15
	b.ne	2f
	ldrb	w13, [x5, #3]
	ubfx	x15, x14, #24, #8
	cmp	w13, w15
	b.ne	2f
	ldrb	w13, [x5, #4]
	ubfx	x15, x14, #32, #8
	cmp	w13, w15
	b.ne	2f
	ldrb	w13, [x5, #5]
	ubfx	x15, x14, #40, #8
	cmp	w13, w15
	b.ne	2f
	b	3f
2:
	add	x5, x5, #1
	b	1b
3:
	add	x5, x5, #6
	mov	x4, #0
4:
	ldrb	w13, [x5], #1
	cmp	w13, #0x30
	b.lo	6f
	cmp	w13, #0x39
	b.ls	7f
	orr	w13, w13, #0x20
	cmp	w13, #0x61
	b.lo	6f
	cmp	w13, #0x66
	b.hi	6f
	sub	w13, w13, #0x57
	b	9f
7:
	sub	w13, w13, #0x30
9:
	lsl	x4, x4, #4
	orr	x4, x4, x13
	b	4b
6:
	mov	x16, x4
	mov	x17, #1
	b	1b
5:
	cbz	x17, 14f
	lsr	x4, x16, #2
	and	x4, x4, #0x0fffffff
	orr	x10, x4, #0xc0000000
	b	10f
14:
	and	x10, x10, #0x3fffffff
	orr	x10, x10, #0xc0000000
10:
	mov	x0, #0x0012
	movk	x0, #0x8400, lsl #16
	mov	x1, #0x0001
	movk	x1, #0x8000, lsl #16
	mov	x2, #0xc0de
	movk	x2, #0xcb05, lsl #16
	cmp	x9, #2
	b.eq	11f
	mov	x2, x10
11:
	smc	#0
	mov	x0, #0x0009
	movk	x0, #0x8400, lsl #16
	smc	#0
12:
	wfe
	b	12b
	.globl stub_end
stub_end:
	.globl tramp_start
tramp_start:
tramp_loc:
	mov	x6, x2
	mov	x2, x0
	mov	x3, x1
	msr	daifset, #0xf
	mov	x0, #0x0800
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0
	isb
	ic	iallu
	dsb	sy
	isb
	/* breadcrumb into the persistent console (ramoops@95200000, header {sig, start, size}, text at +12) */
	mov	x7, #0x0000
	movk	x7, #0x9520, lsl #16
	mov	w8, #0x4244
	movk	w8, #0x4347, lsl #16
	str	w8, [x7]
	str	wzr, [x7, #4]
	mov	w9, #0x10
	str	w9, [x7, #8]
	mov	w8, #0x5254
	movk	w8, #0x504d, lsl #16
	str	w8, [x7, #12]
	mov	w8, #0x4f2d
	movk	w8, #0x0a4b, lsl #16
	str	w8, [x7, #16]
	mov	w8, #0x4a55
	movk	w8, #0x504d, lsl #16
	str	w8, [x7, #20]
	mov	w8, #0x200a
	movk	w8, #0x2020, lsl #16
	str	w8, [x7, #24]
	dsb	sy
	cmp	x6, #9
	b.ne	13f
	/* mode 9: call the return stub with a test log staged at span+0x3000 */
	adr	x4, tramp_loc
	and	x4, x4, #0xffffffffffffc000
	add	x0, x4, #0x3, lsl #12
	mov	x1, #0x40
	mov	x2, #0
	mov	x3, #0
	add	x4, x4, #0x1, lsl #12
	br	x4
13:
	cbz	x6, 1f
	cmp	x6, #2
	b.ne	3f
	/* mode 2: plain PSCI SYSTEM_RESET only */
	mov	x0, #0x0009
	movk	x0, #0x8400, lsl #16
	smc	#0
	b	2f
3:
	/* test mode: report the cookie 0xc000222N (N = mode) through PSCI SYSTEM_RESET2 and stop */
	mov	x0, #0x0012
	movk	x0, #0x8400, lsl #16
	mov	x1, #0x0001
	movk	x1, #0x8000, lsl #16
	mov	x2, #0x2220
	movk	x2, #0xc000, lsl #16
	add	x2, x2, x6
	smc	#0
	mov	x0, #0x0009
	movk	x0, #0x8400, lsl #16
	smc	#0
2:
	wfe
	b	2b
1:
	mov	x0, x2
	br	x3
	.globl tramp_end
tramp_end:

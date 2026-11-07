/* Section layout expected by the Android 6.12 module loader (copied from vendor modules). */
	.section .modinfo,"a",@progbits
	.asciz "license=GPL"
	.asciz "name=cubs_handover"
	.asciz "depends="
	.asciz VERMAGIC

	.section __versions,"a",@progbits
	.balign 8
	.quad 0x797f2b3e
	.asciz "module_layout"
	.space 56 - 14
	.quad 0x16b5b21d
	.asciz "_printk"
	.space 56 - 8

	.section .gnu.linkonce.this_module,"aw",@progbits
	.balign 64
	.globl __this_module
	.type __this_module,@object
__this_module:
	.space 24
	.asciz "cubs_handover"
	.org __this_module + 0x188
	.quad cubs_init
	.org __this_module + 0x5f8
	.quad cubs_exit
	.org __this_module + 1600

	.section .plt,"a",@progbits
	.balign 16
	.byte 0
	.section .init.plt,"a",@progbits
	.byte 0
	.section .text.ftrace_trampoline,"a",@progbits
	.byte 0

	.globl init_module
	.set init_module, cubs_init
	.globl cleanup_module
	.set cleanup_module, cubs_exit

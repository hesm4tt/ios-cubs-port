/* Section layout expected by the Android 6.12 module loader (copied from vendor modules). */
	.section .modinfo,"a",@progbits
	.asciz "license=GPL"
	.asciz "name=cubs_log_reader"
	.asciz "depends="
	.asciz VERMAGIC

	.section __versions,"a",@progbits
	.balign 8
	.quad 0x797f2b3e
	.asciz "module_layout"
	.space 42
	.quad 0x1f42c609
	.asciz "__arch_copy_to_user"
	.space 36
	.quad 0x521dd20
	.asciz "memremap"
	.space 47
	.quad 0xba20531d
	.asciz "memunmap"
	.space 47
	.quad 0x7e9c6300
	.asciz "misc_register"
	.space 42
	.quad 0x3553109a
	.asciz "misc_deregister"
	.space 40
	.quad 0x72c16011
	.asciz "noop_llseek"
	.space 44

	.section .gnu.linkonce.this_module,"aw",@progbits
	.balign 64
	.globl __this_module
	.type __this_module,@object
__this_module:
	.space 24
	.asciz "cubs_log_reader"
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

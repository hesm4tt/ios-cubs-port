/* Hand-built Android kernel module: CUBS hand-over, milestone 1 (loads and logs). No kernel headers needed. */
extern int _printk(const char *fmt, ...);

int cubs_init(void)
{
	_printk("\0016cubs_handover: loaded\n");
	return 0;
}

void cubs_exit(void)
{
	_printk("\0016cubs_handover: unloaded\n");
}

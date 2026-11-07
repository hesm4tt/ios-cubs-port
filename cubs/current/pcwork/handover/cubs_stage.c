/* CUBS hand-over module: stage the XNU payload from Linux into reserved RAM (PA >= 0x900000000, hidden from Linux by
 * mem=4G on the boot_a cmdline), test executing from it under pKVM, and hand the CPU over to XNU.
 * Hand-built module: no kernel headers; struct layouts verified against 6.12 vendor modules.
 * Commands written to /dev/cubs_go:  "S" stage   "T" exec test (stays in Linux)   "J" jump into XNU (no return). */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long size_t;
typedef long ssize_t;
typedef long long loff_t;

struct file;
struct module;

struct file_operations {
	struct module *owner;                     /* 0 */
	u32 fop_flags;                            /* 8 */
	loff_t (*llseek)(struct file *, loff_t, int);              /* 16 */
	ssize_t (*read)(struct file *, char *, size_t, loff_t *);  /* 24 */
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *); /* 32 */
	void *rest[40];
};

struct miscdevice {
	int minor;
	const char *name;
	const struct file_operations *fops;
	void *rest[16];
};

extern int _printk(const char *fmt, ...);
extern void *vmalloc_noprof(unsigned long size);
extern void vfree(const void *p);
extern unsigned long __arch_copy_from_user(void *to, const void *from, unsigned long n);
extern unsigned long __arch_copy_to_user(void *to, const void *from, unsigned long n);
extern void *memremap(u64 offset, size_t size, unsigned long flags);
extern void *devm_ioremap(void *dev, u64 offset, size_t size);
extern int misc_register(struct miscdevice *misc);
extern void misc_deregister(struct miscdevice *misc);
extern loff_t noop_llseek(struct file *, loff_t, int);

#include "blobs.h"

#define CAP (64ULL << 20)
#define PAGE_16K 0x4000ULL
#define CUBS_FB_BYTES 0xB00000ULL
#define XNU_LOADER_SLOT_MAGIC 0x545352444c554e58ULL /* matches loader.c */

struct payload_seg { u64 vmaddr, vmsize, file_off, file_size; };
struct payload {
	char magic[8];
	u32 version, nsegs;
	u64 virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size;
	char cmdline[608];
	struct payload_seg segs[];
};
struct boot_video { u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; };
struct boot_args {
	unsigned short Revision, Version;
	u64 virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	u32 machineType;
	u64 deviceTreeP;
	u32 deviceTreeLength;
	char CommandLine[608];
	u64 bootFlags, memSizeActual;
};

static u8 *buf;
static u64 have;
static u8 *dst;         /* ioremap of the first 128 MiB of the reserved span */
static u64 span_pa, boot_args_pa_g, entry_pa_g;
static struct miscdevice dev_go;

static void copy64(volatile u64 *d, const u64 *src, u64 n)
{
	u64 i;
	for (i = 0; i < n / 8; i++)
		d[i] = src[i];
}

static void zero64(volatile u64 *d, u64 n)
{
	u64 i;
	for (i = 0; i < n / 8; i++)
		d[i] = 0;
}

static void put32(volatile u32 *d, const u32 *s, u32 words)
{
	u32 i;
	for (i = 0; i < words; i++)
		d[i] = s[i];
}

static ssize_t cs_write(struct file *f, const char *ub, size_t n, loff_t *pos)
{
	if (!buf || (u64)*pos + n > CAP)
		return -27;
	if (__arch_copy_from_user(buf + *pos, ub, n))
		return -14;
	*pos += n;
	if ((u64)*pos > have)
		have = *pos;
	return n;
}

static u64 clean_len, ram_mem_size, ram_base;
static u64 cp_n = 499;
static u64 q_pat = 0x0000783020637020ULL; /* " pc 0x" padded: bytes 20 70 63 20 30 78 */
static int keep_wdt; /* "K" jumps without disabling the AP watchdog: a hang then resets back into Android */

static void clean_range(u64 off, u64 len)
{
	u64 a, end = (u64)dst + off + len;
	for (a = ((u64)dst + off) & ~63ULL; a < end; a += 64)
		asm volatile("dc cvac, %0" :: "r"(a) : "memory");
	asm volatile("dsb sy\n\tisb" ::: "memory");
}

/* /dev/cubs_ram: sequential writes land at span + ram_base + file position (the iOS system image) */
static ssize_t ram_write(struct file *f, const char *ub, size_t n, loff_t *pos)
{
	u64 off = ram_base + (u64)*pos;
	if (!dst || off + n > ram_mem_size)
		return -27;
	if (__arch_copy_from_user(dst + off, ub, n))
		return -14;
	*pos += n;
	return n;
}

/* reads back from the same window (the GPU ring lives in the span) */
static ssize_t ram_read(struct file *f, char *ub, size_t n, loff_t *pos)
{
	u64 off = ram_base + (u64)*pos;
	if (!dst || off + n > ram_mem_size)
		return -27;
	if (__arch_copy_to_user(ub, dst + off, n))
		return -14;
	*pos += n;
	return n;
}

static u64 va_to_pa(const void *va)
{
	u64 par;
	asm volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va));
	return (par & 0xfffffffff000ULL) | ((u64)va & 0xfff);
}

static int do_stage(void)
{
	const struct payload *p = (const struct payload *)buf;
	u64 bias, adt_pa, ba_pa;
	struct boot_args *ba;
	u32 i, matches = 0;

	if (have < sizeof(*p) || p->magic[0] != 'S' || p->magic[7] != 'U' || p->version != 1) {
		_printk("\0013cubs_stage: bad payload (have %llu)\n", have);
		return -22;
	}
	_printk("\0016cubs_stage: payload %llu bytes nsegs %u phys_base %llx mem_size %llx entry %llx\n",
		have, p->nsegs, p->phys_base, p->mem_size, p->entry_va);
	if (!dst)
		dst = memremap(p->phys_base, p->mem_size, 1); /* MEMREMAP_WB on reserve_mem RAM: linear-map alias */
	if (!dst) {
		_printk("\0013cubs_stage: ioremap failed\n");
		return -12;
	}
	span_pa = p->phys_base;
	bias = p->phys_base - p->virt_base;
	for (i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		u64 off = s->vmaddr - p->virt_base;
		u64 rounded = (s->file_size + 7) & ~7ULL;
		copy64((volatile u64 *)(dst + off), (const u64 *)(buf + s->file_off), rounded);
		if (s->vmsize > rounded)
			zero64((volatile u64 *)(dst + off + rounded), s->vmsize - rounded);
	}
	/* stub (PSCI reset) at span+0x1000, trampoline at span+0x2000 */
	put32((volatile u32 *)(dst + 0x1000), stub_code, STUB_CODE_WORDS);
	*(volatile u64 *)(dst + 0x1008) = q_pat;
	put32((volatile u32 *)(dst + 0x2000), tramp_code, TRAMP_CODE_WORDS);
	put32((volatile u32 *)(dst + 0x4000), amp_code, AMP_CODE_WORDS);
	/* hook XNU's loader-return slot: 4 qwords {magic, stub, checkpoint, pa_bias} */
	for (i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		volatile u64 *w = (volatile u64 *)(dst + (s->vmaddr - p->virt_base));
		u64 off;
		for (off = 0; off + 32 <= s->file_size; off += 8) {
			if (w[off / 8] != XNU_LOADER_SLOT_MAGIC)
				continue;
			matches++;
			w[off / 8 + 1] = p->phys_base + 0x1000;
			w[off / 8 + 2] = cp_n;
			w[off / 8 + 3] = bias;
		}
	}
	_printk("\0016cubs_stage: loader-return slot matches %u\n", matches);
	adt_pa = p->kernel_end + bias;
	copy64((volatile u64 *)(dst + (adt_pa - p->phys_base)), (const u64 *)(buf + p->adt_off), (p->adt_size + 7) & ~7ULL);
	ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	ba = (struct boot_args *)(dst + (ba_pa - p->phys_base));
	zero64((volatile u64 *)ba, PAGE_16K);
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size - CUBS_FB_BYTES;
	ba->Video.v_baseAddr = p->phys_base + ba->memSize;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = 1080 * 4;
	ba->Video.v_width = 1080;
	ba->Video.v_height = 2424;
	ba->Video.v_depth = 32;
	ba->topOfKernelData = ba_pa + PAGE_16K;
	ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
	ba->deviceTreeLength = (u32)p->adt_size;
	for (i = 0; i < 608 && p->cmdline[i]; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	/* clean the staged range to the point of coherency: XNU starts with the MMU (and data caches) off */
	{
		clean_range(0, 0x8000000);
		clean_range(adt_pa - p->phys_base, ba_pa + PAGE_16K - adt_pa);
		clean_len = ba_pa + PAGE_16K - p->phys_base;
		ram_mem_size = p->mem_size;
	}
	boot_args_pa_g = ba_pa;
	entry_pa_g = p->entry_va + bias;
	_printk("\0016cubs_stage: staged: adt %llx boot_args %llx entry %llx\n", adt_pa, ba_pa, entry_pa_g);
	return 0;
}

static u64 *make_idmap(u64 *tbl_pa_out)
{
	u64 *tbl = vmalloc_noprof(4096);
	if (!tbl)
		return 0;
	zero64((volatile u64 *)tbl, 4096);
	/* 1 GiB block at the span, MAIR idx 4 (normal WB), inner shareable, AF; executable, EL1 only */
	tbl[(span_pa >> 30) & 0x1ff] = (span_pa & ~0x3fffffffULL) | 0x711ULL;
	*tbl_pa_out = va_to_pa(tbl);
	return tbl;
}

/* Execute a tiny function from the reserved span through an identity TTBR0 mapping (stays in Linux). */
static int do_exec_test(void)
{
	volatile u32 *code;
	u64 *tbl, tbl_pa, old_ttbr0, flags, res;

	if (!dst)
		return -22;
	code = (volatile u32 *)(dst + 0x3000);
	code[0] = 0xd2824680; /* mov x0, #0x1234 */
	code[1] = 0xd65f03c0; /* ret */
	asm volatile("dc cvac, %0\n\tdsb sy\n\tisb" :: "r"((u64)code) : "memory");
	tbl = make_idmap(&tbl_pa);
	if (!tbl)
		return -12;
	_printk("\0016cubs_stage: exec test: table pa %llx\n", tbl_pa);
	asm volatile("mrs %0, daif\n\tmsr daifset, #0xf" : "=r"(flags) :: "memory");
	asm volatile("mrs %0, ttbr0_el1" : "=r"(old_ttbr0));
	asm volatile("msr ttbr0_el1, %0\n\tisb\n\ttlbi vmalle1\n\tdsb nsh\n\tisb" :: "r"(tbl_pa) : "memory");
	{
		register u64 r asm("x0");
		asm volatile("ic iallu\n\tdsb nsh\n\tisb\n\tblr %1"
			: "=r"(r)
			: "r"(span_pa + 0x3000)
			: "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "memory", "cc");
		res = r;
	}
	asm volatile("msr ttbr0_el1, %0\n\tisb\n\ttlbi vmalle1\n\tdsb nsh\n\tisb" :: "r"(old_ttbr0) : "memory");
	asm volatile("msr daif, %0" :: "r"(flags) : "memory");
	vfree(tbl);
	_printk("\0016cubs_stage: exec test returned %llx (expect 1234)\n", res);
	return 0;
}


/* Linux leaves hundreds of device interrupts enabled and its timers running: quiet the GIC and the arch timers so XNU
 * starts from a calm interrupt controller (its own driver enables only what it needs). */
static void quiesce_gic(void)
{
	volatile u32 *d = devm_ioremap(dev_go.rest[3], 0x9228000ULL, 0x10000);
	volatile u8 *gicr = devm_ioremap(dev_go.rest[3], 0x92a8000ULL, 0x200000);
	volatile u32 *r = (volatile u32 *)(gicr + 0x10000);
	u32 n;
	{
		/* the redistributor frame of the CPU this runs on: match GICR_TYPER's affinity with MPIDR (as XNU does) */
		u64 mpidr, typer;
		u32 aff, f;

		asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
		aff = (u32)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));
		for (f = 0; f < 7; f++) {
			typer = *(volatile u64 *)(gicr + f * 0x40000ULL + 8);
			if ((u32)(typer >> 32) == aff) {
				r = (volatile u32 *)(gicr + f * 0x40000ULL + 0x10000);
				break;
			}
		}
	}
	asm volatile("msr cntp_ctl_el0, xzr" ::: "memory");
	asm volatile("msr cntv_ctl_el0, xzr" ::: "memory");
	asm volatile("msr s3_0_c12_c12_7, xzr" ::: "memory"); /* ICC_IGRPEN1_EL1 = 0 */
	asm volatile("isb" ::: "memory");
	/* Linux (hyp mode available) runs the GICv3 CPU interface with EOImode=1 (EOIR only drops priority, DIR deactivates);
	 * XNU expects EOImode=0, so its first timer interrupt would stay active forever and no further timer fires. */
	{
		u64 ctlr;
		asm volatile("mrs %0, s3_0_c12_c12_4" : "=r"(ctlr));
		_printk("\0016cubs_stage: ICC_CTLR_EL1 was %llx\n", ctlr);
		ctlr &= ~2ULL; /* EOImode = 0 */
		asm volatile("msr s3_0_c12_c12_4, %0\n\tisb" :: "r"(ctlr) : "memory");
		asm volatile("mrs %0, s3_0_c12_c12_4" : "=r"(ctlr));
		_printk("\0016cubs_stage: ICC_CTLR_EL1 now %llx\n", ctlr);
	}
	if (d) {
		for (n = 1; n < 32; n++) {
			d[(0x180 + 4 * n) / 4] = 0xffffffffu; /* ICENABLER */
			d[(0x280 + 4 * n) / 4] = 0xffffffffu; /* ICPENDR */
			d[(0x380 + 4 * n) / 4] = 0xffffffffu; /* ICACTIVER */
		}
	}
	if (r) {
		r[0x180 / 4] = 0xffffffffu;
		r[0x280 / 4] = 0xffffffffu;
		r[0x380 / 4] = 0xffffffffu;
	}
	asm volatile("dsb sy" ::: "memory");
}

/* The AP watchdog (wdt1 at 0x200c5000: key +0x10, control +0x8 bit0) would reset the phone while XNU boots. */
static void wdt_off(void)
{
	volatile u32 *w = devm_ioremap(dev_go.rest[3], 0x200c5000ULL, 0x1000);
	if (!w)
		return;
	w[4] = 0xc0ffee;
	w[2] = w[2] & ~1u;
	w[4] = 0;
	asm volatile("dsb sy" ::: "memory");
}

/* Hand the CPU over to XNU: never returns on success. Other CPUs must already be offline. */
static int do_jump(u64 mode)
{
	u64 *tbl, tbl_pa;

	if (!dst || !boot_args_pa_g)
		return -22;
	if (mode == 0 && !keep_wdt)
		wdt_off();
	if (mode == 0)
		quiesce_gic();
	tbl = make_idmap(&tbl_pa);
	if (!tbl)
		return -12;
	_printk("\0016cubs_stage: JUMP: table pa %llx tramp pa %llx boot_args %llx entry %llx\n",
		tbl_pa, span_pa + 0x2000, boot_args_pa_g, entry_pa_g);
	*(volatile u64 *)(dst + 0x110) = 0x1111; /* Linux reached the jump */
	asm volatile("dc civac, %0\n\tdsb sy" :: "r"((u64)dst + 0x100) : "memory");
	{
		register u64 a0 asm("x0") = boot_args_pa_g;
		register u64 a1 asm("x1") = entry_pa_g;
		register u64 a2 asm("x2") = mode;
		register u64 tgt asm("x9") = span_pa + 0x2000;
		register u64 ttbr asm("x10") = tbl_pa;
		asm volatile("msr daifset, #0xf\n\t"
			     "msr ttbr0_el1, x10\n\tisb\n\ttlbi vmalle1\n\tdsb nsh\n\tisb\n\t"
			     "br x9"
			     :: "r"(a0), "r"(a1), "r"(a2), "r"(tgt), "r"(ttbr)
			     : "memory");
	}
	return 0;
}

#define RESERVED_PA 0x922000000ULL /* start of the reserve_mem span (check /proc/iomem after each boot) */

/* Read the breadcrumbs the trampoline / Linux left at span+0x100.. (survive a reset if DRAM is not scrubbed). */
static int do_read_marks(void)
{
	volatile u64 *m;
	u64 i;

	if (!dst)
		dst = memremap(RESERVED_PA, 0x8000000, 1);
	if (!dst)
		return -12;
	m = (volatile u64 *)(dst + 0x100);
	for (i = 0; i < 8; i++)
		_printk("\0016cubs_stage: mark[%llx] = %llx\n", i * 8, m[i]);
	for (i = 0; i < 8; i++)
		m[i] = 0;
	asm volatile("dc civac, %0\n\tdsb sy" :: "r"((u64)dst + 0x100) : "memory");
	return 0;
}

static ssize_t go_write(struct file *f, const char *ub, size_t n, loff_t *pos)
{
	char c = 0;
	int r = 0;

	if (n && __arch_copy_from_user(&c, ub, 1))
		return -14;
	if (c == 'O') { /* "O<hex offset>": where /dev/cubs_ram writes start, relative to the span */
		char t[24];
		u64 v = 0;
		size_t i, m = n < 23 ? n : 23;
		if (__arch_copy_from_user(t, ub, m))
			return -14;
		for (i = 1; i < m; i++) {
			char h = t[i];
			if (h >= '0' && h <= '9') v = v * 16 + (h - '0');
			else if (h >= 'a' && h <= 'f') v = v * 16 + (h - 'a' + 10);
			else break;
		}
		ram_base = v;
		_printk("\0016cubs_stage: ram_base %llx\n", v);
		return n;
	}
	if (c == 'P') { /* "P<hex>": loader checkpoint number at which XNU hands back (499 = never) */
		char t[24];
		u64 v = 0;
		size_t i, m = n < 23 ? n : 23;
		if (__arch_copy_from_user(t, ub, m))
			return -14;
		for (i = 1; i < m; i++) {
			char h = t[i];
			if (h >= '0' && h <= '9') v = v * 16 + (h - '0');
			else if (h >= 'a' && h <= 'f') v = v * 16 + (h - 'a' + 10);
			else break;
		}
		cp_n = v;
		return n;
	}
	if (c == 'Q') { /* "Q<6 chars>": text pattern whose following hex number the return stub reports */
		char t[8];
		u64 v = 0;
		u32 i;
		if (n < 7 || __arch_copy_from_user(t, ub, 7))
			return -14;
		for (i = 0; i < 6; i++)
			v |= (u64)(u8)t[1 + i] << (8 * i);
		q_pat = v;
		return n;
	}
	if (c == 'W') { /* write a test string into the ramoops console zone (header + text) and clean it */
		static const char tx[] = "HELLO-FROM-LINUX-MODULE-TEST\n";
		volatile u8 *z = memremap(0x95200000ULL, 0x1000, 1);
		volatile u32 *h = (volatile u32 *)z;
		u32 i;
		if (!z)
			return -12;
		h[0] = 0x43474244u;
		h[1] = 0;
		h[2] = sizeof(tx) - 1;
		for (i = 0; i < sizeof(tx) - 1; i++)
			z[12 + i] = tx[i];
		asm volatile("dc cvac, %0\n\tdsb sy" :: "r"((u64)z) : "memory");
		return n;
	}
	if (c == 'M') { /* "M<hex phys addr>": print 8 words (32-bit reads) from a physical MMIO address */
		char t[24];
		u64 v = 0;
		size_t i, m = n < 23 ? n : 23;
		volatile u32 *r;
		if (__arch_copy_from_user(t, ub, m))
			return -14;
		for (i = 1; i < m; i++) {
			char h = t[i];
			if (h >= '0' && h <= '9') v = v * 16 + (h - '0');
			else if (h >= 'a' && h <= 'f') v = v * 16 + (h - 'a' + 10);
			else break;
		}
		r = devm_ioremap(dev_go.rest[3], v, 0x1000);
		if (!r)
			return -12;
		_printk("\0016cubs_stage: M %llx: %x %x %x %x %x %x %x %x\n", v, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
		return n;
	}
	if (c == 'Y' || c == 'F' || c == 'L' || c == 'X' || c == 'N' || c == 'V' || c == 'D' || c == 'A' || c == 'E' || c == 'H' || c == 'I' || c == 'G') { /* "Y<addr>:<val>" 32-bit MMIO write; "F<pa>:<val>:<len>" fill RAM with val and clean */
		char t[48];
		u64 a[3] = {0, 0, 0};
		size_t i, m = n < 47 ? n : 47;
		int k = 0;
		if (__arch_copy_from_user(t, ub, m))
			return -14;
		for (i = 1; i < m && k < 3; i++) {
			char h = t[i];
			if (h >= '0' && h <= '9') a[k] = a[k] * 16 + (h - '0');
			else if (h >= 'a' && h <= 'f') a[k] = a[k] * 16 + (h - 'a' + 10);
			else if (h == ':') k++;
			else break;
		}
		if (c == 'N') { /* "N<mpidr>:<mode>": start that core with PSCI CPU_ON at the amp stub; mode 1 enters XNU, 0 only reports */
			volatile u64 *pb;
			u64 ret;

			if (!dst)
				return -22;
			put32((volatile u32 *)(dst + 0x4000), amp_code, AMP_CODE_WORDS);
			pb = (volatile u64 *)(dst + 0x2900);
			pb[0] = boot_args_pa_g;
			pb[1] = entry_pa_g;
			pb[2] = 0;
			pb[3] = 0;
			pb[4] = 0;
			pb[5] = a[1];
			clean_range(0x2800, 0x2400);
			{
				register u64 x0 asm("x0") = 0xC4000003ULL;
				register u64 x1 asm("x1") = a[0];
				register u64 x2 asm("x2") = span_pa + 0x4000;
				register u64 x3 asm("x3") = span_pa + 0x2900;
				asm volatile(".inst 0xd4000003" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
					     : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
				ret = x0;
			}
			_printk("\0016cubs_stage: CPU_ON mpidr %llx entry %llx -> %lld\n", a[0], span_pa + 0x4000, (long long)ret);
			return n;
		}
		if (c == 'D') { /* "D<span off>:<len>": print that RAM as text lines in dmesg (XNU's log, read while it runs) */
			volatile u8 *p;
			char line[160];
			u64 i, cnt = 0;

			if (!dst || a[0] + a[1] > ram_mem_size)
				return -22;
			p = dst + a[0];
			for (i = 0; i < a[1]; i += 64)
				asm volatile("dc civac, %0" :: "r"((u64)p + i) : "memory");
			asm volatile("dsb sy" ::: "memory");
			for (i = 0; i < a[1]; i++) {
				u8 ch = p[i];

				if (ch == '\n' || cnt == 150) {
					line[cnt] = 0;
					_printk("\0016X| %s\n", line);
					cnt = 0;
					if (ch == '\n')
						continue;
				}
				if (ch == 0)
					break;
				line[cnt++] = (ch >= 32 && ch < 127) ? ch : '.';
			}
			if (cnt) {
				line[cnt] = 0;
				_printk("\0016X| %s\n", line);
			}
			return n;
		}
		if (c == 'I') { /* print the ID registers of the CPU this write runs on */
			u64 m0, m1, m2, p0, p1, i0, i1, d0, mp, mi;

			asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(m0));
			asm volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(m1));
			asm volatile("mrs %0, id_aa64mmfr2_el1" : "=r"(m2));
			asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(p0));
			asm volatile("mrs %0, id_aa64pfr1_el1" : "=r"(p1));
			asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(i0));
			asm volatile("mrs %0, id_aa64isar1_el1" : "=r"(i1));
			asm volatile("mrs %0, id_aa64dfr0_el1" : "=r"(d0));
			asm volatile("mrs %0, mpidr_el1" : "=r"(mp));
			asm volatile("mrs %0, midr_el1" : "=r"(mi));
			_printk("\0016ID| midr %llx mpidr %llx mmfr0 %llx mmfr1 %llx mmfr2 %llx\n", mi, mp, m0, m1, m2);
			_printk("\0016ID| pfr0 %llx pfr1 %llx isar0 %llx isar1 %llx dfr0 %llx\n", p0, p1, i0, i1, d0);
			return n;
		}
		if (c == 'G') { /* "G<off>:<len>": find the text "Darwin Kernel" in that part of the span (first 4 hits) */
			const char pat[] = "Darwin Kernel";
			volatile u8 *p;
			u64 i, j, hits = 0;

			if (!dst || a[0] + a[1] > ram_mem_size)
				return -22;
			p = dst + a[0];
			for (i = 0; i < a[1]; i += 64)
				asm volatile("dc civac, %0" :: "r"((u64)p + i) : "memory");
			asm volatile("dsb sy" ::: "memory");
			for (i = 0; i + 13 < a[1] && hits < 4; i++) {
				for (j = 0; j < 13 && p[i + j] == (u8)pat[j]; j++)
					;
				if (j == 13) {
					_printk("\0016G| hit at span offset %llx\n", a[0] + i);
					hits++;
				}
			}
			_printk("\0016G| scan done, %llu hits\n", hits);
			return n;
		}
		if (c == 'H') { /* "H<span off>:<nwords>": print that many 64-bit words, four a line */
			volatile u64 *p;
			u64 i;

			if (!dst || a[0] + a[1] * 8 > ram_mem_size)
				return -22;
			p = (volatile u64 *)(dst + a[0]);
			for (i = 0; i < a[1]; i += 64 / 8)
				asm volatile("dc civac, %0" :: "r"((u64)(p + i)) : "memory");
			asm volatile("dsb sy" ::: "memory");
			for (i = 0; i + 3 < a[1]; i += 4)
				_printk("\0016H| %llx: %016llx %016llx %016llx %016llx\n", a[0] + i * 8, p[i], p[i + 1], p[i + 2], p[i + 3]);
			return n;
		}
		if (c == 'E') { /* attach to an already running span without staging anything: E<phys_base>:<size> */
			span_pa = a[0];
			ram_mem_size = a[1];
			dst = memremap(a[0], a[1], 1);
			return dst ? n : -12;
		}
		if (c == 'A') { /* "A<mpidr>": PSCI AFFINITY_INFO */
			register u64 x0 asm("x0") = 0xC4000004ULL;
			register u64 x1 asm("x1") = a[0];
			register u64 x2 asm("x2") = 0;
			asm volatile(".inst 0xd4000003" : "+r"(x0) : "r"(x1), "r"(x2)
				     : "memory", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
			_printk("\0016cubs_stage: affinity %llx = %lld\n", a[0], (long long)x0);
			return n;
		}
		if (c == 'V') { /* print what the started core reported */
			volatile u64 *pb = (volatile u64 *)(dst + 0x2900);

			asm volatile("dc civac, %0\n\tdsb sy" :: "r"((u64)pb) : "memory");
			_printk("\0016cubs_stage: amp EL %llx mpidr %llx marker %llx log %llx len %llx reason %llx code %llx elr %llx far %llx lr %llx sp %llx\n", pb[2], pb[3], pb[4], pb[6], pb[7], pb[8], pb[9], pb[10], pb[11], pb[12], pb[13]);
			return n;
		}
		if (c == 'X') { /* "X<addr>": read ONE 32-bit register and print it */
			volatile u32 *w = devm_ioremap(dev_go.rest[3], a[0] & ~0xfffULL, 0x1000);
			if (!w)
				return -12;
			_printk("\0016cubs_stage: X %llx = %x\n", a[0], w[(a[0] & 0xfff) / 4]);
			return n;
		}
		if (c == 'L') { /* "L<span offset>:<len>": clean that span range to the point of coherency */
			clean_range(a[0], a[1]);
			return n;
		}
		if (c == 'Y') {
			volatile u32 *w = devm_ioremap(dev_go.rest[3], a[0] & ~0xfffULL, 0x1000);
			if (!w)
				return -12;
			w[(a[0] & 0xfff) / 4] = (u32)a[1];
			asm volatile("dsb sy" ::: "memory");
			return n;
		} else {
			volatile u32 *p = memremap(a[0], a[2], 1);
			u64 j;
			if (!p)
				return -12;
			for (j = 0; j < a[2] / 4; j++)
				p[j] = (u32)a[1];
			for (j = 0; j < a[2]; j += 64)
				asm volatile("dc civac, %0" :: "r"((u64)p + j) : "memory");
			asm volatile("dsb sy" ::: "memory");
			return n;
		}
	}
	if (c == 'C') {
		clean_range(0, clean_len);
		_printk("\0016cubs_stage: cleaned %llx bytes\n", clean_len);
		return n;
	}
	if (c == 'S')
		r = do_stage();
	else if (c == 'T')
		r = do_exec_test();
	else if (c == 'J')
		r = do_jump(0);
	else if (c == 'K') {
		keep_wdt = 1;
		r = do_jump(0);
	} else if (c == 'U') { /* self-test of the return stub with a fake XNU log */
		static const char tl[] = "panic: x at pc 0xfffffff007781fd4, lr 0x1";
		u32 i;
		for (i = 0; i < sizeof(tl); i++)
			((volatile u8 *)(dst + 0x3000))[i] = tl[i];
		clean_range(0x1000, 0x4000);
		r = do_jump(9);
	} else if (c == 'A')
		r = do_jump(1);
	else if (c == 'B')
		r = do_jump(2);
	else if (c == 'R')
		r = do_read_marks();
	else
		_printk("\0016cubs_stage: unknown command %c\n", c ? c : '?');
	return r ? (ssize_t)r : (ssize_t)n;
}

static const struct file_operations ops_data = { .llseek = noop_llseek, .write = cs_write };
static const struct file_operations ops_go = { .llseek = noop_llseek, .write = go_write };
static const struct file_operations ops_ram = { .llseek = noop_llseek, .read = ram_read, .write = ram_write };
static struct miscdevice dev_ram = { .minor = 255, .name = "cubs_ram", .fops = &ops_ram };
static struct miscdevice dev_data = { .minor = 255, .name = "cubs_stage", .fops = &ops_data };
static struct miscdevice dev_go = { .minor = 255, .name = "cubs_go", .fops = &ops_go };

int cubs_init(void)
{
	int r;
	buf = vmalloc_noprof(CAP);
	if (!buf)
		return -12;
	r = misc_register(&dev_data);
	if (r)
		return r;
	r = misc_register(&dev_go);
	if (r) {
		misc_deregister(&dev_data);
		return r;
	}
	r = misc_register(&dev_ram);
	if (r) {
		misc_deregister(&dev_go);
		misc_deregister(&dev_data);
		return r;
	}
	_printk("\0016cubs_stage: ready (/dev/cubs_stage, /dev/cubs_go: S=stage T=exec test J=jump)\n");
	return 0;
}

void cubs_exit(void)
{
	misc_deregister(&dev_ram);
	misc_deregister(&dev_go);
	misc_deregister(&dev_data);
	vfree(buf);
}

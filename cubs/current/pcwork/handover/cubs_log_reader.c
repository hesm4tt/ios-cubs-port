/* Experimental read-only view of the reserved XNU span.
 * This device has its own mapping and never changes cubs_stage's dst or offset.
 * Do not deploy until its ABI, symbol versions and access controls are verified.
 */
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long size_t;
typedef long ssize_t;
typedef long long loff_t;
struct file;
struct module;
extern struct module __this_module;
struct file_operations {
	struct module *owner;
	u32 fop_flags;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
	void *rest[40];
};
struct miscdevice { int minor; const char *name; const struct file_operations *fops; void *rest[16]; };
extern void *memremap(u64, size_t, unsigned long);
extern void memunmap(void *);
extern unsigned long __arch_copy_to_user(void *, const void *, unsigned long);
extern int misc_register(struct miscdevice *);
extern void misc_deregister(struct miscdevice *);
extern loff_t noop_llseek(struct file *, loff_t, int);

#define LOG_PA 0x922000000ULL
#define LOG_SIZE (48ULL << 20)
static const char *log_mapping;

static ssize_t log_read(struct file *file, char *user, size_t n, loff_t *pos)
{
	u64 off;
	(void)file;
	if (*pos < 0) return -22;
	off = (u64)*pos;
	if (off >= LOG_SIZE) return 0;
	if (n > LOG_SIZE - off) n = (size_t)(LOG_SIZE - off);
	if (__arch_copy_to_user(user, log_mapping + off, n)) return -14;
	*pos += n;
	return (ssize_t)n;
}

static const struct file_operations log_ops = { .owner = &__this_module, .llseek = noop_llseek, .read = log_read };
static struct miscdevice log_dev = { .minor = 255, .name = "cubs_log", .fops = &log_ops };

int cubs_init(void)
{
	int ret;
	log_mapping = memremap(LOG_PA, LOG_SIZE, 1);
	if (!log_mapping) return -12;
	ret = misc_register(&log_dev);
	if (ret) memunmap((void *)log_mapping);
	return ret;
}

void cubs_exit(void)
{
	misc_deregister(&log_dev);
	memunmap((void *)log_mapping);
}

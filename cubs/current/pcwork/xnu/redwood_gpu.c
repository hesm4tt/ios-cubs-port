/*
 * /dev/cubs-gpu: iOS userland's window onto the GPU ring that Linux's GPU server (pcwork/gpusrv) serves.
 * The ring is RAM beside XNU's own (PA 0xab2000000, boot-arg cubs_gpu_pa=<hex> to move it); read(2) and write(2) at a file
 * offset move bytes between the caller and the ring, with the same layout as pcwork/gpusrv/src/ring.rs.
 *
 * To build in: add "bsd/dev/redwood_gpu.c standard" to bsd/conf/files and call redwood_gpu_init() from redwood_input_init()
 * (bsd/dev/redwood_input.c), where the other /dev/redwood-* devices are made. Not yet built or run.
 */
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <vm/memory_types.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <pexpert/pexpert.h>

#define CUBS_GPU_RING_SIZE 0x1000000ULL

extern vm_offset_t io_map(vm_map_offset_t phys_addr, vm_size_t size, unsigned int flags, vm_prot_t prot, bool unmappable);
extern void FlushPoC_DcacheRegion(vm_offset_t va, unsigned length);

static uint64_t gpudev_pa = 0xab2000000ULL;
static volatile uint8_t *gpudev_ring;

static int
gpudev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	if (gpudev_ring == NULL) {
		gpudev_ring = (volatile uint8_t *)io_map(gpudev_pa, CUBS_GPU_RING_SIZE, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
	}
	printf("cubs-gpu: open, ring %p (pa %llx), first word %x\n", (void *)gpudev_ring, gpudev_pa,
	    gpudev_ring ? *(volatile uint32_t *)gpudev_ring : 0);
	return gpudev_ring == NULL ? ENXIO : 0;
}

static int
gpudev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
gpudev_rw(struct uio *uio, bool write)
{
	off_t off = uio_offset(uio);
	user_ssize_t n = uio_resid(uio);
	int error;

	if (gpudev_ring == NULL || off < 0 || (uint64_t)off >= CUBS_GPU_RING_SIZE) {
		return 0;
	}
	if ((uint64_t)n > CUBS_GPU_RING_SIZE - (uint64_t)off) {
		n = (user_ssize_t)(CUBS_GPU_RING_SIZE - (uint64_t)off);
	}
	if (!write) {
		FlushPoC_DcacheRegion((vm_offset_t)(gpudev_ring + off), (unsigned)n);
	}
	error = uiomove((caddr_t)(gpudev_ring + off), (int)n, uio);
	if (write) {
		__asm__ volatile ("dsb sy" ::: "memory");
		FlushPoC_DcacheRegion((vm_offset_t)(gpudev_ring + off), (unsigned)n);
	}
	return error;
}

static int
gpudev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return gpudev_rw(uio, false);
}

static int
gpudev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return gpudev_rw(uio, true);
}

static const struct cdevsw gpudev_cdevsw = {
	.d_open = gpudev_open, .d_close = gpudev_close, .d_read = gpudev_read, .d_write = gpudev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
redwood_gpu_init(void)
{
	uint64_t pa;
	int major;

	if (PE_parse_boot_argn("cubs_gpu_pa", &pa, sizeof(pa)) && pa != 0) {
		gpudev_pa = pa;
	}
	major = cdevsw_add(-1, &gpudev_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cubs-gpu") == NULL) {
		printf("cubs-gpu: cannot create the device\n");
	}
}

/*
 * libmgpatch.dylib: injected into lsd (and other processes that ask MobileGestalt many questions). libMobileGestalt waits in
 * MGIOKitSupport for IORegistry services (IONetworkController, AppleDiagnosticDataAccessReadOnly, ...) with a CFRunLoop of up to
 * 5 seconds per question, and on this port some of these services never appear, so every such question costs 5 seconds and iOS
 * first boot takes ~16 minutes. The run loop's timeout is converted by one instruction (scvtf d0, w8); this patches it to
 * "fmov d0, xzr" (a timeout of 0) in the process's own copy of the dyld shared cache page. iOS 16.5 (20F66) only; the old
 * instruction is checked first.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PATCH_OFFSET 0x1A9DCUL          /* from MGSetAnswer to the scvtf in the match-waiting loop */
#define OLD_INSN 0x1e620100U            /* scvtf d0, w8 */
#define NEW_INSN 0x9e6703e0U            /* fmov d0, xzr */

__attribute__((constructor)) static void
start(void)
{
	int con = open("/dev/console", O_WRONLY);
	char b[200];
	int n;
	void *base = dlsym(RTLD_DEFAULT, "MGSetAnswer");

	if (!base) {
		void *h = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);

		base = h ? dlsym(h, "MGSetAnswer") : NULL;
	}
	if (!base) {
		n = snprintf(b, sizeof b, "mgpatch: MGSetAnswer not found\n");
		goto out;
	}
	uint32_t *p = (uint32_t *)((uintptr_t)base + PATCH_OFFSET);

	if (*p != OLD_INSN) {
		n = snprintf(b, sizeof b, "mgpatch: unexpected instruction %08x at %p, not patching\n", *p, (void *)p);
		goto out;
	}
	vm_address_t page = (vm_address_t)((uintptr_t)p & ~0x3fffUL);
	kern_return_t kr = vm_protect(mach_task_self(), page, 0x4000, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

	if (kr != KERN_SUCCESS) {
		n = snprintf(b, sizeof b, "mgpatch: vm_protect(write) failed %d\n", kr);
		goto out;
	}
	*p = NEW_INSN;
	sys_icache_invalidate(p, 4);
	kr = vm_protect(mach_task_self(), page, 0x4000, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	n = snprintf(b, sizeof b, "mgpatch: patched %p (now %08x), restore protection %d\n", (void *)p, *p, kr);
out:
	if (con >= 0) {
		(void)write(con, b, (size_t)n);
		close(con);
	}
}

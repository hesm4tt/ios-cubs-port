/*
 * libsbspy.dylib: injected into SpringBoard (INJECT line "/redwood/bin/libsbspy.dylib SpringBoard"), it prints the call stack of
 * every thread of the process to /dev/console at 60 s, then every 120 s, to see what SpringBoard waits for during iOS first boot
 * (it uses ~14 s of CPU in 16 minutes). Frame-pointer walk, symbols by dladdr.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int con = -1;

static void
say(const char *fmt, ...)
{
	char b[400];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	__builtin_va_end(ap);
	if (con >= 0 && n > 0) {
		(void)write(con, b, (size_t)n);
	}
}

static int
readmem(uint64_t addr, void *out, size_t len)
{
	vm_size_t got = 0;

	return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, len, (vm_address_t)out, &got) == KERN_SUCCESS && got == len;
}

static void
frame(int i, uint64_t pc)
{
	Dl_info di;

	pc &= 0x0000007fffffffffULL;
	if (dladdr((void *)pc, &di) && di.dli_fname) {
		const char *base = strrchr(di.dli_fname, '/');

		say("sbspy:   #%d %p %s`%s+%lu\n", i, (void *)pc, base ? base + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?",
		    di.dli_saddr ? (unsigned long)(pc - (uint64_t)di.dli_saddr) : 0UL);
	} else {
		say("sbspy:   #%d %p\n", i, (void *)pc);
	}
}

static void
dump_all(int round)
{
	thread_act_array_t th;
	mach_msg_type_number_t n = 0;
	mach_port_t self = mach_thread_self();

	say("sbspy: ===== round %d =====\n", round);
	if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) {
		return;
	}
	for (mach_msg_type_number_t t = 0; t < n && t < 60; t++) {
		if (th[t] == self) {
			continue;
		}
		thread_suspend(th[t]);
		arm_thread_state64_t st;
		mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;

		if (thread_get_state(th[t], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
			uint64_t pc = __darwin_arm_thread_state64_get_pc(st), fp = __darwin_arm_thread_state64_get_fp(st);

			say("sbspy: thread %u\n", t);
			frame(0, pc);
			for (int i = 1; i < 24 && fp > 0x1000; i++) {
				uint64_t pair[2];

				if (!readmem(fp, pair, 16)) {
					break;
				}
				frame(i, pair[1]);
				if (pair[0] <= fp) {
					break;
				}
				fp = pair[0];
			}
		}
		thread_resume(th[t]);
	}
}

static void *
spy(void *arg)
{
	(void)arg;
	con = open("/dev/console", O_WRONLY);
	if (con < 0) {
		con = 2;
	}
	sleep(45);
	for (int r = 0; r < 12; r++) {
		dump_all(r);
		sleep(120);
	}
	return NULL;
}

__attribute__((constructor)) static void
start(void)
{
	pthread_t t;

	if (pthread_create(&t, NULL, spy, NULL) == 0) {
		pthread_detach(t);
	}
}

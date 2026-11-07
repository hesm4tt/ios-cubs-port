/*
 * tfpspy: run as root from a launchd job line ("/redwood/bin/tfpspy MobileGestaltHelper &"). After a delay it finds the process
 * by name (sysctl kern.proc.all) and prints the call stack of each of its threads to /dev/console, a few rounds. For daemons
 * whose sandbox does not let an injected library write the console (MobileGestaltHelper). Frame-pointer walk, symbols by dladdr
 * (system libraries have the same addresses in every process through the shared cache).
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern kern_return_t task_for_pid(mach_port_t, int, mach_port_t *);

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
find_pid(const char *name)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;

	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
		return -1;
	}
	struct kinfo_proc *kp = malloc(len + 4096);

	len += 4096;
	if (!kp || sysctl(mib, 3, kp, &len, NULL, 0) != 0) {
		return -1;
	}
	int pid = -1;

	for (size_t i = 0; i < len / sizeof(*kp); i++) {
		if (strncmp(kp[i].kp_proc.p_comm, name, 15) == 0) {
			pid = kp[i].kp_proc.p_pid;
		}
	}
	free(kp);
	return pid;
}

static void
frame(int i, uint64_t pc)
{
	Dl_info di;

	pc &= 0x0000007fffffffffULL;
	if (dladdr((void *)pc, &di) && di.dli_fname) {
		const char *base = strrchr(di.dli_fname, '/');

		say("tfpspy:   #%d %p %s`%s+%lu\n", i, (void *)pc, base ? base + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?",
		    di.dli_saddr ? (unsigned long)(pc - (uint64_t)di.dli_saddr) : 0UL);
	} else {
		say("tfpspy:   #%d %p\n", i, (void *)pc);
	}
}

static void
dump(mach_port_t task, int round)
{
	thread_act_array_t th;
	mach_msg_type_number_t n = 0;

	say("tfpspy: ===== round %d =====\n", round);
	kern_return_t kr = task_threads(task, &th, &n);

	if (kr != KERN_SUCCESS) {
		say("tfpspy: task_threads failed %d port %u\n", kr, (unsigned)task);
		return;
	}
	for (mach_msg_type_number_t t = 0; t < n && t < 40; t++) {
		thread_suspend(th[t]);
		arm_thread_state64_t st;
		mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;

		if (thread_get_state(th[t], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
			uint64_t pc = __darwin_arm_thread_state64_get_pc(st), fp = __darwin_arm_thread_state64_get_fp(st);

			say("tfpspy: thread %u\n", t);
			frame(0, pc);
			for (int i = 1; i < 24 && fp > 0x1000; i++) {
				uint64_t pair[2];
				vm_size_t got = 0;

				if (vm_read_overwrite(task, (vm_address_t)fp, 16, (vm_address_t)pair, &got) != KERN_SUCCESS || got != 16) {
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

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "MobileGestaltHe";
	int delay = argc > 2 ? atoi(argv[2]) : 60;

	con = open("/dev/console", O_WRONLY);
	sleep(delay);
	for (int r = 0; r < 6; r++) {
		int pid = find_pid(name);
		mach_port_t task = MACH_PORT_NULL;

		if (pid < 0) {
			say("tfpspy: no process %s\n", name);
		} else {
			kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);

			if (kr != KERN_SUCCESS) {
				say("tfpspy: task_for_pid(%d) = %d\n", pid, kr);
			} else {
				say("tfpspy: pid %d\n", pid);
				dump(task, r);
			}
		}
		sleep(60);
	}
	return 0;
}

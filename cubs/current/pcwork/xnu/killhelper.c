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
#include <signal.h>
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

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "MobileGestaltHe";
	int delay = argc > 2 ? atoi(argv[2]) : 60;
	int rounds = argc > 3 ? atoi(argv[3]) : 30;

	con = open("/dev/console", O_WRONLY);
	sleep(delay);
	for (int r = 0; r < rounds; r++) {
		int pid = find_pid(name);

		if (pid > 1) {
			say("killhelper: kill -9 %d = %d\n", pid, kill(pid, SIGKILL));
		}
		sleep(10);
	}
	return 0;
}

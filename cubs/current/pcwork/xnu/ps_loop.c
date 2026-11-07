/*
 * ps_loop: every PERIOD seconds print the processes that have used the most CPU time (/bin/ps -A -o pid,time,comm), to see what
 * iOS first boot spends its time on. Dynamically linked iOS program; run by a launchd job list line "/redwood/bin/ps_loop &".
 */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int
main(int argc, char **argv)
{
	int period = argc > 1 ? atoi(argv[1]) : 60;
	int rounds = argc > 2 ? atoi(argv[2]) : 40;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 0; i < rounds; i++) {
		sleep(period);
		printf("ps_loop: round %d\n", i);
		char *args[] = { "/bin/ps", "-A", "-o", "pid,ppid,stat,wchan,time,comm", NULL };
		pid_t pid;

		if (posix_spawn(&pid, "/bin/ps", NULL, NULL, args, environ) == 0) {
			int st;

			waitpid(pid, &st, 0);
		} else {
			printf("ps_loop: cannot run ps\n");
		}
	}
	return 0;
}

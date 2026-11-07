/*
 * varscan: after DELAY seconds, look through /private/var for files whose name contains NAME (default "MobileGestalt"), print
 * their path and size, and hex dump the small ones ("VARHEX path offset hex..."), so that a file iOS builds on first boot can be
 * taken out of the XNU log and pre-seeded into the root image (var.skel). Dynamically linked iOS program.
 * usage: varscan DELAY [NAME] [MAXBYTES]
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *needle = "MobileGestalt";
static long maxbytes = 300000;
static int dumped;

static void
dump(const char *path, long size)
{
	FILE *f = fopen(path, "rb");
	unsigned char buf[96];
	long off = 0;
	size_t n;

	if (!f) {
		return;
	}
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		char line[400];
		int k = snprintf(line, sizeof line, "VARHEX %s %ld ", path, off);

		for (size_t i = 0; i < n; i++) {
			k += snprintf(line + k, sizeof line - k, "%02x", buf[i]);
		}
		puts(line);
		off += (long)n;
	}
	fclose(f);
	printf("VARDONE %s %ld\n", path, size);
	dumped++;
}

static void
walk(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d || depth > 12) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (lstat(path, &st) != 0) {
			continue;
		}
		if (strcasestr(e->d_name, needle)) {
			printf("VARFILE %s %lld %s\n", path, (long long)st.st_size, S_ISDIR(st.st_mode) ? "dir" : "file");
		}
		if (S_ISDIR(st.st_mode)) {
			walk(path, depth + 1);
		} else if (S_ISREG(st.st_mode) && strcasestr(e->d_name, needle) && st.st_size <= maxbytes && dumped < 6) {
			dump(path, st.st_size);
		}
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	int delay = argc > 1 ? atoi(argv[1]) : 1200;

	if (argc > 2) {
		needle = argv[2];
	}
	if (argc > 3) {
		maxbytes = atol(argv[3]);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	sleep(delay);
	printf("varscan: scanning /private/var for \"%s\"\n", needle);
	walk("/private/var", 0);
	printf("varscan: done, %d files dumped\n", dumped);
	return 0;
}

/* rmls: removes the LaunchServices databases (csstore files) so that lsd makes them afresh with the MobileGestalt answers of libmgcam (the image is loaded into RAM at every boot: nothing is lost) */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int n;

static void
walk(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char p[1024];
	struct stat st;

	if (!d) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (lstat(p, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < 7) {
				walk(p, depth + 1);
			}
		} else if (strstr(e->d_name, "csstore") || strstr(e->d_name, "LaunchServices-") || strstr(e->d_name, "com.apple.lsd.") || strstr(e->d_name, "mobile.installation")) {
			printf("rmls: %s %s\n", p, unlink(p) == 0 ? "removed" : "NOT removed");
			n++;
		}
	}
	closedir(d);
}

int main(void)
{
	walk("/private/var/containers/Data/System", 0);
	walk("/private/var/mobile/Library/Caches", 0);
	walk("/private/var/db/lsd", 0);
	walk("/private/var/installd", 0);
	printf("rmls: %d files\n", n);
	return 0;
}

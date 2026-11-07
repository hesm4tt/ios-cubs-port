/* rmicons: removes SpringBoard's saved icon layout so that it is made afresh (apps that were left out for a missing capability come back) */
#include <stdio.h>
#include <unistd.h>
int main(void)
{
	static const char *const f[] = {
		"/private/var/mobile/Library/SpringBoard/IconState.plist", "/private/var/mobile/Library/SpringBoard/DesiredIconState.plist",
		"/private/var/mobile/Library/SpringBoard/IconSupportState.plist", "/private/var/mobile/Library/SpringBoard/DesiredIconSupportState.plist",
		"/private/var/mobile/Library/SpringBoard/ApplicationShortcuts.plist", NULL };

	for (int i = 0; f[i]; i++) {
		printf("rmicons: %s %s\n", f[i], unlink(f[i]) == 0 ? "removed" : "not there");
	}
	return 0;
}

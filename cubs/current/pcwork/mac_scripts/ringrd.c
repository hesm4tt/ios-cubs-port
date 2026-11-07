/* ringrd OFFSET LENGTH OUTFILE: reads the shared RAM (/dev/cubs_ram, span PA 0xab2000000) with pread */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int c, char **v)
{
	long off = strtol(v[1], NULL, 0), len = strtol(v[2], NULL, 0);
	char *b = malloc(len);
	int fd = open("/dev/cubs_ram", O_RDONLY), o = open(v[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	long d = 0;

	while (d < len) {
		ssize_t r = pread(fd, b + d, len - d, off + d);
		if (r <= 0) break;
		d += r;
	}
	write(o, b, d);
	printf("read %ld bytes\n", d);
	return 0;
}

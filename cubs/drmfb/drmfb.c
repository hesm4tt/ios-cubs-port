/* Put a linear dumb framebuffer on the panel through Linux's own DRM driver, fill it with a test pattern, and write the
 * physical page runs of the buffer to a file so XNU can draw into the same pages after the hand-over.
 * usage (as root, after "stop surfaceflinger; stop vendor.hwcomposer-3"): drmfb OUTFILE [hold]
 * With "hold" the process stays alive (closing the DRM fd would remove the framebuffer and blank the plane). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct res { uint64_t fb_p, crtc_p, conn_p, enc_p; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; };
struct mode { uint32_t clock; uint16_t hd, hss, hse, ht, hskew, vd, vss, vse, vt, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct conn { uint64_t enc_p, modes_p, props_p, pvals_p; uint32_t nmodes, nprops, nenc, enc_id, conn_id, type, type_id, connection, mmw, mmh, subpixel, pad; };
struct enc { uint32_t id, type, crtc_id, possible_crtcs, possible_clones; };
struct crtc { uint64_t set_conn_p; uint32_t nconn, crtc_id, fb_id, x, y, gamma, valid; struct mode m; };
struct cdumb { uint32_t h, w, bpp, flags, handle, pitch; uint64_t size; };
struct mdumb { uint32_t handle, pad; uint64_t offset; };
struct fbcmd { uint32_t fb_id, w, h, pitch, bpp, depth, handle; };

#define IOR(nr, t) _IOWR('d', nr, t)
#define GETRES   IOR(0xA0, struct res)
#define GETCRTC  IOR(0xA1, struct crtc)
#define SETCRTC  IOR(0xA2, struct crtc)
#define GETENC   IOR(0xA6, struct enc)
#define GETCONN  IOR(0xA7, struct conn)
#define ADDFB    IOR(0xAE, struct fbcmd)
#define CDUMB    IOR(0xB2, struct cdumb)
#define MDUMB    IOR(0xB3, struct mdumb)
#define SETMASTER _IO('d', 0x1e)

static int try_card(const char *path, int *fdo, struct mode *mo, uint32_t *conn_id, uint32_t *crtc_id)
{
	int fd = open(path, O_RDWR);
	struct res r; uint32_t crtcs[16], conns[16], encs[16], fbs[16];
	unsigned i;
	if (fd < 0) return -1;
	memset(&r, 0, sizeof r);
	if (ioctl(fd, GETRES, &r) < 0) { close(fd); return -1; }
	r.fb_p = 0; r.crtc_p = (uintptr_t)crtcs; r.conn_p = (uintptr_t)conns; r.enc_p = (uintptr_t)encs;
	if (r.ncrtc > 16 || r.nconn > 16 || r.nenc > 16) { close(fd); return -1; }
	(void)fbs; r.nfb = 0;
	if (ioctl(fd, GETRES, &r) < 0) { close(fd); return -1; }
	printf("%s: %u crtcs %u connectors %u encoders\n", path, r.ncrtc, r.nconn, r.nenc);
	for (i = 0; i < r.nconn; i++) {
		struct conn c; struct mode modes[32]; uint32_t cenc[8];
		memset(&c, 0, sizeof c);
		c.conn_id = conns[i];
		if (ioctl(fd, GETCONN, &c) < 0) continue;
		uint32_t nm = c.nmodes > 32 ? 32 : c.nmodes;
		c.modes_p = (uintptr_t)modes; c.nmodes = nm; c.props_p = 0; c.nprops = 0; c.pvals_p = 0;
		c.enc_p = (uintptr_t)cenc; c.nenc = c.nenc > 8 ? 8 : c.nenc;
		if (ioctl(fd, GETCONN, &c) < 0) continue;
		printf("  connector %u type %u connection %u modes %u encoder %u\n", c.conn_id, c.type, c.connection, nm, c.enc_id);
		if (c.connection == 1 && nm > 0) {
			struct enc e; uint32_t cr = 0;
			memset(&e, 0, sizeof e); e.id = c.enc_id;
			if (c.enc_id && ioctl(fd, GETENC, &e) == 0) cr = e.crtc_id;
			if (!cr) { unsigned k; for (k = 0; k < r.ncrtc; k++) if (e.possible_crtcs & (1u << k)) { cr = crtcs[k]; break; } }
			if (!cr) cr = crtcs[0];
			*mo = modes[0]; *conn_id = c.conn_id; *crtc_id = cr; *fdo = fd;
			printf("  using mode %ux%u@%u (%s) crtc %u\n", mo->hd, mo->vd, mo->vrefresh, mo->name, cr);
			return 0;
		}
	}
	close(fd);
	return -1;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "/data/local/tmp/fblist.bin";
	int hold = argc > 2;
	char path[32]; int fd = -1, i;
	struct mode mode; uint32_t conn_id = 0, crtc_id = 0;
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		if (try_card(path, &fd, &mode, &conn_id, &crtc_id) == 0) break;
	}
	if (fd < 0) { puts("no connected display found"); return 1; }
	if (ioctl(fd, SETMASTER, 0) < 0) perror("SET_MASTER (is surfaceflinger stopped?)");
	struct cdumb cd = { .w = mode.hd, .h = mode.vd, .bpp = 32 };
	if (ioctl(fd, CDUMB, &cd) < 0) { perror("create_dumb"); return 2; }
	printf("dumb %ux%u pitch %u size %llu handle %u\n", cd.w, cd.h, cd.pitch, (unsigned long long)cd.size, cd.handle);
	struct mdumb md = { .handle = cd.handle };
	if (ioctl(fd, MDUMB, &md) < 0) { perror("map_dumb"); return 3; }
	uint8_t *fb = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (fb == MAP_FAILED) { perror("mmap"); return 4; }
	/* test pattern: four colour bands, white border, diagonal stripe */
	for (uint32_t y = 0; y < cd.h; y++) {
		uint32_t *row = (uint32_t *)(fb + (uint64_t)y * cd.pitch);
		for (uint32_t x = 0; x < cd.w; x++) {
			uint32_t c = y < cd.h / 4 ? 0xff0000 : y < cd.h / 2 ? 0x00ff00 : y < 3 * cd.h / 4 ? 0x0000ff : 0xffff00;
			if (x < 8 || y < 8 || x >= cd.w - 8 || y >= cd.h - 8 || ((x + y) % 200) < 12) c = 0xffffff;
			row[x] = c;
		}
	}
	struct fbcmd fc = { .w = cd.w, .h = cd.h, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle };
	if (ioctl(fd, ADDFB, &fc) < 0) { perror("addfb"); return 5; }
	struct crtc sc; memset(&sc, 0, sizeof sc);
	uint32_t cl[1] = { conn_id };
	sc.set_conn_p = (uintptr_t)cl; sc.nconn = 1; sc.crtc_id = crtc_id; sc.fb_id = fc.fb_id; sc.valid = 1; sc.m = mode;
	if (ioctl(fd, SETCRTC, &sc) < 0) { perror("setcrtc"); return 6; }
	puts("setcrtc ok: the test pattern should be on the panel");
	/* physical pages of the buffer through pagemap */
	int pm = open("/proc/self/pagemap", O_RDONLY);
	uint64_t npages = (cd.size + 4095) / 4096, runs = 0;
	FILE *o = fopen(out, "wb");
	uint64_t hdr[8] = { 0x3130424643425543ULL /* "CUBFB01" */, 0, cd.w, cd.h, cd.pitch, 32, cd.size, 0 };
	fwrite(hdr, 8, 8, o);
	uint64_t run_pa = 0, run_len = 0;
	for (uint64_t p = 0; p < npages; p++) {
		uint64_t e = 0;
		volatile uint8_t t = fb[p * 4096]; (void)t;
		if (pread(pm, &e, 8, ((uintptr_t)fb / 4096 + p) * 8) != 8 || !(e >> 63)) { puts("pagemap failed"); return 7; }
		uint64_t pa = (e & ((1ULL << 55) - 1)) << 12;
		if (!pa) { puts("pagemap hides PFNs (not root?)"); return 8; }
		if (run_len && pa == run_pa + run_len) run_len += 4096;
		else {
			if (run_len) { uint64_t ent[2] = { run_pa, run_len }; fwrite(ent, 8, 2, o); runs++; }
			run_pa = pa; run_len = 4096;
		}
	}
	if (run_len) { uint64_t ent[2] = { run_pa, run_len }; fwrite(ent, 8, 2, o); runs++; }
	fseek(o, 8, SEEK_SET); fwrite(&runs, 8, 1, o); fclose(o);
	printf("%llu pages in %llu physical runs written to %s\n", (unsigned long long)npages, (unsigned long long)runs, out);
	if (hold) { puts("holding the DRM fd"); for (;;) sleep(1000); }
	return 0;
}

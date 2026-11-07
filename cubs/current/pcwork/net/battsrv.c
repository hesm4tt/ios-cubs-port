/* Android-owned, read-only power-supply telemetry for the iOS power source. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OFF 0xff4000
#define MAGIC 0x31544142u
struct state {
	uint32_t size;
	int32_t percent, voltage, current, temperature, present, external, status;
	int32_t charge, full, design, cycles, usb_type, usb_voltage, usb_current;
};
struct wire { uint32_t magic, sequence; struct state state; };

static int readnum(const char *path, int fallback)
{
	FILE *f = fopen(path, "r"); int value;
	if (!f) return fallback;
	int ok = fscanf(f, "%d", &value); fclose(f);
	return ok == 1 ? value : fallback;
}
static int status(void)
{
	char s[32] = ""; FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
	if (!f) return 0;
	(void)fgets(s, sizeof(s), f); fclose(f);
	if (!strncmp(s, "Charging", 8)) return 1;
	if (!strncmp(s, "Discharging", 11)) return 2;
	if (!strncmp(s, "Not charging", 12)) return 3;
	if (!strncmp(s, "Full", 4)) return 4;
	return 0;
}
/* sysfs lists supported types and brackets the active one. Never guess from
 * an unbracketed list; its entries are capabilities, not the current type. */
static int usb_type_from_text(const char *s)
{
    static const struct { const char *name; int value; } types[] = {
        {"SDP",1}, {"DCP",2}, {"CDP",3}, {"ACA",4}, {"C",5},
        {"PD",6}, {"PD_DRP",7}, {"PD_PPS",8}
    };
    const char *begin = strchr(s, '['), *end;
    if (!begin || !(end = strchr(++begin, ']'))) return 0;
    size_t n=(size_t)(end-begin);
    for (size_t i=0;i<sizeof(types)/sizeof(types[0]);i++)
        if (strlen(types[i].name)==n && !memcmp(begin,types[i].name,n)) return types[i].value;
    return 0;
}
static int usb_type(void)
{
    char s[256]; FILE *f=fopen("/sys/class/power_supply/usb/usb_type","r");
    if (!f) return 0;
    char *line=fgets(s,sizeof(s),f); fclose(f);
    return line ? usb_type_from_text(s) : 0;
}
int main(int argc, char **argv)
{
	const char *path = "/dev/cubs_ram";
#ifdef CUBS_BATTERY_TEST
	if (argc != 2) return 2;
	path = argv[1];
#else
	(void)argc; (void)argv;
#endif
	int fd = open(path, O_RDWR); uint32_t seq = 0;
	if (fd < 0) { perror("battsrv: open"); return 1; }
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (;;) {
		struct wire w = { .magic = MAGIC, .sequence = seq + 1 };
		int cap = readnum("/sys/class/power_supply/battery/capacity", -1);
		if (cap < 0 || cap > 100) { sleep(2); continue; }
		w.state.size = sizeof(w.state); w.state.percent = cap * 100;
		w.state.voltage = readnum("/sys/class/power_supply/battery/voltage_now", 0);
		w.state.current = readnum("/sys/class/power_supply/battery/current_now", 0);
		w.state.temperature = readnum("/sys/class/power_supply/battery/temp", 0) * 10;
		w.state.present = readnum("/sys/class/power_supply/battery/present", 0);
		w.state.external = readnum("/sys/class/power_supply/usb/online", 0) || readnum("/sys/class/power_supply/wireless/online", 0);
		w.state.status = status();
		w.state.charge = readnum("/sys/class/power_supply/battery/charge_counter", 0);
		w.state.full = readnum("/sys/class/power_supply/battery/charge_full", 0);
		w.state.design = readnum("/sys/class/power_supply/battery/charge_full_design", 0);
		w.state.cycles = readnum("/sys/class/power_supply/battery/cycle_count", 0);
		if (readnum("/sys/class/power_supply/usb/online", 0)) w.state.usb_type = usb_type();
		if (pwrite(fd, &w.sequence, sizeof(w.sequence), OFF + 4) != sizeof(w.sequence) ||
		    pwrite(fd, &w.magic, sizeof(w.magic), OFF) != sizeof(w.magic) ||
		    pwrite(fd, &w.state, sizeof(w.state), OFF + 8) != sizeof(w.state)) { perror("battsrv: sample"); return 1; }
		seq += 2;
		if (pwrite(fd, &seq, sizeof(seq), OFF + 4) != sizeof(seq)) { perror("battsrv: sequence"); return 1; }
		if (seq == 2) printf("battsrv: %d%% external %d status %d\n", cap, w.state.external, w.state.status);
#ifdef CUBS_BATTERY_TEST
		close(fd); return 0;
#endif
		sleep(2);
	}
}

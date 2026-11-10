/*
 * btbridge: iOS's Bluetooth stack (bluetoothd, BlueTool) to the OnePlus 7 Pro's WCN3998, through kernel 0220.
 *
 * BlueTool (transport-encoding 3) talks to a kernel control socket, com.apple.uart.bluetooth
 * (research/bluetooth/transport.md), not to /dev/cu.bluetooth as first thought; kernel 0220 (bsd/dev/sweet_btuart.c)
 * registers it and joins it to com.sweet.bt.host, which this daemon connects to (root). Both speak H4 HCI to what
 * they believe is an iPhone X's Broadcom BCM4357. Here the controller is a Qualcomm WCN3998 behind QUP wrapper 2 SE3,
 * driven through /dev/pmem by btuart.c's code (included). btbridge:
 *   1. brings the controller up as `btuart hello` + `load` do (power pulses, rampatch, NVM, HCI_Reset, QCA logs off);
 *   2. connects to com.sweet.bt.host; /dev/cu.bluetooth and /dev/uart.bluetooth are also made (links to /dev/null:
 *      bluetoothd checks that one of them exists at start, research/bluetooth/transport.md);
 *   3. moves H4 packets both ways. Commands of the vendor group (OGF 0x3f, opcodes 0xfc00-0xffff) are Broadcom's
 *      (BlueTool's D22 boot script: bcm -b 1500000 baud change, bcm -W firmware download, -B -A -N -Q -x, -s ...,
 *      research/bluetooth/bluetool.md) and mean something else to a Qualcomm chip, so they are NOT sent: btbridge
 *      answers Command Complete, status 0, itself. Every other command goes to the controller and its events come
 *      back. The controller's own log packets (ACL handle 0x2edc) are dropped.
 * The link to the controller runs at 3 Mbaud after the boot (btuart.c uart_fast), 115200 if that fails or with -s,
 * whatever the host side sets (the kernel control keeps its termios). 115200 (~11 KB/s) is too slow for audio.
 * A stream that stops making sense (bytes lost on the way, 2026-11-09 with AirPods) is skipped to the next plausible
 * packet and counted ("resyncs"), never a reason to stop.
 *
 *   btbridge          run in the foreground, one line per command (launchd job com.sweet.btbridge later)
 *   btbridge -q       only errors and a summary every 60 s
 *   btbridge -s       stay at 115200 (options combine: -q -s)
 *   btbridge -a       also one line per ACL packet (start of its data)
 * Needs Wi-Fi up (wland: the controller's rails) and /var/local/fw/bt/crbtfw21.tlv + crnv21.bin.
 * MIT, as userland/.
 */
#define BTUART_NO_MAIN
#include "btuart.c"

#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/file.h>

/* Kernel-control headers are private and absent from the iOS SDK. */
#define SYSPROTO_CONTROL 2
#define AF_SYS_CONTROL 2
#define MAX_KCTL_NAME 96
struct ctl_info { uint32_t ctl_id; char ctl_name[MAX_KCTL_NAME]; };
struct sockaddr_ctl { uint8_t sc_len, sc_family; uint16_t ss_sysaddr; uint32_t sc_id, sc_unit, sc_reserved[5]; };
#define CTLIOCGINFO _IOWR('N', 3, struct ctl_info)

static int quiet, acl_log;

/* -a: one line per ACL packet, the first 40 bytes (handle, L2CAP length + channel, then e.g. the AVDTP / SDP / AVCTP
 * signal), to see how far audio setup gets (2026-11-09: AirPods connected, encrypted, no audio, ~50 ACL each way) */
static void show_acl(const char *dir, const uint8_t *b, unsigned n)
{
	if (!acl_log)
		return;
	printf("ACL %s handle %03x len %u:", dir, (b[1] | b[2] << 8) & 0xfff, n);
	for (unsigned i = 5; i < n && i < 45; i++)
		printf(" %02x", b[i]);
	printf(n > 45 ? " ...\n" : "\n");
}

static const char *const links[] = { "/dev/cu.bluetooth", "/dev/uart.bluetooth" };
static const char bcm4357b1_hcd[] =
	"/usr/share/firmware/bluetooth/BCM4357B1_19.1.282.5154_Hercules_OS_USI_20210524.hcd";

/* BlueTool's bcm -W parser expects H4 command records in the HCD file. Our
 * bridge consumes Broadcom vendor commands locally, so a harmless one-record
 * placeholder is sufficient when the original Broadcom firmware is absent. */
static int ensure_hcd(void)
{
	static const uint8_t placeholder[] = { 0x01, 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };
	if (access(bcm4357b1_hcd, F_OK) == 0)
		return 0;
	if ((mkdir("/usr/share/firmware", 0755) && errno != EEXIST) ||
	    (mkdir("/usr/share/firmware/bluetooth", 0755) && errno != EEXIST)) {
		printf("btbridge: create HCD directory: %s\n", strerror(errno));
		return -1;
	}
	int fd = open(bcm4357b1_hcd, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		if (errno == EEXIST)
			return 0;
		printf("btbridge: create %s: %s\n", bcm4357b1_hcd, strerror(errno));
		return -1;
	}
	ssize_t n = write(fd, placeholder, sizeof(placeholder));
	int saved_errno = errno;
	close(fd);
	if (n != (ssize_t)sizeof(placeholder)) {
		unlink(bcm4357b1_hcd);
		printf("btbridge: write %s: %s\n", bcm4357b1_hcd,
		    n < 0 ? strerror(saved_errno) : "short write");
		return -1;
	}
	printf("btbridge: installed one-record stand-in HCD at %s\n", bcm4357b1_hcd);
	return 0;
}

static int boot_controller(int fast)
{
	static const uint8_t version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 }, reset[] = { 0x01, 0x03, 0x0c, 0x00 },
		logs_off[] = { 0x01, 0x17, 0xfc, 0x02, 0x14, 0x00 }, bdaddr[] = { 0x01, 0x09, 0x10, 0x00 };
	if (setup())
		return -1;
	pulse();
	exchange("QCA version request", version, sizeof(version), 500);
	exchange("HCI_Reset", reset, sizeof(reset), 500);
	uart_init(1);
	set_baud(115200);
	if (download("/var/local/fw/bt/crbtfw21.tlv", 0) || download("/var/local/fw/bt/crnv21.bin", 1)) {
		printf("btbridge: firmware download failed\n");
		return -1;
	}
	delay_ms(100);
	exchange("HCI_Reset", reset, sizeof(reset), 1000);
	exchange("QCA logging off", logs_off, sizeof(logs_off), 300);
	exchange("HCI_Read_BD_ADDR", bdaddr, sizeof(bdaddr), 300);
	/* whatever is still in the receiver (late download events, a log packet) is not the host's: drop it */
	uint8_t junk[4096];
	unsigned n = uart_read(junk, sizeof(junk), 300);
	if (n)
		printf("btbridge: %u bytes left over after boot, dropped\n", n);
	if (fast && uart_fast()) {
		printf("btbridge: 3 Mbaud failed: booting the controller again at 115200\n");
		return 1;
	}
	return 0;
}

static int open_host(void)
{
	int fd = socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL);
	struct ctl_info info;
	memset(&info, 0, sizeof(info));
	strlcpy(info.ctl_name, "com.sweet.bt.host", sizeof(info.ctl_name));
	if (fd < 0 || ioctl(fd, CTLIOCGINFO, &info) < 0) {
		printf("btbridge: no com.sweet.bt.host kernel control (%s): kernel 0220 not running?\n", strerror(errno));
		return -1;
	}
	struct sockaddr_ctl sc;
	memset(&sc, 0, sizeof(sc));
	sc.sc_len = sizeof(sc);
	sc.sc_family = AF_SYSTEM;
	sc.ss_sysaddr = AF_SYS_CONTROL;
	sc.sc_id = info.ctl_id;
	sc.sc_unit = 0;
	if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) < 0) {
		printf("btbridge: connect com.sweet.bt.host: %s\n", strerror(errno));
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	for (unsigned i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		unlink(links[i]);
		if (symlink("/dev/null", links[i]))
			printf("btbridge: symlink %s: %s\n", links[i], strerror(errno));
	}
	printf("btbridge: connected to com.sweet.bt.host; BlueTool / bluetoothd reach it as com.apple.uart.bluetooth\n");
	return fd;
}


/* 0x0c "Command Disallowed" for LE_Set_Scan_Parameters (2026-11-09, every time: 01 0b 20 07 01 40 00 30 00 01 00):
 * bluetoothd scans with own address type 1 (random), and sets its random address through Apple / Broadcom vendor
 * commands (0xfce9 ..., answered here), never with the standard LE_Set_Random_Address, so the WCN3998 has none and
 * refuses. Before a command that uses own address type random (scan parameters 0x200b, advertising parameters
 * 0x2006, create connection 0x200d), a static random address is set the standard way, once per controller reset. */
static int random_addr_set;
static uint8_t random_addr[6];

/* Controller -> host bytes not yet forwarded (the main loop's buffer). Commands btbridge sends on its own
 * (cmd_sync) read their answer INTO this buffer and take out only that answer: everything else that arrives
 * meanwhile stays, in order, for the main loop. Until 2026-11-09 they read into a private buffer and threw the rest
 * away: the Beats' AVDTP Get_All_Capabilities answer and an L2CAP Configure Request arrived during an LE scan
 * parameters exchange and were lost (bluetoothd: "Didn't get command response to transaction 4", A2DP dropped), and
 * likewise an Authentication Complete and a Connection Complete (bluetoothd restarts, reason 621). */
static uint8_t ctl[65536];
static unsigned nc;
static int h4_len(const uint8_t *b, unsigned n);

/* send an HCI command to the controller and wait (up to ms) for its Command Complete / Command Status; that event
 * is copied to ev (up to evmax bytes, length returned) and removed from ctl; 0 = none in time */
static unsigned cmd_sync(const uint8_t *cmd, unsigned n, uint8_t *ev, unsigned evmax, unsigned ms)
{
	uart_write(cmd, n);
	uint64_t t0 = now_ms();
	unsigned scanned = 0;           /* complete packets before this offset were already checked */
	do {
		nc += drain(ctl + nc, sizeof(ctl) - nc);
		for (;;) {
			int len = h4_len(ctl + scanned, nc - scanned);
			if (len <= 0)
				break;                  /* incomplete, or garbage: left for the main loop */
			const uint8_t *e = ctl + scanned;
			int mine = e[0] == 0x04 && ((e[1] == 0x0e && len >= 6 && e[4] == cmd[1] && e[5] == cmd[2]) ||
			    (e[1] == 0x0f && len >= 7 && e[5] == cmd[1] && e[6] == cmd[2]));
			if (mine) {
				unsigned k = (unsigned)len < evmax ? (unsigned)len : evmax;
				memcpy(ev, e, k);
				memmove(ctl + scanned, ctl + scanned + len, nc - scanned - (unsigned)len);
				nc -= (unsigned)len;
				return k;
			}
			scanned += (unsigned)len;
		}
	} while (now_ms() - t0 < ms && nc < sizeof(ctl));
	return 0;
}

static void ensure_random_address(void)
{
	if (random_addr_set)
		return;
	if (!random_addr[5]) {
		FILE *f = fopen("/dev/urandom", "rb");
		if (!f || fread(random_addr, 1, 6, f) != 6)
			memcpy(random_addr, "\x5a\x31\x9c\x07\x42\x00", 6);
		if (f)
			fclose(f);
		random_addr[5] |= 0xc0;                 /* static random: the two top bits set */
	}
	uint8_t cmd[10] = { 0x01, 0x05, 0x20, 0x06 }, r[64];
	memcpy(cmd + 4, random_addr, 6);
	unsigned n = cmd_sync(cmd, sizeof(cmd), r, sizeof(r), 300);
	int ok = n >= 7 && r[1] == 0x0e && r[6] == 0;
	printf("btbridge: LE random address %02x:%02x:%02x:%02x:%02x:%02x -> controller: %s\n", random_addr[5],
	    random_addr[4], random_addr[3], random_addr[2], random_addr[1], random_addr[0], ok ? "ok" : "no / odd reply");
	if (!ok && n)
		show("  reply", r, n);
	random_addr_set = ok;
}


/* LE_Set_Scan_Parameters, done here (2026-11-09: the WCN3998 answers it 0x0c Command Disallowed every time, also
 * with a random address set and no scan enable or extended command before it). Tries in turn, logging each answer:
 * scan off first (LE_Set_Scan_Enable 0, its answer swallowed) then the command as sent; then with own address type
 * public (0). bluetoothd gets the Command Complete of the last try. */
static int sync_cmd(const uint8_t *cmd, unsigned n, uint8_t *status)
{
	uint8_t r[64];
	unsigned got = cmd_sync(cmd, n, r, sizeof(r), 300);
	if (got >= 7 && r[1] == 0x0e) {
		*status = r[6];
		return 0;
	}
	if (got >= 4 && r[1] == 0x0f) {
		*status = r[3];
		return 0;
	}
	printf("btbridge: no answer to %02x%02x in 300 ms\n", cmd[2], cmd[1]);
	return -1;
}

static void scan_parameters(int m, const uint8_t *cmd, unsigned n)
{
	static const uint8_t off[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
	uint8_t st = 0xff, st_off = 0xff, c[16];
	memcpy(c, cmd, n);
	sync_cmd(off, sizeof(off), &st_off);
	sync_cmd(c, n, &st);
	printf("btbridge: LE scan parameters: scan off -> %#x, as sent (own address type %u) -> %#x\n", st_off, c[9], st);
	if (st == 0x0c && c[9] != 0) {
		c[9] = 0;
		sync_cmd(c, n, &st);
		printf("btbridge: LE scan parameters with own address type public -> %#x\n", st);
	}
	uint8_t ev[] = { 0x04, 0x0e, 0x04, 0x01, 0x0b, 0x20, st };
	if (write(m, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		printf("btbridge: write: %s\n", strerror(errno));
}

/* Command Complete for a command we answer ourselves */
static void fake_complete(int m, uint16_t opcode)
{
	uint8_t ev[] = { 0x04, 0x0e, 0x04, 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), 0x00 };
	if (write(m, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		printf("btbridge: write: %s\n", strerror(errno));
}

/* A Broadcom vendor command answered here, with return parameters: status 0, then the command's first parameter
 * byte (Broadcom's multi-function commands, e.g. BD_VSC_LE_META_ENA_FEATURES 0xfd48, echo their sub-opcode), then
 * zeros. bluetoothd's bcm_vsc.c reads at least one byte past the status for some (2026-11-09: "AssertFail
 * bcm_vsc.c:642 : ByteStream_NumReadBytesAvail(*pBs) >= 1" after LE_META_ENA_FEATURES, then a restart every ~10 s);
 * a parser reading less ignores the rest. */
static void fake_vendor_complete(int m, uint16_t opcode, const uint8_t *params, unsigned nparams)
{
	uint8_t ev[7 + 16] = { 0x04, 0x0e, 3 + 1 + 16, 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), 0x00 };
	memset(ev + 7, 0, 16);
	if (nparams > 0)
		ev[7] = params[0];
	if (write(m, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		printf("btbridge: write: %s\n", strerror(errno));
}

/* Read Local Version Information: identify as the Broadcom D22 profile that
 * BlueTool's getChipset() expects, while retaining the actual Qualcomm radio. */
static void fake_local_version(int m)
{
	static const uint8_t event[] = {
		0x04, 0x0e, 0x0c, 0x01, 0x01, 0x10,   /* Command Complete for opcode 0x1001 (OGF 4, OCF 1) */
		0x00, 0x09, 0x00, 0x00, 0x09, 0x0f, 0x00, 0x0e, 0x42
	};
	if (write(m, event, sizeof(event)) != (ssize_t)sizeof(event))
		printf("btbridge: Read Local Version reply: %s\n", strerror(errno));
}

#define H4_ACL_MAX 4096        /* far above both sides' ACL buffer sizes (~1 KB) */

/* length of the H4 packet at b (n bytes there), 0 when incomplete, -1 when it is not H4 */
static int h4_len(const uint8_t *b, unsigned n)
{
	if (n < 1)
		return 0;
	switch (b[0]) {
	case 0x01:
		return n < 4 ? 0 : 4 + b[3] <= (int)n ? 4 + b[3] : 0;
	case 0x02:
		if (n >= 5 && (b[3] | b[4] << 8) > H4_ACL_MAX)
			return -1;                                  /* no real ACL packet is that long: lost bytes */
		return n < 5 ? 0 : 5 + (b[3] | b[4] << 8) <= (int)n ? 5 + (b[3] | b[4] << 8) : 0;
	case 0x03:
		return n < 4 ? 0 : 4 + b[3] <= (int)n ? 4 + b[3] : 0;
	case 0x04:
		return n < 3 ? 0 : 3 + b[2] <= (int)n ? 3 + b[2] : 0;
	default:
		return -1;
	}
}

int main(int argc, char **argv)
{
	int fast = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-q"))
			quiet = 1;
		else if (!strcmp(argv[i], "-s"))
			fast = 0;
		else if (!strcmp(argv[i], "-a"))
			acl_log = 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* one instance: a second one would reset the controller under the first (launchd job com.sweet.btbridge) */
	int lock = open("/var/run/btbridge.lock", O_CREAT | O_RDWR, 0644);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
		printf("btbridge: already running (/var/run/btbridge.lock); to run by hand: "
		    "launchctl bootout system/com.sweet.btbridge\n");
		return 1;
	}
	pm_init();
	int b = boot_controller(fast);
	if (b == 1)
		b = boot_controller(0);
	if (b)
		return 1;
	if (ensure_hcd())
		return 1;
	int m = open_host();
	if (m < 0)
		return 1;

	static uint8_t host[65536];
	unsigned nh = 0;
	unsigned long cmds = 0, faked = 0, events = 0, acl_in = 0, acl_out = 0, dropped = 0, resyncs = 0;
	int in_garbage = 0;
	uint64_t last_summary = now_ms(), idle_since = now_ms();
	for (;;) {
		int busy = 0;
		/* host -> controller */
		if (nh == sizeof(host)) {
			/* full without one complete packet: cannot be a real stream (read(.., 0) would then return 0 and
			 * look like a closed socket, which is how the first AirPods test ended) */
			printf("btbridge: host buffer full without a complete packet: %u bytes dropped\n", nh);
			nh = 0;
			resyncs++;
		}
		ssize_t r = read(m, host + nh, sizeof(host) - nh);
		if (r > 0) {
			nh += (unsigned)r;
			busy = 1;
		} else if (r == 0) {
			printf("btbridge: the kernel control closed\n");
			return 1;
		} else if (r < 0 && errno != EAGAIN) {
			printf("btbridge: read: %s\n", strerror(errno));
		}
		for (;;) {
			int len = h4_len(host, nh);
			if (len < 0 || (nh > 0 && host[0] == 0x04)) {    /* the host never sends events */
				if (!in_garbage) {
					printf("btbridge: host stream out of step at %02x %02x %02x %02x %02x (%u bytes waiting): "
					    "skipping to the next packet\n", host[0], nh > 1 ? host[1] : 0, nh > 2 ? host[2] : 0,
					    nh > 3 ? host[3] : 0, nh > 4 ? host[4] : 0, nh);
					resyncs++;
				}
				in_garbage = 1;
				memmove(host, host + 1, --nh);
				continue;
			}
			if (len > 0)
				in_garbage = 0;
			if (len == 0)
				break;
			if (host[0] == 0x01) {
				uint16_t op = (uint16_t)(host[1] | host[2] << 8);
				cmds++;
				/* HCI_Read_Local_Version_Information is opcode 0x1001 (bytes 01 10 on the wire; BlueTool sent
				 * 01 01 10 00). 0x1002 is Read_Local_Supported_Commands: the first build matched that, so the
				 * version still went to the WCN3998 and BlueTool kept "chip = ALL". */
				if (op == 0x1001) {
					faked++;
					if (!quiet)
						show("Read Local Version (BCM4357B1 profile)", host, (unsigned)len);
					fake_local_version(m);
				} else if (((op == 0x200b && len >= 11 && (host[9] & 1)) ||     /* scan: own address type at [9] */
				    (op == 0x2006 && len >= 10 && (host[9] & 1)) ||         /* advertising: [9] */
				    (op == 0x200d && len >= 17 && (host[16] & 1))) &&       /* create connection: [16] */
				    (ensure_random_address(), 0)) {
					/* not reached: ensure_random_address() runs as a side effect, the command goes on below */
				} else if (op == 0x204a) {
					/* LE_Read_Periodic_Advertiser_List_Size is one of Bluetooth 5's extended / periodic advertising
					 * commands: once a host uses any of them, the controller must refuse the legacy LE scan and
					 * advertising commands, and the WCN3998 does ("Command Disallowed" for LE_Set_Scan_Parameters and
					 * even LE_Set_Scan_Enable 0, 2026-11-09; bluetoothd restart reason 712). bluetoothd sends it
					 * during init (01 4a 20 00) and then scans the legacy way, so it is answered here (size 3, the
					 * chip's own answer) and the chip stays in legacy mode. */
					faked++;
					if (!quiet)
						show("host LE_Read_Periodic_Advertiser_List_Size (answered here)", host, (unsigned)len);
					uint8_t ev[] = { 0x04, 0x0e, 0x05, 0x01, 0x4a, 0x20, 0x00, 0x03 };
					if (write(m, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
						printf("btbridge: write: %s\n", strerror(errno));
				} else if (op == 0x200b && len == 11) {
					if (host[9] & 1)
						ensure_random_address();
					if (!quiet)
						show("host command (LE scan parameters, done here)", host, (unsigned)len);
					scan_parameters(m, host, (unsigned)len);
				} else if (op == 0xfc01 && len == 10) {
					/* Broadcom Write_BD_ADDR (BlueTool: 01 01 fc 06 + address, little-endian) -> the WCN3998's own
					 * (Linux btqca.c qca_set_bdaddr: vendor 0xfc14, the same 6 bytes); its Command Complete for
					 * 0xfc14 is swallowed and BlueTool gets one for 0xfc01. */
					uint8_t q[10] = { 0x01, 0x14, 0xfc, 0x06 };
					memcpy(q + 4, host + 4, 6);
					uint8_t r2[64];
					unsigned got2 = cmd_sync(q, sizeof(q), r2, sizeof(r2), 300);
					printf("btbridge: BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x -> controller (QCA 0xfc14): %s\n", host[9],
					    host[8], host[7], host[6], host[5], host[4],
					    got2 >= 7 && r2[1] == 0x0e && r2[6] == 0 ? "ok" : "no / odd reply");
					if (got2 && !quiet)
						show("  reply", r2, got2);
					faked++;
					fake_complete(m, op);
				} else if ((op >> 10) == 0x3f) {
					faked++;
					if (!quiet)
						show("host vendor command (answered here)", host, (unsigned)len);
					fake_vendor_complete(m, op, host + 4, host[3]);
				} else {
					if (op == 0x0c03)
						random_addr_set = 0;            /* HCI_Reset clears the controller's random address */
					if (!quiet)
						show("host command", host, (unsigned)len);
					uart_write(host, (unsigned)len);
				}
			} else {
				if (host[0] == 0x02)
					acl_out++;
				if (host[0] == 0x02)
					show_acl("out", host, (unsigned)len);
				uart_write(host, (unsigned)len);
			}
			memmove(host, host + len, nh - (unsigned)len);
			nh -= (unsigned)len;
		}
		/* controller -> host */
		unsigned got = drain(ctl + nc, sizeof(ctl) - nc);
		if (got) {
			nc += got;
			busy = 1;
		}
		for (;;) {
			int len = h4_len(ctl, nc);
			if (len < 0) {
				dropped++;
				memmove(ctl, ctl + 1, --nc);
				continue;
			}
			if (len == 0)
				break;
			int forward = 1;
			if (ctl[0] == 0x02 && ((ctl[1] | ctl[2] << 8) & 0x0fff) == 0x0edc) {
				forward = 0;                            /* QCA firmware log (handle 0x2edc) */
				dropped++;
			} else if (ctl[0] == 0x04) {
				events++;
				/* A command the WCN3998 does not know: bluetoothd's stack treats a failure in its init sequence as
				 * fatal (2026-11-09: "OI_HCICMD_WriteDefaultErroneousDataReporting (status=628)", "HCI
				 * initialization failed", restart). Command Complete with status 0x01 (Unknown HCI Command) for a
				 * standard command is turned into success; logged. */
				/* Read_Local_Supported_Commands (0x1002): bluetoothd's stack checks this mask and fails a command the
				 * controller does not claim WITHOUT sending it (2026-11-09: Write_Default_Erroneous_Data_Reporting,
				 * "status=628", never on the wire; the WCN3998's octet 18 is 0x83, bits 2-3 clear). The bits it
				 * needs are added here (octet N is ctl[7 + N]); the chip's own "unknown command" is answered as
				 * success below. Octet 18 bit 2 Read / bit 3 Write_Default_Erroneous_Data_Reporting. */
				if (ctl[1] == 0x0e && len >= 7 + 64 && ctl[4] == 0x02 && ctl[5] == 0x10 && ctl[6] == 0x00) {
					ctl[7 + 18] |= 0x0c;
					printf("btbridge: supported commands: octet 18 -> %02x (erroneous data reporting claimed)\n",
					    ctl[7 + 18]);
				}
				if (ctl[1] == 0x0e && len >= 7 && ctl[6] == 0x01) {
					printf("btbridge: controller does not know opcode %02x%02x: answered success instead\n", ctl[5],
					    ctl[4]);
					ctl[6] = 0x00;
				}
				if (!quiet)
					show("controller event", ctl, (unsigned)len);
			} else if (ctl[0] == 0x02) {
				acl_in++;
				show_acl("in ", ctl, (unsigned)len);
			}
			/* with no iOS side connected the kernel drops it (and counts it) */
			if (forward && write(m, ctl, (size_t)len) != len)
				printf("btbridge: write: %s\n", strerror(errno));
			memmove(ctl, ctl + len, nc - (unsigned)len);
			nc -= (unsigned)len;
		}
		if (nc == sizeof(ctl))
			nc = 0;
		uint64_t t = now_ms();
		if (busy)
			idle_since = t;
		else
			usleep(t - idle_since > 1000 ? 5000 : 500);    /* poll fast while traffic is recent */
		if (t - last_summary >= 60000) {
			printf("btbridge: %lu commands (%lu answered here), %lu events, ACL %lu in / %lu out, %lu dropped, "
			    "%lu resyncs\n", cmds, faked, events, acl_in, acl_out, dropped, resyncs);
			last_summary = t;
		}
	}
}

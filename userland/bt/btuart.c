/*
 * btuart: the OnePlus 7 Pro's Bluetooth controller (WCN3990) over its UART, from userland through /dev/pmem
 * (kernel 0138+, boot-arg sweetpmem=1, root), with wland's pmem.c. Step 2 of Bluetooth (step 1: btprobe).
 *
 * Facts (btprobe, 2026-11-09): the UART is QUP wrapper 2 SE3 at 0xc8c000 with UART firmware loaded (GENI FW
 * revision 0x20b: protocol 2), wrapper HW version 1.3 (so 32x oversampling, Linux qcom_geni_serial: < 2.5), FIFOs
 * 16 TX / 32 RX words (SE_HW_PARAM_0/1). Its pins GPIO43 TX, 44 RTS, 45 RX, 46 CTS are in the EAST TLMM tile
 * (0x03500000), function 1 = qup13 (Linux pinctrl-sm8150.c); left by Android's boot as GPIO with pulls. The
 * controller's rails are wland's (Wi-Fi): this assumes Wi-Fi is up.
 *
 * Clock: the SE's source (GCC RCG 0x1e4d8) is set to 7.3728 MHz = GPLL0 600 MHz x 384 / 31250 (M/N, as Linux's
 * frequency table), so every UART rate used here divides exactly: 2400 (/96), 115200 (/2), 230400 (/1), at 32x.
 * GENI UART in FIFO mode, polled, 8N1, bytes LSB first (Linux qcom_geni_serial.c / geni-se.c).
 *
 * WCN3990 start (Linux hci_qca.c qca_regulator_init / qca_send_power_pulse): no flow control; 0xC0 (power-off
 * pulse) at 2400 baud; 0xFC (power-on pulse) at 115200; 100 ms; then H4 HCI at 115200.
 *
 *   btuart setup           clocks, pins, UART at 115200 (8N1, no flow control), receiver on
 *   btuart pulse           the power-off / power-on pulses, then print anything the controller sends (500 ms)
 *   btuart version         QCA "patch version" request (HCI vendor command 0xFC00, sub-command 0x19)
 *   btuart reset           HCI_Reset (expect 04 0e 04 01 03 0c 00)
 *   btuart send HEX...     send these bytes (H4: first byte 01 = command), print the reply (500 ms)
 *   btuart listen [MS]     print what arrives
 *   btuart hello           setup + pulse + version + reset (send all of the output)
 *   btuart board           QCA board ID (vendor 0xFC00, sub-command 0x23): picks the NVM variant
 *   btuart load [NVM]      after hello: rampatch /var/local/fw/bt/crbtfw21.tlv, then NVM (default crnv21.bin),
 *                          HCI_Reset, local version and BD_ADDR
 *   btuart scan [SECS]     after load: classic inquiry + LE active scan for SECS (default 12) seconds, one line per
 *                          device (address, RSSI, name, LE/classic)
 *   btuart regs            the SE's registers
 * 2026-11-09 hello: the controller answers. QCA version event: product 0x0a, patch 0x6699, ROM 0x1001, SoC ID
 * 0x40010224 = WCN3998 ("Cherokee 2.1": Android's patch_dl_manager CHEROKEE_VER_2_1 -> crbtfw21.tlv, crnv21.*),
 * and HCI_Reset completes (04 0e 04 01 03 0c 00).
 *
 * Download (Linux btqca.c qca_download_firmware / qca_tlv_send_segment): the whole TLV file, header included, in
 * segments of <= 243 bytes as vendor command 0xFC00 [0x1E, length, data]; the WCN399x answers only the last one
 * (Linux sends the others without waiting), so flow control (CTS) is on for it and events are drained between
 * segments. NVM tags as Linux for a WCN3998 (< WCN3991): tag 17 (HCI) byte 2 = the UART baud rate after the
 * download, set to 0 (115200) here so the link stays at this speed; Linux's in-band-sleep (tag 17 byte 0 bit 7) and
 * deep-sleep (tag 27) changes are NOT made: this host does not speak the sleep protocol.
 * 2026-11-09 first load: rampatch accepted (04 ff 03 00 04 00 = download result 0), but crnv21.bin is TLV type 4
 * (a container; its NVM tags are in an inner type-2 entry), so tag 17 was not patched: it asks for in-band sleep and
 * 3.2 Mbaud (82 01 11), and the link went silent after the reset. Now patched in the inner entry too: IBS off, 115200.
 * Second load (NVM patched): HCI_Reset completes, Local Version: HCI 9 (5.0), manufacturer 0x001d (Qualcomm), LMP
 * subversion 0x02be; BD_ADDR 39:90:21:74:07:00 (the NVM default). The firmware then streams its logs as ACL packets
 * on handle 0x2edc (Linux QCA_DEBUG_HANDLE); scan turns them off (vendor 0xFC17 [0x14, 0x00], Linux
 * qca_disable_soc_logging) and skips any that arrive.
 * Board ID request (0x23) answered 04 ff 05 00 00 10 0a 03 and status 01: not used.
 * Undo: reboot (or Wi-Fi off removes the controller's power).
 * MIT, as userland/.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pmem.h"

#define GCC             0x00100000ull
#define GCC_WRAP2_VOTE  0x52014u
#define SE_INDEX        3u
#define RCG             (0x1e148u + 0x130u * SE_INDEX)  /* 0x1e4d8: cmd, +4 cfg, +8 M, +0xc N, +0x10 D */
#define BRANCH          (RCG - 4u)
#define SE              0x00c8c000ull
#define TLMM_EAST       0x03500000ull

/* GENI */
#define GENI_STATUS             0x040
#define   M_CMD_ACTIVE          (1u << 0)
#define   S_CMD_ACTIVE          (1u << 12)
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_SER_S_CLK_CFG      0x04c
#define SE_GENI_CLK_SEL         0x07c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL      0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL      0x634
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_EN        0x644
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFO         0x700
#define SE_GENI_RX_FIFO         0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define   M_CMD_DONE            (1u << 0)
#define   CMD_CANCEL            (1u << 2)
#define   OPCODE_SHIFT          27
#define   UART_START_TX         1u
#define   UART_START_READ       1u
#define   RX_WC                 0x1ffffffu
#define   RX_LAST               (1u << 31)
#define   RX_LAST_BYTES(s)      (((s) >> 28) & 7u)
/* UART */
#define SE_UART_TX_TRANS_CFG    0x25c
#define   UART_CTS_MASK         (1u << 1)
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_UART_MANUAL_RFR      0x2ac

#define SRC_HZ          7372800u
#define OVERSAMPLE      32u
#define TX_FIFO_WORDS   16u

static uint32_t se_rd(uint32_t off) { return rd32(SE + off); }
static void se_wr(uint32_t off, uint32_t v) { wr32(SE + off, v); }

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* the SE's source clock: GPLL0 main 600 MHz (source 1) / 1 (pre-divider field 1) x M / N, dual-edge M/N mode
 * (clk-rcg2.c). 384/31250 = 7.3728 MHz (2400, 115200); 4/25 = 96 MHz (3 Mbaud: 96 MHz / 32 / 1). */
static unsigned src_hz = SRC_HZ;

static int rcg_set(uint32_t m, uint32_t n, unsigned hz)
{
	const uint32_t mask = 0xffff;
	uint32_t d = n;                                 /* "2d" for 50 % duty, clamped to [m, n - m + 1] as clk-rcg2.c */
	if (d > n - m + 1)
		d = n - m + 1;
	if (d < m)
		d = m;
	wr32(GCC + RCG + 0x8, m);
	wr32(GCC + RCG + 0xc, ~(n - m) & mask);
	wr32(GCC + RCG + 0x10, ~d & mask);
	uint32_t cfg = rd32(GCC + RCG + 4);
	cfg = (cfg & ~(0x1fu | (7u << 8) | (3u << 12) | (1u << 20))) | 1u | (1u << 8) | (2u << 12);
	wr32(GCC + RCG + 4, cfg);
	wr32(GCC + RCG, rd32(GCC + RCG) | 1u);         /* UPDATE */
	if (wait_bit(GCC + RCG, 1u, 0, 100)) {
		printf("SE3 RCG update did not finish (cmd %08x)\n", rd32(GCC + RCG));
		return -1;
	}
	src_hz = hz;
	printf("clock: vote %#x, branch %08x, RCG cfg %08x M %u N %u -> %u Hz\n", rd32(GCC + GCC_WRAP2_VOTE),
	    rd32(GCC + BRANCH), rd32(GCC + RCG + 4), m, n, hz);
	return 0;
}

static int clocks(void)
{
	uint32_t v = rd32(GCC + GCC_WRAP2_VOTE);
	wr32(GCC + GCC_WRAP2_VOTE, v | 0x0f | (1u << (4 + SE_INDEX)));
	if (wait_bit(GCC + BRANCH, 1u << 31, 0, 100)) {
		printf("SE3 clock branch does not run (%08x)\n", rd32(GCC + BRANCH));
		return -1;
	}
	if (rd32(GCC + RCG + 0x14) & 1) {
		printf("SE3 RCG is in DFS mode (unexpected; btprobe saw it off): not reprogrammed\n");
		return -1;
	}
	return rcg_set(384, 31250, 7372800u);
}

static void pins(void)
{
	/* function 1 (qup13); TX no pull, RTS / CTS pull-down, RX pull-up (idle high); 2 mA (drive field 0) */
	static const struct { unsigned gpio; uint32_t cfg; } p[] = {
		{ 43, (1u << 2) | 0 }, { 44, (1u << 2) | 1 }, { 45, (1u << 2) | 3 }, { 46, (1u << 2) | 1 },
	};
	for (unsigned i = 0; i < 4; i++) {
		uint64_t a = TLMM_EAST + 0x1000ull * p[i].gpio;
		uint32_t old = rd32(a);
		wr32(a, (old & ~0x7ffu) | p[i].cfg);
		printf("pin GPIO%u %03x -> %03x\n", p[i].gpio, old & 0x7ff, rd32(a) & 0x7ff);
	}
}

static void rx_stop(void)
{
	if (!(se_rd(GENI_STATUS) & S_CMD_ACTIVE))
		return;
	se_wr(SE_GENI_S_CMD_CTRL, CMD_CANCEL);
	uint64_t t = now_ms();
	while ((se_rd(GENI_STATUS) & S_CMD_ACTIVE) && now_ms() - t < 50)
		;
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
}

static void rx_start(void)
{
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);
}

static void set_baud(unsigned baud)
{
	unsigned div = src_hz / (baud * OVERSAMPLE);
	rx_stop();
	se_wr(SE_GENI_CLK_SEL, 0);
	se_wr(GENI_SER_M_CLK_CFG, (div << 4) | 1u);
	se_wr(GENI_SER_S_CLK_CFG, (div << 4) | 1u);
	rx_start();
	printf("baud %u (divider %u%s)\n", baud, div, div * baud * OVERSAMPLE == src_hz ? "" : ", NOT exact");
}

static void uart_init(int flow)
{
	rx_stop();
	/* FIFO mode, 8-bit bytes four to a word, LSB first (geni_se_config_packing(8, 4, false)) */
	se_wr(SE_GENI_DMA_MODE_EN, 0);
	se_wr(SE_GENI_TX_PACKING_CFG0, 0x4380e);
	se_wr(SE_GENI_TX_PACKING_CFG1, 0xc3e0e);
	se_wr(SE_GENI_RX_PACKING_CFG0, 0x4380e);
	se_wr(SE_GENI_RX_PACKING_CFG1, 0xc3e0e);
	se_wr(SE_GENI_RX_WATERMARK, 2);
	se_wr(SE_GENI_RX_RFR_WATERMARK, 32 - 2);
	/* 8N1; CTS honoured only with flow control */
	se_wr(SE_UART_TX_TRANS_CFG, flow ? 0 : UART_CTS_MASK);
	se_wr(SE_UART_TX_PARITY_CFG, 0);
	se_wr(SE_UART_RX_TRANS_CFG, 0);
	se_wr(SE_UART_RX_PARITY_CFG, 0);
	se_wr(SE_UART_TX_WORD_LEN, 8);
	se_wr(SE_UART_RX_WORD_LEN, 8);
	se_wr(SE_UART_TX_STOP_BIT_LEN, 0);
	se_wr(SE_UART_RX_STALE_CNT, 10 * 16);
	se_wr(SE_UART_MANUAL_RFR, 0);                  /* RFR (our RTS) by the hardware, from the RX watermark */
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	printf("uart: FIFO mode, 8N1, flow control %s\n", flow ? "on" : "off");
}

static int uart_write(const uint8_t *b, unsigned n)
{
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_UART_TX_TRANS_LEN, n);
	se_wr(SE_GENI_M_CMD0, UART_START_TX << OPCODE_SHIFT);
	unsigned i = 0;
	uint64_t t = now_ms();
	while (i < n) {
		if ((se_rd(SE_GENI_TX_FIFO_STATUS) & 0x0fffffffu) >= TX_FIFO_WORDS) {
			if (now_ms() - t > 1000) {
				printf("uart: TX FIFO stuck (CTS held?)\n");
				return -1;
			}
			continue;
		}
		uint32_t w = 0;
		for (unsigned k = 0; k < 4 && i < n; k++, i++)
			w |= (uint32_t)b[i] << (8 * k);
		se_wr(SE_GENI_TX_FIFO, w);
	}
	while (!(se_rd(SE_GENI_M_IRQ_STATUS) & M_CMD_DONE)) {
		if (now_ms() - t > 2000) {
			printf("uart: TX not done (M_IRQ %08x, status %08x)\n", se_rd(SE_GENI_M_IRQ_STATUS), se_rd(GENI_STATUS));
			return -1;
		}
	}
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	return 0;
}

/* what arrives within ms (printed as hex) */
static unsigned uart_read(uint8_t *buf, unsigned max, unsigned ms)
{
	unsigned got = 0;
	uint64_t t = now_ms();
	while (now_ms() - t < ms && got < max) {
		uint32_t st = se_rd(SE_GENI_RX_FIFO_STATUS);
		unsigned wc = st & RX_WC;
		if (!wc)
			continue;
		for (unsigned i = 0; i < wc; i++) {
			uint32_t w = se_rd(SE_GENI_RX_FIFO);
			unsigned bytes = 4;
			if (i == wc - 1 && (st & RX_LAST) && RX_LAST_BYTES(st))
				bytes = RX_LAST_BYTES(st);
			for (unsigned k = 0; k < bytes && got < max; k++)
				buf[got++] = (uint8_t)(w >> (8 * k));
		}
		se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
		t = now_ms() - (ms > 200 ? ms - 200 : 0);       /* keep listening 200 ms after the last bytes */
	}
	return got;
}

static void show(const char *what, const uint8_t *b, unsigned n)
{
	printf("%s (%u bytes):", what, n);
	for (unsigned i = 0; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

static void exchange(const char *what, const uint8_t *cmd, unsigned n, unsigned ms)
{
	uint8_t r[1024];
	show(what, cmd, n);
	if (uart_write(cmd, n))
		return;
	unsigned got = uart_read(r, sizeof(r), ms);
	if (got)
		show("  reply", r, got);
	else
		printf("  no reply in %u ms\n", ms);
}


/* everything already received (no waiting beyond the stale timer) */
static unsigned drain(uint8_t *buf, unsigned max)
{
	return uart_read(buf, max, 3);
}

static uint8_t *load_file(const char *path, unsigned *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)n);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = (unsigned)n;
	return b;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* one TLV file to the controller; returns 0 when the last segment was answered */
static int download(const char *path, int nvm)
{
	unsigned len;
	uint8_t *fw = load_file(path, &len);
	if (!fw)
		return -1;
	uint32_t tl = le32(fw);
	unsigned type = tl & 0xff, tlen = tl >> 8;
	printf("%s: %u bytes, TLV type %u length %u\n", path, len, type, tlen);
	if (type == 1 && len >= 4 + 24) {
		const uint8_t *h = fw + 4;
		printf("  patch: total %u, data %u, format %u, signature %u, download mode %u, product %#x, ROM build %#x, "
		    "patch version %#x, entry %#x\n", le32(h), le32(h + 4), h[8], h[9], h[10], le16(h + 12), le16(h + 14),
		    le16(h + 16), le32(h + 20));
	} else if (nvm && (type == 2 || type == 4)) {
		/* type 2: NVM tags directly; type 4 (this phone's crnv21.*): a container of TLVs, the NVM tags in its type-2
		 * entry. Tag: id u16, length u16, 8 reserved bytes, data. */
		unsigned pos = type == 2 ? 0 : 4, end = type == 2 ? 4 + tlen : len;
		while (pos + 4 <= end) {
			uint32_t itl = type == 2 ? (2u | (tlen << 8)) : le32(fw + pos);
			unsigned itype = itl & 0xff, ilen = itl >> 8;
			uint8_t *d = type == 2 ? fw + 4 : fw + pos + 4;
			if (type == 4)
				printf("  container entry: TLV type %u length %u\n", itype, ilen);
			if (itype == 2) {
				unsigned idx = 0, tags = 0;
				while (idx + 12 <= ilen && d + idx + 12 <= fw + len) {
					uint8_t *t = d + idx;
					unsigned id = le16(t), tagl = le16(t + 2);
					if (id == 2 && tagl == 6)
						printf("  NVM tag 2 (BD_ADDR): %02x:%02x:%02x:%02x:%02x:%02x\n", t[17], t[16], t[15], t[14],
						    t[13], t[12]);
					if (id == 17 && tagl >= 3) {
						/* Linux: data[0] bit 7 = in-band sleep (IBS), data[2] = UART baud code after the download.
						 * This file asks for IBS and 0x11 (3.2 Mbaud): the first load lost the link right after. */
						printf("  NVM tag 17 (HCI): %02x %02x %02x -> %02x %02x 00 (in-band sleep off, 115200)\n",
						    t[12], t[13], t[14], t[12] & 0x7f, t[13]);
						t[12] &= 0x7f;
						t[14] = 0;
					}
					if (id == 27 && tagl >= 1) {
						/* deep sleep on (01) goes with in-band sleep (the host wakes it with IBS bytes): off with it */
						printf("  NVM tag 27 (deep sleep): %02x -> %02x\n", t[12], t[12] & 0xfe);
						t[12] &= 0xfe;
					}
					idx += 12 + tagl;
					tags++;
				}
				printf("  %u NVM tags\n", tags);
			}
			if (type == 2)
				break;
			pos += 4 + ilen;
		}
	}
	uint8_t ev[4096];
	unsigned evn = 0, segs = 0;
	uint64_t t0 = now_ms();
	for (unsigned off = 0; off < len; off += 243) {
		unsigned seg = len - off < 243 ? len - off : 243;
		uint8_t cmd[4 + 2 + 243] = { 0x01, 0x00, 0xfc, (uint8_t)(seg + 2), 0x1e, (uint8_t)seg };
		memcpy(cmd + 6, fw + off, seg);
		if (uart_write(cmd, 6 + seg)) {
			printf("  segment %u: write failed\n", segs);
			free(fw);
			return -1;
		}
		segs++;
		evn += drain(ev + evn, sizeof(ev) - evn);
	}
	evn += uart_read(ev + evn, sizeof(ev) - evn, 2000);
	printf("  %u segments in %llu ms; %u bytes of events back\n", segs, (unsigned long long)(now_ms() - t0), evn);
	if (evn)
		show("  events (last 64 bytes)", ev + (evn > 64 ? evn - 64 : 0), evn > 64 ? 64 : evn);
	free(fw);
	return evn ? 0 : -1;
}


/* ---- scan: one H4 packet stream parser ---- */
struct dev {
	uint8_t addr[6];
	int le, rssi;
	char name[64];
};
static struct dev devs[128];
static unsigned ndevs;

static struct dev *dev_for(const uint8_t *a, int le)
{
	for (unsigned i = 0; i < ndevs; i++)
		if (!memcmp(devs[i].addr, a, 6) && devs[i].le == le)
			return &devs[i];
	if (ndevs == sizeof(devs) / sizeof(devs[0]))
		return NULL;
	struct dev *d = &devs[ndevs++];
	memset(d, 0, sizeof(*d));
	memcpy(d->addr, a, 6);
	d->le = le;
	d->rssi = 127;
	printf("  new %s %02x:%02x:%02x:%02x:%02x:%02x\n", le ? "LE     " : "classic", a[5], a[4], a[3], a[2], a[1], a[0]);
	fflush(stdout);
	return d;
}

/* EIR / advertising data: name (0x09 complete, 0x08 short) */
static void ad_name(struct dev *d, const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i + 1 < n && p[i];) {
		unsigned l = p[i], t = p[i + 1];
		if (i + 1 + l > n)
			break;
		if ((t == 0x09 || (t == 0x08 && !d->name[0])) && l > 1) {
			unsigned k = l - 1 < sizeof(d->name) - 1 ? l - 1 : sizeof(d->name) - 1;
			memcpy(d->name, p + i + 2, k);
			d->name[k] = 0;
		}
		i += 1 + l;
	}
}

static void event(const uint8_t *e, unsigned n)
{
	unsigned code = e[0];
	const uint8_t *p = e + 2;
	if (code == 0x2f && n >= 2 + 15) {                      /* Extended Inquiry Result */
		struct dev *d = dev_for(p + 1, 0);
		if (d) {
			d->rssi = (int8_t)p[14];
			ad_name(d, p + 15, n - 2 - 15);
		}
	} else if (code == 0x22 && n >= 2 + 15) {               /* Inquiry Result with RSSI */
		struct dev *d = dev_for(p + 1, 0);
		if (d)
			d->rssi = (int8_t)p[14];
	} else if (code == 0x02 && n >= 2 + 15) {               /* Inquiry Result */
		dev_for(p + 1, 0);
	} else if (code == 0x3e && n >= 3 && p[0] == 0x02) {    /* LE Advertising Report */
		unsigned num = p[1], at = 2;
		for (unsigned r = 0; r < num && at + 9 <= n - 2; r++) {
			const uint8_t *a = p + at + 2;
			unsigned dl = p[at + 8];
			if (at + 9 + dl + 1 > n - 2)
				break;
			struct dev *d = dev_for(a, 1);
			if (d) {
				ad_name(d, p + at + 9, dl);
				d->rssi = (int8_t)p[at + 9 + dl];
			}
			at += 9 + dl + 1;
		}
	} else if (code == 0x01) {
		printf("  inquiry complete (status %u)\n", p[0]);
	} else if (code == 0x0e && n >= 6 && p[3] != 0) {
		printf("  command %02x%02x failed: status %#x\n", p[2], p[1], p[3]);
	}
}

static void scan(unsigned secs)
{
	static const struct { const char *what; uint8_t cmd[16]; unsigned n; } setup_cmds[] = {
		{ "QCA logging off", { 0x01, 0x17, 0xfc, 0x02, 0x14, 0x00 }, 6 },
		{ "event mask", { 0x01, 0x01, 0x0c, 0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f }, 12 },
		{ "LE event mask", { 0x01, 0x01, 0x20, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0 }, 12 },
		{ "inquiry mode: extended", { 0x01, 0x45, 0x0c, 0x01, 0x02 }, 5 },
		{ "LE scan parameters: active, 10 ms / 10 ms", { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 }, 11 },
	};
	for (unsigned i = 0; i < sizeof(setup_cmds) / sizeof(setup_cmds[0]); i++)
		exchange(setup_cmds[i].what, setup_cmds[i].cmd, setup_cmds[i].n, 300);
	static const uint8_t le_on[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x01 }, le_off[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
	uint8_t inquiry[] = { 0x01, 0x01, 0x04, 0x05, 0x33, 0x8b, 0x9e, 0x00, 0x00 };
	unsigned len128 = secs * 100 / 128;                     /* inquiry length in 1.28 s units */
	inquiry[7] = (uint8_t)(len128 < 1 ? 1 : len128 > 0x30 ? 0x30 : len128);
	uart_write(le_on, sizeof(le_on));
	uart_write(inquiry, sizeof(inquiry));
	printf("scanning %u s (classic inquiry + LE active scan)...\n", secs);
	fflush(stdout);

	static uint8_t buf[8192];
	unsigned have = 0, acl = 0;
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < secs * 1000ull) {
		have += drain(buf + have, sizeof(buf) - have);
		unsigned at = 0;
		for (;;) {
			if (at >= have)
				break;
			uint8_t type = buf[at];
			unsigned need;
			if (type == 0x04) {
				if (have - at < 3)
					break;
				need = 3 + buf[at + 2];
			} else if (type == 0x02) {
				if (have - at < 5)
					break;
				need = 5 + (buf[at + 3] | buf[at + 4] << 8);
			} else {
				at++;                                   /* out of step: resync on the next byte */
				continue;
			}
			if (have - at < need)
				break;
			if (type == 0x04)
				event(buf + at + 1, need - 1);
			else
				acl++;
			at += need;
		}
		memmove(buf, buf + at, have - at);
		have -= at;
		if (have == sizeof(buf))
			have = 0;
	}
	uart_write(le_off, sizeof(le_off));
	static const uint8_t inq_cancel[] = { 0x01, 0x02, 0x04, 0x00 };
	uart_write(inq_cancel, sizeof(inq_cancel));
	drain(buf, sizeof(buf));
	printf("%u devices (%u firmware log packets skipped):\n", ndevs, acl);
	for (unsigned i = 0; i < ndevs; i++) {
		struct dev *d = &devs[i];
		printf("  %02x:%02x:%02x:%02x:%02x:%02x  %-7s  RSSI %4d  %s\n", d->addr[5], d->addr[4], d->addr[3],
		    d->addr[2], d->addr[1], d->addr[0], d->le ? "LE" : "classic", d->rssi == 127 ? 0 : d->rssi,
		    d->name[0] ? d->name : "(no name)");
	}
}

static int setup(void)
{
	if (clocks())
		return -1;
	pins();
	uart_init(0);
	set_baud(115200);
	return 0;
}

static void pulse(void)
{
	uint8_t r[256], off = 0xc0, on = 0xfc;
	set_baud(2400);
	uart_write(&off, 1);
	delay_ms(10);
	set_baud(115200);
	uart_write(&on, 1);
	delay_ms(100);
	unsigned got = uart_read(r, sizeof(r), 500);
	if (got)
		show("after the power-on pulse", r, got);
	else
		printf("after the power-on pulse: nothing received (normal: the controller waits for a command)\n");
}

/* Controller and host to 3 Mbaud after the download (Linux hci_qca.c qca_set_speed for WCN399x): our RFR (the chip's
 * CTS) held "not ready" so the chip's answer waits; vendor 0xFC48 [baud code] (enum qca_baudrate: 14 = 3000000);
 * the host's clock to 96 MHz (divider 1 at 32x); RFR back to the hardware; the answer, sent at the new speed, is
 * dropped (Linux QCA_DROP_VENDOR_EVENT); then HCI_Read_BD_ADDR must complete at 3 Mbaud. 0 = running at 3 Mbaud.
 * At 115200 the link moves ~11 KB/s, too little for A2DP (2026-11-09: AirPods connected, bluetoothd's queue
 * overflowed, the H4 stream lost bytes and btbridge stopped). */
__attribute__((unused)) static int uart_fast(void)
{
	static const uint8_t setbaud[] = { 0x01, 0x48, 0xfc, 0x01, 0x0e }, bdaddr[] = { 0x01, 0x09, 0x10, 0x00 };
	uint8_t r[256];
	se_wr(SE_UART_MANUAL_RFR, (1u << 31) | (1u << 1));     /* manual RFR, not ready */
	if (uart_write(setbaud, sizeof(setbaud)))
		return -1;
	delay_ms(10);
	rx_stop();
	if (rcg_set(4, 25, 96000000u))
		return -1;
	set_baud(3000000);
	se_wr(SE_UART_MANUAL_RFR, 0);                          /* RFR by the hardware again */
	unsigned got = uart_read(r, sizeof(r), 300);
	if (got)
		show("  set-baud answer at 3 Mbaud (dropped)", r, got);
	if (uart_write(bdaddr, sizeof(bdaddr)))
		return -1;
	got = uart_read(r, sizeof(r), 300);
	if (got)
		show("  HCI_Read_BD_ADDR at 3 Mbaud", r, got);
	for (unsigned i = 0; i + 7 <= got; i++)
		if (r[i] == 0x04 && r[i + 1] == 0x0e && r[i + 4] == 0x09 && r[i + 5] == 0x10 && r[i + 6] == 0x00) {
			printf("uart: 3 Mbaud link up\n");
			return 0;
		}
	printf("uart: no valid answer at 3 Mbaud\n");
	return -1;
}

static void regs(void)
{
	static const uint32_t o[] = { 0x040, 0x048, 0x04c, 0x068, 0x07c, 0x258, 0x25c, 0x260, 0x264, 0x268, 0x26c, 0x270,
		0x280, 0x284, 0x288, 0x28c, 0x294, 0x2ac, 0x600, 0x610, 0x630, 0x640, 0x800, 0x804, 0x810, 0x814, 0x908 };
	for (unsigned i = 0; i < sizeof(o) / sizeof(o[0]); i++)
		printf("  +%03x %08x\n", o[i], se_rd(o[i]));
}

#ifndef BTUART_NO_MAIN
int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: btuart setup | pulse | version | reset | send HEX... | listen [MS] | hello | regs\n");
		return 2;
	}
	pm_init();
	const char *c = argv[1];
	static const uint8_t version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 }, reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	if (!strcmp(c, "setup"))
		return setup() ? 1 : 0;
	if (!strcmp(c, "pulse")) {
		pulse();
		return 0;
	}
	if (!strcmp(c, "version")) {
		exchange("QCA version request", version, sizeof(version), 500);
		return 0;
	}
	if (!strcmp(c, "reset")) {
		exchange("HCI_Reset", reset, sizeof(reset), 500);
		return 0;
	}
	if (!strcmp(c, "send") && argc > 2) {
		uint8_t b[256];
		unsigned n = 0;
		for (int i = 2; i < argc && n < sizeof(b); i++)
			b[n++] = (uint8_t)strtoul(argv[i], NULL, 16);
		exchange("sent", b, n, 500);
		return 0;
	}
	if (!strcmp(c, "listen")) {
		uint8_t r[4096];
		unsigned ms = argc > 2 ? (unsigned)atoi(argv[2]) : 2000;
		unsigned got = uart_read(r, sizeof(r), ms);
		if (got)
			show("received", r, got);
		else
			printf("nothing in %u ms\n", ms);
		return 0;
	}
	if (!strcmp(c, "hello")) {
		if (setup())
			return 1;
		pulse();
		exchange("QCA version request", version, sizeof(version), 500);
		exchange("HCI_Reset", reset, sizeof(reset), 1000);
		printf("SE registers:\n");
		regs();
		return 0;
	}
	if (!strcmp(c, "board")) {
		static const uint8_t board[] = { 0x01, 0x00, 0xfc, 0x01, 0x23 };
		exchange("QCA board ID request", board, sizeof(board), 500);
		return 0;
	}
	if (!strcmp(c, "load")) {
		char nvm[256];
		snprintf(nvm, sizeof(nvm), "/var/local/fw/bt/%s", argc > 2 ? argv[2] : "crnv21.bin");
		uart_init(1);                                   /* CTS on: the controller paces the download */
		set_baud(115200);
		if (download("/var/local/fw/bt/crbtfw21.tlv", 0))
			printf("rampatch: no answer to the last segment\n");
		if (download(nvm, 1))
			printf("NVM: no answer to the last segment\n");
		delay_ms(100);
		exchange("HCI_Reset", reset, sizeof(reset), 1000);
		static const uint8_t lver[] = { 0x01, 0x01, 0x10, 0x00 }, bdaddr[] = { 0x01, 0x09, 0x10, 0x00 };
		exchange("HCI_Read_Local_Version_Information", lver, sizeof(lver), 500);
		exchange("HCI_Read_BD_ADDR", bdaddr, sizeof(bdaddr), 500);
		return 0;
	}
	if (!strcmp(c, "scan")) {
		scan(argc > 2 ? (unsigned)atoi(argv[2]) : 12);
		return 0;
	}
	if (!strcmp(c, "regs")) {
		regs();
		return 0;
	}
	fprintf(stderr, "btuart: unknown command %s\n", c);
	return 2;
}
#endif /* BTUART_NO_MAIN: btbridge.c includes this file for its UART and controller code */

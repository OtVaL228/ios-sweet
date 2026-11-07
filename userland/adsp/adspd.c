/*
 * adspd: the audio DSP (ADSP, "lpass") of guacamole (SM8150) from userland
 * over /dev/pmem, the way userland/wland brings up the modem: power vote,
 * TrustZone PAS boot of the firmware (modem_b image/adsp.*), SMP2P, then the
 * GLINK edge with two channels: IPCRTR (QRTR: pd-mapper for the ADSP's
 * protection domains) and apr_audio_svc (APR, Linux sound/soc/qcom/qdsp6:
 * q6core, later q6afe/q6asm/q6adm).
 *
 * Board facts (live DT of the stock firmware): PAS id 1, pil_adsp_region
 * 0x8be00000 + 0x1e00000, SMEM host / remote pid 2, SMP2P items 443 in and
 * 429 out with APCS IPC bit 10, GLINK IPC bit 8, vdd_cx >= 385, AOP signal.
 *
 *   adspd boot              power vote + PAS boot + wait for ready
 *   adspd up [SECS]         boot (unless running), GLINK + QRTR + pd-mapper, APR q6core queries, then poll SECS
 *   adspd afe [SECS]        boot (ADSP must be off), then the speakers' AFE port (QUAT MI2S RX) with its clocks for SECS
 *   adspd serve             boot once, then commands through /var/local/adspd.cmd (see serve())
 *   adspd shutdown          graceful stop (force-stop, stop-ack) + PAS shutdown
 *   adspd smp2p             the ADSP's SMP2P state
 * Firmware: /var/local/fw/adsp/adsp.mdt + adsp.bNN (lower case).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "edge.h"
#include "../wland/link.h"
#include "../wland/pmem.h"
#include "../wland/qmi.h"
#include "../wland/smem.h"

#define HOST_ADSP	2
#define PAS_ADSP	1
#define ADSP_PA		0x8be00000ull
#define ADSP_SIZE	0x01e00000ull
#define FW_DIR		"/var/local/fw/adsp"
#define APCS_IPC	0x17c0000cull
#define IPC_SMP2P	10
#define IPC_GLINK	8
#define SMP2P_IN	443
#define SMP2P_OUT	429
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

/* ---- cmd-db + RPMh ARC vote (as wland) ---- */

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

/* the lowest level index whose hlvl is at least min */
static int vote_at_least(const char *res, uint32_t min)
{
	uint64_t aux;
	uint32_t len, idx = 0;
	if (cmddb_aux(res, &aux, &len))
		return -1;
	for (uint32_t i = 0; i < len / 2; i++) {
		idx = i;
		if (aux16(aux, i) >= min)
			break;
	}
	uint32_t off[1] = { 0 }, val[1] = { idx };
	int rc = pm_rpmh(res, 1, off, val);
	printf("vote %s [%u] (%u) -> %d\n", res, idx, aux16(aux, idx), rc);
	return rc;
}

/* ---- AOSS QMP (as wland) ---- */

#define QMP_RAM		0x0c300000ull
#define IPC_AOSS	0
#define QMP_UP		0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_open(void)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1)
		return -1;
	if (rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)
		return 0;
	wr32(QMP_RAM + 0x10, rd32(QMP_RAM + 0x0c));
	wr32(QMP_RAM + 0x24, QMP_UP);
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x28, QMP_UP, 1000))
		return -1;
	wr32(QMP_RAM + 0x2c, QMP_UP);
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x14, QMP_UP, 1000))
		return -1;
	wr32(QMP_RAM + 0x18, QMP_UP);
	kick(IPC_AOSS);
	return wait_eq(QMP_RAM + 0x30, QMP_UP, 1000);
}

static int qmp_send(const char *msg)
{
	if (qmp_open()) {
		printf("qmp: link not up\n");
		return -1;
	}
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	kick(IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	printf("qmp: \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- SMP2P with the ADSP ---- */

static void wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	strncpy(n, name, sizeof(n));
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + i, w);
	}
}

static uint64_t smp2p_out;
static uint32_t adsp_state = 0xffffffff;

static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(HOST_ADSP, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (2u << 16));		/* local pid 0, remote pid 2 */
		wr32(pa + 12, 16 | (1u << 16));		/* 16 entries, 1 valid */
		wr32(pa + 16, 0);
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1, features: SSR ack */
	}
	smp2p_out = pa;
	kick(IPC_SMP2P);
	return 0;
}

/* address of the value of the ADSP's inbound entry NAME (0 = found) */
static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(HOST_ADSP, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

/* a stopped ADSP leaves its last state bits (ready) until it runs again: forget them (as wland for the modem) */
static void smp2p_forget(void)
{
	uint64_t va;
	if (smp2p_in_entry("slave-kernel", &va) == 0)
		wr32(va, 0);
	adsp_state = 0xffffffff;
}

static int smp2p_in_value(const char *name, uint32_t *v)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(HOST_ADSP, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*v = rd32(e + 16);
			return 0;
		}
	}
	return -1;
}

static void smp2p_show(void)
{
	uint64_t pa;
	uint32_t sz;
	for (int k = 0; k < 2; k++) {
		unsigned item = k ? SMP2P_OUT : SMP2P_IN;
		if (smem_get(HOST_ADSP, item, &pa, &sz)) {
			printf("smp2p %s (item %u): absent\n", k ? "out" : "in", item);
			continue;
		}
		uint32_t counts = rd32(pa + 12);
		printf("smp2p %s (item %u): magic %08x pids %x entries %u flags %x\n", k ? "out" : "in", item, rd32(pa),
		       rd32(pa + 8), counts >> 16, rd32(pa + 16));
		for (uint32_t i = 0; i < (counts >> 16) && i < 16; i++) {
			uint64_t e = pa + 20 + 20ull * i;
			char nm[17];
			for (int j = 0; j < 16; j++)
				nm[j] = (char)(rd32(e + (j & ~3)) >> (8 * (j & 3)));
			nm[16] = 0;
			printf("  %-16s %08x\n", nm, rd32(e + 16));
		}
	}
}

static void smp2p_service(void)
{
	uint64_t ip;
	uint32_t sz, v;
	if (!smp2p_out)
		return;
	if (smem_get(HOST_ADSP, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out + 16) & 2) != want) {
			wr32(smp2p_out + 16, (rd32(smp2p_out + 16) & ~2u) | want);
			kick(IPC_SMP2P);
		}
	}
	if (smp2p_in_value("slave-kernel", &v) == 0 && v != adsp_state) {
		printf("[%6llu] smp2p: adsp state %x%s%s%s\n", (unsigned long long)edge_ms(), v, v & 1 ? " ERR-FATAL" : "",
		       v & 2 ? " ready" : "", v & 8 ? " stop-ack" : "");
		adsp_state = v;
	}
}

/* ---- PAS boot (Linux qcom_q6v5_pas + qcom_mdt_loader, as wland's modem) ---- */

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 4);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static int adsp_running(void)
{
	uint32_t v;
	return smp2p_in_value("slave-kernel", &v) == 0 && (v & 2) && !(v & 1);
}

static int boot_adsp(void)
{
	if (vote_at_least("cx.lvl", 385))
		return -1;
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/adsp.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "boot: no %s/adsp.mdt\n", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	if (hash < 0) {
		fprintf(stderr, "boot: no hash segment\n");
		return -1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = malloc(meta_len);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/adsp.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "boot: hash segment %s missing\n", path);
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	uint64_t meta_pa = pm_alloc(meta_len, 0x1000);
	if (!meta_pa)
		return -1;
	pm_wr(meta_pa, meta, meta_len);
	uint64_t r1 = 0;
	int64_t rc = scm(1, 2 | (2u << 6), PAS_ADSP, meta_pa, 0, &r1);
	printf("boot: init image (%zu bytes) -> %lld/%llx\n", meta_len, (long long)rc, (unsigned long long)r1);
	pm_free(meta_pa);
	if (rc || r1)
		return -1;
	rc = scm(2, 3, PAS_ADSP, ADSP_PA, ADSP_SIZE, &r1);
	printf("boot: mem setup %llx+%llx -> %lld/%llx\n", ADSP_PA, ADSP_SIZE, (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	uint64_t total = 0;
	static uint8_t zeros[0x10000];
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = ph[i].paddr;
		if (pa < ADSP_PA || pa + ph[i].memsz > ADSP_PA + ADSP_SIZE) {
			fprintf(stderr, "boot: segment %u at %llx+%x outside the region\n", i, (unsigned long long)pa, ph[i].memsz);
			return -1;
		}
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/adsp.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl < ph[i].filesz) {
				fprintf(stderr, "boot: %s missing or short\n", path);
				return -1;
			}
			pm_wr(pa, seg, (ph[i].filesz + 3) & ~3u);
			free(seg);
			total += ph[i].filesz;
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			pm_wr(pa + o, zeros, (n + 3) & ~3ull);
		}
	}
	printf("boot: %llu bytes of segments loaded\n", (unsigned long long)total);
	qmp_send("{class: image, res: load_state, name: adsp, val: on}");
	smp2p_forget();
	rc = scm(5, 1, PAS_ADSP, 0, 0, &r1);
	printf("boot: auth and reset -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	for (int t = 0; t < 300; t++) {
		smp2p_service();
		if (adsp_running()) {
			printf("boot: adsp ready after %d00 ms\n", t);
			return 0;
		}
		usleep(100000);
	}
	printf("boot: no ready from the adsp in 30 s\n");
	smp2p_show();
	return -1;
}

/*
 * Linux qcom_q6v5_request_stop + qcom_scm_pas_shutdown: raise force-stop (our master-kernel entry, bit 0),
 * wait for stop-ack (the ADSP's slave-kernel, bit 3), then the PAS shutdown. 0153's crash: a plain PAS
 * shutdown of a running ADSP and a new boot right after it hung the phone (no log yet).
 */
static int shutdown_adsp(void)
{
	uint64_t r1 = 0;
	if (smp2p_out && adsp_running()) {
		wr32(smp2p_out + 36, rd32(smp2p_out + 36) | 1);
		kick(IPC_SMP2P);
		uint32_t v = 0;
		int t;
		for (t = 0; t < 50; t++) {
			if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 8))
				break;
			usleep(100000);
		}
		printf("shutdown: stop-ack %s (slave-kernel %x)\n", (v & 8) ? "received" : "NOT received in 5 s", v);
	}
	int64_t rc = scm(6, 1, PAS_ADSP, 0, 0, &r1);
	if (smp2p_out)
		wr32(smp2p_out + 36, rd32(smp2p_out + 36) & ~1u);
	printf("shutdown: -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	smp2p_forget();
	qmp_send("{class: image, res: load_state, name: adsp, val: off}");
	return rc ? -1 : 0;
}

/* ---- QRTR on IPCRTR, pd-mapper for the ADSP's domains ---- */

#define PDMAPPER_SERVICE 0x40
#define PDMAPPER_PORT	0x11
#define GET_DOMAIN_LIST	0x21
#define PFR		0x24

static struct edge_chan chans[] = {
	{ .name = "IPCRTR" },
	{ .name = "apr_audio_svc" },
	{ .name = "apr_apps2" },
};
#define NCHAN	3
#define CH_QRTR (&chans[0])
static struct edge_chan *CH_APR = &chans[1];	/* where APR commands go (aprch) */

static uint32_t adsp_node = 5;	/* learned from HELLO */

static int qrtr_tx(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	uint8_t b[4096 + 64];
	struct qrtr_hdr hd = {
		.version = 1, .type = type, .src_node = QRTR_NODE_APPS, .src_port = src_port,
		.size = len, .dst_node = dst_node, .dst_port = dst_port,
	};
	uint32_t padded = (len + 3) & ~3u;
	if (sizeof(hd) + padded > sizeof(b))
		return -1;
	memset(b, 0, sizeof(hd) + padded);
	memcpy(b, &hd, sizeof(hd));
	memcpy(b + sizeof(hd), p, len);
	return edge_send(CH_QRTR, b, sizeof(hd) + padded);
}

/* the firmware's adspr.jsn and adspua.jsn */
static const struct { const char *service, *domain; uint32_t instance; } pd_db[] = {
	{ "tms/servreg", "msm/adsp/root_pd", 74 },
	{ "tms/servreg", "msm/adsp/audio_pd", 74 },
	{ "avs/audio", "msm/adsp/audio_pd", 74 },
};

static void pdmapper_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	if (m.id == PFR) {
		uint16_t l1 = 0, l2 = 0;
		const uint8_t *svc = qmi_get(&m, 1, &l1), *why = qmi_get(&m, 2, &l2);
		printf("pd-mapper: PD FAILURE \"%.*s\": %.*s\n", svc ? l1 : 0, svc ? (const char *)svc : "",
		       why ? l2 : 0, why ? (const char *)why : "");
		qmi_tlv_result(&q, 0, 0);
	} else if (m.id != GET_DOMAIN_LIST) {
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);
	} else {
		uint16_t l = 0;
		const uint8_t *p = qmi_get(&m, 1, &l);
		char name[80] = { 0 };
		uint32_t offset = 0;
		if (p)
			memcpy(name, p, l < 79 ? l : 79);
		qmi_get_u32(&m, 0x10, &offset);
		uint8_t list[512];
		size_t o = 1;
		uint16_t total = 0;
		list[0] = 0;
		for (unsigned i = 0; i < sizeof(pd_db) / sizeof(pd_db[0]); i++) {
			if (strcmp(pd_db[i].service, name))
				continue;
			if (total++ < offset)
				continue;
			uint8_t nl = (uint8_t)strlen(pd_db[i].domain);
			list[o++] = nl;
			memcpy(list + o, pd_db[i].domain, nl);
			o += nl;
			memcpy(list + o, &pd_db[i].instance, 4);
			o += 4;
			list[o++] = 0;
			memset(list + o, 0, 4);
			o += 4;
			list[0]++;
		}
		printf("pd-mapper: \"%s\" -> %u domain(s)\n", name, total);
		qmi_tlv_result(&q, 0, 0);
		qmi_tlv(&q, 0x10, &total, 2);
		uint16_t rev = 1;
		qmi_tlv(&q, 0x11, &rev, 2);
		qmi_tlv(&q, 0x12, list, (uint16_t)o);
	}
	qrtr_tx(QRTR_TYPE_DATA, PDMAPPER_PORT, h->src_node, h->src_port, q.b, (uint32_t)qmi_finish(&q));
}

/*
 * PDR (Linux drivers/soc/qcom/pdr_interface.c): the ADSP's service registry notifier (QMI service 0x42) tells
 * the state of a protection domain: REGISTER_LISTENER for a service path answers with the current state.
 */
#define SERVREG_NOTIFIER_SERVICE	0x42
#define SERVREG_REGISTER_LISTENER	0x20
#define SERVREG_STATE_UPDATED		0x22
#define SERVREG_CLIENT_PORT		0x20
static uint32_t servreg_node, servreg_port;
static uint16_t servreg_txn = 1;

static void servreg_handle(const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m))
		return;
	uint32_t state = 0;
	uint16_t l = 0;
	const uint8_t *r = qmi_get(&m, 2, &l);
	if (m.id == SERVREG_REGISTER_LISTENER && m.type == QMI_RESPONSE) {
		qmi_get_u32(&m, 0x10, &state);
		printf("pdr: register listener -> result %u error %u, state %#x (%s)\n", r && l >= 4 ? r[0] | r[1] << 8 : 0xffff,
		       r && l >= 4 ? r[2] | r[3] << 8 : 0xffff, state,
		       state == 0x1fffffff ? "UP" : state == 0x0fffffff ? "DOWN" : state == 0x7fffffff ? "UNINIT" : "?");
	} else if (m.id == SERVREG_STATE_UPDATED) {
		qmi_get_u32(&m, 1, &state);
		printf("pdr: state update %#x\n", state);
		qmi_dump(&m);
	} else {
		qmi_dump(&m);
	}
}

static void servreg_query(const char *path)
{
	if (!servreg_port) {
		printf("pdr: no servreg notifier announced\n");
		return;
	}
	struct qmi_buf q;
	qmi_start(&q, QMI_REQUEST, servreg_txn++, SERVREG_REGISTER_LISTENER);
	qmi_tlv_u8(&q, 1, 1);
	qmi_tlv(&q, 2, path, (uint16_t)strlen(path));
	printf("pdr: register listener for %s at %u:%x\n", path, servreg_node, servreg_port);
	qrtr_tx(QRTR_TYPE_DATA, SERVREG_CLIENT_PORT, servreg_node, servreg_port, q.b, (uint32_t)qmi_finish(&q));
}

static void qrtr_rx(const uint8_t *d, uint32_t n)
{
	struct qrtr_hdr h;
	if (n < sizeof(h))
		return;
	memcpy(&h, d, sizeof(h));
	const uint8_t *p = d + sizeof(h);
	uint32_t len = n - sizeof(h) < h.size ? n - sizeof(h) : h.size;
	uint32_t w[5] = { 0 };
	memcpy(w, p, len < sizeof(w) ? len : sizeof(w));
	if (h.type == QRTR_TYPE_DATA) {
		if (h.confirm_rx) {
			uint32_t pkt[5] = { QRTR_TYPE_RESUME_TX, h.dst_node, h.dst_port, 0, 0 };
			qrtr_tx(QRTR_TYPE_RESUME_TX, h.dst_port, h.src_node, h.src_port, pkt, sizeof(pkt));
		}
		if (h.dst_port == PDMAPPER_PORT)
			pdmapper_handle(&h, p, len);
		else if (h.dst_port == SERVREG_CLIENT_PORT)
			servreg_handle(p, len);
		else
			printf("qrtr: < DATA %u:%x -> %u:%x, %u bytes\n", h.src_node, h.src_port, h.dst_node, h.dst_port, len);
		return;
	}
	printf("[%6llu] qrtr: < ctrl %u from %u:%x: %x %x %x %x\n", (unsigned long long)edge_ms(), w[0], h.src_node,
	       h.src_port, w[1], w[2], w[3], w[4]);
	if (w[0] == QRTR_TYPE_HELLO) {
		adsp_node = h.src_node;
		uint32_t hello[5] = { QRTR_TYPE_HELLO };
		qrtr_tx(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h.src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
		uint32_t srv[5] = { QRTR_TYPE_NEW_SERVER, PDMAPPER_SERVICE, 1 | (1u << 8), QRTR_NODE_APPS, PDMAPPER_PORT };
		qrtr_tx(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, h.src_node, QRTR_PORT_CTRL, srv, sizeof(srv));
		printf("qrtr: adsp is node %u; pd-mapper at %u:%x\n", adsp_node, QRTR_NODE_APPS, PDMAPPER_PORT);
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		printf("qrtr:   server service %#x version %u instance %#x at %u:%x\n", w[1], w[2] & 0xff, w[2] >> 8, w[3], w[4]);
		if (w[1] == SERVREG_NOTIFIER_SERVICE) {
			servreg_node = w[3];
			servreg_port = w[4];
		}
	}
}

/* ---- APR (Linux include/soc/qcom/apr.h, sound/soc/qcom/qdsp6/q6core.c) ---- */

#define APR_DOMAIN_ADSP		4
#define APR_DOMAIN_APPS		5
#define APR_SVC_ADSP_CORE	3
#define APR_HDR_FIELD_CMD	((1u << 8) | (5u << 4))	/* SEQ_CMD, 20-byte header, version 0 */
#define APR_BASIC_RSP_RESULT	0x000110e8
#define AVCS_GET_VERSIONS	0x00012905
#define AVCS_GET_VERSIONS_RSP	0x00012906
#define AVCS_EVENT_GET_STATE	0x0001290c
#define AVCS_EVENT_GET_STATE_RSP 0x0001290d
#define AVCS_GET_FWK_VERSION	0x0001292c
#define AVCS_GET_FWK_VERSION_RSP 0x0001292d

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

static uint32_t apr_token = 1;

static int apr_send(uint8_t svc, uint32_t opcode, const void *payload, uint16_t len)
{
	uint8_t b[512];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD_CMD, .pkt_size = (uint16_t)(sizeof(h) + len),
		.src_svc = svc, .src_domain = APR_DOMAIN_APPS, .src_port = 0,
		.dest_svc = svc, .dest_domain = APR_DOMAIN_ADSP, .dest_port = 0,
		.token = apr_token++, .opcode = opcode,
	};
	memcpy(b, &h, sizeof(h));
	if (len)
		memcpy(b + sizeof(h), payload, len);
	printf("[%6llu] apr: > svc %u opcode %#x token %u (%u bytes)\n", (unsigned long long)edge_ms(), svc, opcode,
	       h.token, len);
	return edge_send(CH_APR, b, sizeof(h) + len);
}

static int adsp_ready_state = -1;
static uint32_t rsp_opcode, rsp_status;

/* send and wait for APR_BASIC_RSP_RESULT for that opcode: its status, or -1 on timeout */
static int apr_cmd(uint8_t svc, uint32_t opcode, const void *payload, uint16_t len)
{
	rsp_opcode = 0;
	if (apr_send(svc, opcode, payload, len))
		return -1;
	for (int t = 0; t < 200; t++) {
		edge_poll(5);
		if (rsp_opcode == opcode)
			return (int)rsp_status;
	}
	printf("apr: no answer to %#x\n", opcode);
	return -1;
}

static void apr_rx(const uint8_t *d, uint32_t n)
{
	struct apr_hdr h;
	if (n < sizeof(h)) {
		printf("apr: short packet %u\n", n);
		return;
	}
	memcpy(&h, d, sizeof(h));
	const uint8_t *p = d + ((h.hdr_field >> 4) & 0xf) * 4;
	uint32_t len = n - (uint32_t)(p - d);
	uint32_t w[8] = { 0 };
	memcpy(w, p, len < sizeof(w) ? len : sizeof(w));
	printf("[%6llu] apr: < svc %u from %u:%u opcode %#x token %u, %u bytes:", (unsigned long long)edge_ms(), h.dest_svc,
	       h.src_domain, h.src_svc, h.opcode, h.token, len);
	for (uint32_t i = 0; i < len / 4 && i < 8; i++)
		printf(" %08x", w[i]);
	printf("\n");
	switch (h.opcode) {
	case APR_BASIC_RSP_RESULT:
		printf("apr:   result for opcode %#x: status %#x\n", w[0], w[1]);
		rsp_opcode = w[0];
		rsp_status = w[1];
		break;
	case AVCS_EVENT_GET_STATE_RSP:
		adsp_ready_state = (int)w[0];
		printf("apr:   ADSP state %u (%s)\n", w[0], w[0] == 1 ? "ready" : "not ready");
		break;
	case AVCS_GET_FWK_VERSION_RSP:
		printf("apr:   framework build %u.%u.%u.%u, %u services\n", w[0], w[1], w[2], w[3], w[4]);
		break;
	case AVCS_GET_VERSIONS_RSP: {
		printf("apr:   build id %#x, %u services\n", w[0], w[1]);
		for (uint32_t i = 0; i < w[1] && 8 + 8 * i + 8 <= len; i++) {
			uint32_t sv[2];
			memcpy(sv, p + 8 + 8 * i, 8);
			printf("apr:     service %#x version %#x\n", sv[0], sv[1]);
		}
		break;
	}
	}
}

/* ---- AFE (Linux sound/soc/qcom/qdsp6/q6afe.c) ---- */

#define APR_SVC_AFE			4
#define AFE_PORT_CMD_DEVICE_START	0x000100e5
#define AFE_PORT_CMD_DEVICE_STOP	0x000100e6
#define AFE_PORT_CMD_SET_PARAM_V2	0x000100ef
#define AFE_SVC_CMD_SET_PARAM		0x000100f3
#define AFE_MODULE_AUDIO_DEV_INTERFACE	0x0001020c
#define AFE_PARAM_ID_I2S_CONFIG		0x0001020d
#define AFE_MODULE_CLOCK_SET		0x0001028f
#define AFE_PARAM_ID_CLOCK_SET		0x00010290
#define AFE_PORT_QUAT_MI2S_RX		0x1006
#define CLK_ID_QUAD_MI2S_IBIT		0x106
#define TLMM_SOUTH			0x03d00000ull

/*
 * The speakers' bus (live DT): QUAT MI2S, SCK GPIO137, WS GPIO138, SD0 GPIO139 (feedback in), SD1 GPIO140
 * (to the amps: qcom,msm-mi2s-rx-lines = <2>), function qua_mi2s (mux 1). 48 kHz stereo in 32-bit slots
 * (the amps' container: 32-bit TDM slots), so the bit clock is 48000 * 2 * 32 = 3.072 MHz.
 */
#define MI2S_RATE	48000
#define MI2S_BITS	32

static void mi2s_pins(void)
{
	for (unsigned pin = 137; pin <= 140; pin++) {
		uint64_t cfg = TLMM_SOUTH + 0x1000ull * pin;
		wr32(cfg, (rd32(cfg) & ~0x3ffu) | (1u << 2) | (3u << 6));	/* mux 1, 8 mA, no pull */
	}
	printf("mi2s pins: %#x %#x %#x %#x\n", rd32(TLMM_SOUTH + 137 * 0x1000), rd32(TLMM_SOUTH + 138 * 0x1000),
	       rd32(TLMM_SOUTH + 139 * 0x1000), rd32(TLMM_SOUTH + 140 * 0x1000));
}

static int afe_clock(uint32_t id, uint32_t hz, int enable)
{
	struct __attribute__((packed)) {
		uint32_t payload_size, addr_lsw, addr_msw, mem_map_handle;
		uint32_t module_id, param_id;
		uint16_t param_size, reserved;
		uint32_t minor, clk_id, freq;
		uint16_t attri, root;
		uint32_t enable;
	} p = {
		.payload_size = 12 + 24, .module_id = AFE_MODULE_CLOCK_SET, .param_id = AFE_PARAM_ID_CLOCK_SET,
		.param_size = 24, .minor = 1, .clk_id = id, .freq = hz, .attri = 1 /* couple no */, .enable = enable ? 1 : 0,
	};
	int st = apr_cmd(APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, &p, sizeof(p));
	printf("afe: clock %#x %u Hz %s -> %d\n", id, hz, enable ? "on" : "off", st);
	return st;
}

static int afe_i2s_config(uint16_t port)
{
	struct __attribute__((packed)) {
		uint16_t port_id, payload_size;
		uint32_t addr_lsw, addr_msw, mem_map_handle;
		uint32_t module_id, param_id;
		uint16_t param_size, reserved;
		uint32_t minor;
		uint16_t bit_width, channel_mode, mono_stereo, ws_src;
		uint32_t sample_rate;
		uint16_t data_format, reserved2;
	} p = {
		.port_id = port, .payload_size = 12 + 20, .module_id = AFE_MODULE_AUDIO_DEV_INTERFACE,
		.param_id = AFE_PARAM_ID_I2S_CONFIG, .param_size = 20, .minor = 1, .bit_width = MI2S_BITS,
		.channel_mode = 2 /* SD1 */, .mono_stereo = 1 /* stereo */, .ws_src = 1 /* internal: we are master */,
		.sample_rate = MI2S_RATE,
	};
	int st = apr_cmd(APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, &p, sizeof(p));
	printf("afe: port %#x I2S config SD1 stereo %u-bit %u Hz -> %d\n", port, MI2S_BITS, MI2S_RATE, st);
	return st;
}

static int afe_port(uint16_t port, int start)
{
	uint16_t p[2] = { port, 0 };
	int st = apr_cmd(APR_SVC_AFE, start ? AFE_PORT_CMD_DEVICE_START : AFE_PORT_CMD_DEVICE_STOP, p, sizeof(p));
	printf("afe: port %#x %s -> %d\n", port, start ? "start" : "stop", st);
	return st;
}

static void apr_rx_apps2(const uint8_t *d, uint32_t n)
{
	printf("[%6llu] apr_apps2: %u bytes\n", (unsigned long long)edge_ms(), n);
	apr_rx(d, n);
}

/* ---- main ---- */

static void idle(unsigned ms)
{
	uint64_t end = edge_ms() + ms;
	while (edge_ms() < end) {
		edge_poll(50);
		smp2p_service();
	}
}

/*
 * The GLINK and QRTR state of a running ADSP lives in the process that booted it; `quit` saves it here so a
 * new adspd (an update) adopts the edge instead of restarting the ADSP (a silent ADSP restart crashed the
 * modem: it is not told through glink_ssr, and Wi-Fi died with it).
 */
#define STATE_FILE "/var/local/adspd.state"
#define QRTR_STATE_FILE "/var/local/adspd.qrtr"
static int adopted;

static int save_qrtr_state(void)
{
	FILE *f = fopen(QRTR_STATE_FILE, "w");
	if (!f)
		return -1;
	fprintf(f, "%u %u %u\n", adsp_node, servreg_node, servreg_port);
	fclose(f);
	return 0;
}

static int load_qrtr_state(void)
{
	FILE *f = fopen(QRTR_STATE_FILE, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%u %u %u", &adsp_node, &servreg_node, &servreg_port) == 3;
	fclose(f);
	return ok ? 0 : -1;
}

/* ADSP up (booted once per phone boot), APR q6core ready */
static int bring_up(int fresh)
{
	smem_init();
	if (smp2p_create())
		return 1;
	if (fresh && adsp_running()) {
		shutdown_adsp();
		usleep(200000);
	}
	if (!adsp_running()) {
		unlink(STATE_FILE);
		if (boot_adsp())
			return 1;
	} else if (edge_adopt(STATE_FILE, HOST_ADSP, IPC_GLINK, chans, NCHAN) == 0 && load_qrtr_state() == 0) {
		adopted = 1;
	} else {
		printf("adsp already running, and no saved edge state: GLINK cannot be resumed (reboot the phone)\n");
		return 1;
	}
	chans[0].rx = qrtr_rx;
	chans[1].rx = apr_rx;
	chans[2].rx = apr_rx_apps2;
	if (adopted) {
		adsp_ready_state = 1;
		return 0;
	}
	if (edge_init(HOST_ADSP, IPC_GLINK, chans, NCHAN))
		return 1;
	uint64_t end = edge_ms() + 15000;
	while (edge_ms() < end && !edge_ready(CH_APR)) {
		edge_poll(10);
		smp2p_service();
	}
	printf("glink: IPCRTR %s, apr_audio_svc %s\n", edge_ready(CH_QRTR) ? "open" : "NOT open",
	       edge_ready(CH_APR) ? "open" : "NOT open");
	if (!edge_ready(CH_APR))
		return 1;
	for (int t = 0; t < 50 && adsp_ready_state != 1; t++) {
		if (t % 10 == 0)
			apr_send(APR_SVC_ADSP_CORE, AVCS_EVENT_GET_STATE, NULL, 0);
		edge_poll(100);
		smp2p_service();
	}
	return adsp_ready_state == 1 ? 0 : 1;
}

/*
 * serve: boot the ADSP once and keep GLINK/APR in this process (each new process would have to restart the
 * ADSP, see shutdown_adsp); commands, one per line, through the FIFO /var/local/adspd.cmd:
 *   afe on | afe off | status | pd PATH (PDR state, e.g. msm/adsp/audio_pd) | open NAME RCID (our end of a
 *   channel the ADSP opened) | aprch audio|apps2 (the channel for APR commands) | apr SVC OPCODE [WORDS] (raw
 *   APR command) | stop (graceful ADSP shutdown,
 *   then exit) | quit (exit, ADSP keeps running)
 */
#define CMD_FIFO "/var/local/adspd.cmd"
static int afe_running;

static void command(char *line)
{
	char *nl = strchr(line, '\n');
	if (nl)
		*nl = 0;
	printf("[%6llu] cmd: \"%s\"\n", (unsigned long long)edge_ms(), line);
	if (!strcmp(line, "afe on") && !afe_running) {
		mi2s_pins();
		int st = afe_clock(CLK_ID_QUAD_MI2S_IBIT, MI2S_RATE * 2 * MI2S_BITS, 1);
		if (!st)
			st = afe_i2s_config(AFE_PORT_QUAT_MI2S_RX);
		if (!st)
			st = afe_port(AFE_PORT_QUAT_MI2S_RX, 1);
		afe_running = !st;
		if (st)
			afe_clock(CLK_ID_QUAD_MI2S_IBIT, 0, 0);
	} else if (!strcmp(line, "afe off") && afe_running) {
		afe_port(AFE_PORT_QUAT_MI2S_RX, 0);
		afe_clock(CLK_ID_QUAD_MI2S_IBIT, 0, 0);
		afe_running = 0;
	} else if (!strncmp(line, "open ", 5)) {
		char name[64];
		unsigned rcid;
		if (sscanf(line + 5, "%63s %u", name, &rcid) == 2) {
			for (unsigned i = 0; i < NCHAN; i++)
				if (!strcmp(chans[i].name, name)) {
					edge_open_late(&chans[i], rcid);
					printf("open: %s (rcid %u, lcid %u) opened on our side\n", name, rcid, chans[i].lcid);
				}
		}
	} else if (!strncmp(line, "aprch ", 6)) {
		CH_APR = !strcmp(line + 6, "apps2") ? &chans[2] : &chans[1];
		printf("aprch: APR commands go to %s (%s)\n", CH_APR->name, edge_ready(CH_APR) ? "open" : "NOT open");
	} else if (!strncmp(line, "apr ", 4)) {
		/* apr SVC OPCODE [32-bit payload words...]: a raw APR command, for experiments */
		uint32_t w[32];
		unsigned n = 0, svc;
		char *t = strtok(line + 4, " ");
		svc = t ? (unsigned)strtoul(t, NULL, 0) : 0;
		uint32_t opcode = (t = strtok(NULL, " ")) ? (uint32_t)strtoul(t, NULL, 0) : 0;
		while ((t = strtok(NULL, " ")) && n < 32)
			w[n++] = (uint32_t)strtoul(t, NULL, 0);
		int st = apr_cmd((uint8_t)svc, opcode, w, (uint16_t)(4 * n));
		printf("apr: svc %u opcode %#x -> %d\n", svc, opcode, st);
	} else if (!strncmp(line, "pd ", 3)) {
		servreg_query(line + 3);
	} else if (!strcmp(line, "status")) {
		apr_send(APR_SVC_ADSP_CORE, AVCS_EVENT_GET_STATE, NULL, 0);
	}
}

static int serve(void)
{
	signal(SIGPIPE, SIG_IGN);
	if (bring_up(0))
		return 1;
	unlink(CMD_FIFO);
	if (mkfifo(CMD_FIFO, 0600)) {
		perror(CMD_FIFO);
		return 1;
	}
	int fd = open(CMD_FIFO, O_RDWR | O_NONBLOCK);	/* RDWR: never EOF when writers come and go */
	printf("serve: commands through %s\n", CMD_FIFO);
	char buf[256];
	size_t have = 0;
	for (;;) {
		edge_poll(20);
		smp2p_service();
		ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
		if (n > 0)
			have += (size_t)n;
		char *nl;
		while ((nl = memchr(buf, '\n', have))) {
			*nl = 0;
			if (!strcmp(buf, "quit")) {
				int e = edge_save(STATE_FILE) | save_qrtr_state();
				printf("quit: edge state %s\n", e ? "NOT saved" : "saved for the next adspd");
				return e ? 1 : 0;
			}
			if (!strcmp(buf, "stop")) {
				if (afe_running)
					command((char[]){ "afe off" });
				return shutdown_adsp() ? 1 : 0;
			}
			command(buf);
			size_t used = (size_t)(nl - buf) + 1;
			memmove(buf, nl + 1, have - used);
			have -= used;
		}
		if (have == sizeof(buf) - 1)
			have = 0;
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: adspd boot | up [SECS] | afe [SECS] | serve | shutdown | smp2p\n");
		return 2;
	}
	pm_init();
	if (!strcmp(argv[1], "smp2p")) {
		smem_init();
		smp2p_show();
		return 0;
	}
	if (!strcmp(argv[1], "shutdown")) {
		smem_init();
		return shutdown_adsp() ? 1 : 0;
	}
	if (!strcmp(argv[1], "boot")) {
		smem_init();
		if (smp2p_create())
			return 1;
		return boot_adsp() ? 1 : 0;
	}
	unsigned secs = argc > 2 ? (unsigned)atoi(argv[2]) : 5;
	if (!strcmp(argv[1], "up")) {
		if (bring_up(0))
			return 1;
		apr_send(APR_SVC_ADSP_CORE, AVCS_GET_FWK_VERSION, NULL, 0);
		edge_poll(500);
		apr_send(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0);
		idle(500 + 1000 * secs);
		return 0;
	}
	if (!strcmp(argv[1], "afe")) {
		/* the QUAT MI2S RX port with its clocks for SECS seconds (the amps should see a stable clock) */
		smem_init();
		if (adsp_running()) {
			printf("afe: the ADSP is running (GLINK state lives in its process): use serve, or shutdown first\n");
			return 1;
		}
		if (bring_up(0))
			return 1;
		mi2s_pins();
		int st = afe_clock(CLK_ID_QUAD_MI2S_IBIT, MI2S_RATE * 2 * MI2S_BITS, 1);
		if (!st)
			st = afe_i2s_config(AFE_PORT_QUAT_MI2S_RX);
		if (!st)
			st = afe_port(AFE_PORT_QUAT_MI2S_RX, 1);
		if (!st) {
			printf("afe: QUAT MI2S RX running for %u s\n", secs);
			idle(1000 * secs);
			afe_port(AFE_PORT_QUAT_MI2S_RX, 0);
		}
		afe_clock(CLK_ID_QUAD_MI2S_IBIT, 0, 0);
		return st ? 1 : 0;
	}
	if (!strcmp(argv[1], "serve"))
		return serve();
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}

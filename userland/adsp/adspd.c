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
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "edge.h"
#include "../audio/sweetpcm.h"
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
	{ .name = "fastrpcglink-apps-dsp" },
};
#define NCHAN	4
#define CH_RPC	(&chans[3])
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
/*
 * SEQ_CMD is 2 (Linux soc/qcom/apr.h: EVENT 0, CMD_RSP 1, SEQ_CMD 2, NSEQ_CMD 3). With 1 every command went out
 * as a response: handlers that send their own RSP (GET_STATE, GET_VERSIONS, AFE GET_PARAM) answered, but no
 * APR_BASIC_RSP_RESULT ever came, so AFE's clock set / port commands looked dead.
 */
#define APR_HDR_FIELD_CMD	((2u << 8) | (5u << 4))	/* SEQ_CMD, 20-byte header, version 0 */
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

/* ASM / ADM answers with their own opcode (shared memory handle, COPP id): the last one, see apr_cmd_ex */
static uint32_t rsp2_opcode, rsp2_w[4];
static void asm_write_done(uint32_t token, const uint32_t *w);
static int smmu_adsp_identity(void);

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
	if (h.opcode == 0x00010d99) {	/* ASM_DATA_EVENT_WRITE_DONE_V2, one per period: not logged */
		asm_write_done(h.token, w);
		return;
	}
	if (h.opcode != APR_BASIC_RSP_RESULT) {
		rsp2_opcode = h.opcode;
		memcpy(rsp2_w, w, sizeof(rsp2_w));
	}
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

/*
 * Does AFE answer at all, before anything touches the hardware? The clock set was always the first AFE command
 * of a boot and nothing ever answered after it (DEVICE_STOP neither): it may hang the AFE thread. This sends
 * a read of the port's I2S config (AFE_PORT_CMD_GET_PARAM_V2, in-band) and a DEVICE_STOP of the idle port,
 * and logs whatever comes back (apr_rx prints every packet).
 */
#define AFE_PORT_CMD_GET_PARAM_V2	0x000100f0
#define AFE_PORT_CMDRSP_GET_PARAM_V2	0x00010106

static void afe_probe(void)
{
	struct __attribute__((packed)) {
		uint16_t port_id, payload_size;
		uint32_t addr_lsw, addr_msw, mem_map_handle;
		uint32_t module_id, param_id;
	} g = {
		.port_id = AFE_PORT_QUAT_MI2S_RX, .payload_size = 12 + 20, .module_id = AFE_MODULE_AUDIO_DEV_INTERFACE,
		.param_id = AFE_PARAM_ID_I2S_CONFIG,
	};
	apr_send(APR_SVC_AFE, AFE_PORT_CMD_GET_PARAM_V2, &g, sizeof(g));
	for (int t = 0; t < 200; t++)
		edge_poll(5);
	afe_port(AFE_PORT_QUAT_MI2S_RX, 0);
}

/*
 * ---- playback: ASM stream -> ADM COPP -> AFE port (Linux sound/soc/qcom/qdsp6/q6asm.c, q6adm.c) ----
 * Session 1, stream 1, 16-bit stereo PCM at 48 kHz from a ring of periods in memory below 4 GB that the
 * ADSP reads through stream 0x1b21 (q6asmdai's, identity in smmu_adsp_identity). ADM opens a COPP on the
 * speaker port with the null topology and the matrix routes the session to it. Each period goes out as
 * ASM_DATA_CMD_WRITE_V2 and is refilled on its WRITE_DONE. `tone` plays a quiet 440 Hz sine through it.
 */
#define APR_SVC_ASM				7
#define APR_SVC_ADM				8
#define ASM_CMD_SHARED_MEM_MAP_REGIONS		0x00010d92
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00010d93
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS	0x00010d94
#define PCM_HANDLE_FILE	"/var/local/adspd.pcm"	/* the ring's ASM memory handle, for the next adspd */
#define ASM_STREAM_CMD_OPEN_WRITE_V3		0x00010db3
#define ASM_STREAM_CMD_CLOSE			0x00010bcd
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2	0x00010d98
#define ASM_SESSION_CMD_RUN_V2			0x00010daa
#define ASM_DATA_CMD_WRITE_V2			0x00010dab
#define ASM_NULL_POPP_TOPOLOGY			0x00010c68
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2	0x00010da5
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL		3
#define ADM_CMD_DEVICE_OPEN_V5			0x00010326
#define ADM_CMDRSP_DEVICE_OPEN_V5		0x00010329
#define ADM_CMD_DEVICE_CLOSE_V5			0x00010327
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5		0x00010325
#define ADM_PATH_PLAYBACK			1
#define NULL_COPP_TOPOLOGY			0x00010312

#define PCM_PA		0xfe800000ull	/* after the FastRPC buffer; nothing below 4 GB is XNU's */
/*
 * The audio buffers' stream in bits 32+ of every address the ADSP is given, as msm_audio_ion.c does
 * (smmu_sid_bits = (iommus SID 0x1b21 & qcom,smmu-sid-mask 0xf) << 32 = 1 << 32). Without it the first
 * shared memory map sent the SoC to crashdump (stream 0x1b20), as FastRPC did without its context bank.
 */
#define PCM_SID_MSW	1u
#define PCM_PERIOD	4096u		/* bytes: 1024 frames of 16-bit stereo, 21.3 ms */
#define PCM_PERIODS	16u
#define PCM_INFLIGHT	4u		/* periods queued in the ADSP: 85 ms */
#define PCM_SESSION	1
#define PCM_PORT	((PCM_SESSION << 8) | 1)	/* session << 8 | stream */

static uint32_t pcm_mem_handle;
static int pcm_open, pcm_copp = -1;
static unsigned pcm_inflight, pcm_written, pcm_done;
static uint64_t pcm_until;	/* edge_ms() when the tone ends */
static double pcm_phase;
static struct sweetpcm *pcm_ring;	/* SweetCodec's ring (`pcm on`), else the tone */
static unsigned pcm_underruns, pcm_skips;

/* the ring file SweetCodec maps: made here, mode 0666 (mediaserverd runs as mobile) */
static int pcm_ring_open(void)
{
	if (pcm_ring)
		return 0;
	int fd = open(SWEETPCM_PATH, O_RDWR | O_CREAT, 0666);
	if (fd < 0 || ftruncate(fd, SWEETPCM_SIZE) || fchmod(fd, 0666)) {
		perror(SWEETPCM_PATH);
		if (fd >= 0)
			close(fd);
		return -1;
	}
	void *m = mmap(NULL, SWEETPCM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		perror("mmap " SWEETPCM_PATH);
		return -1;
	}
	pcm_ring = m;
	if (pcm_ring->magic != SWEETPCM_MAGIC || pcm_ring->frames != SWEETPCM_FRAMES) {
		memset(pcm_ring, 0, sizeof(*pcm_ring) - sizeof(pcm_ring->data));
		pcm_ring->rate = SWEETPCM_RATE;
		pcm_ring->frames = SWEETPCM_FRAMES;
		pcm_ring->channels = 2;
		pcm_ring->magic = SWEETPCM_MAGIC;
	}
	pcm_ring->rpos = pcm_ring->wpos;
	printf("pcm: ring %s, %u frames, wpos %llu\n", SWEETPCM_PATH, SWEETPCM_FRAMES,
	       (unsigned long long)pcm_ring->wpos);
	return 0;
}

/* one period from the ring: whole periods only (silence while short), latency kept near two periods */
static void pcm_fill_ring(int16_t *s)
{
	const uint32_t n = PCM_PERIOD / 4;
	uint64_t w = pcm_ring->wpos, r = pcm_ring->rpos;
	if (w - r > 4 * n || w < r) {
		r = w - 2 * n;
		pcm_skips++;
	}
	if (w - r < n) {
		memset(s, 0, PCM_PERIOD);
		pcm_underruns++;
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t k = (uint32_t)((r + i) % SWEETPCM_FRAMES);
		s[2 * i] = pcm_ring->data[2 * k];
		s[2 * i + 1] = pcm_ring->data[2 * k + 1];
	}
	pcm_ring->rpos = r + n;
}

/* send with explicit ports and token; wait for the basic result of opcode or for rsp (0: none) */
static int apr_cmd_ports(uint8_t svc, uint32_t opcode, uint16_t sport, uint16_t dport, uint32_t token,
			 const void *payload, uint16_t len, uint32_t rsp)
{
	uint8_t b[512];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD_CMD, .pkt_size = (uint16_t)(sizeof(h) + len),
		.src_svc = svc, .src_domain = APR_DOMAIN_APPS, .src_port = sport,
		.dest_svc = svc, .dest_domain = APR_DOMAIN_ADSP, .dest_port = dport,
		.token = token, .opcode = opcode,
	};
	memcpy(b, &h, sizeof(h));
	memcpy(b + sizeof(h), payload, len);
	rsp_opcode = 0;
	rsp2_opcode = 0;
	if (edge_send(CH_APR, b, sizeof(h) + len))
		return -1;
	for (int t = 0; t < 200; t++) {
		edge_poll(5);
		if (rsp_opcode == opcode)
			return (int)rsp_status;
		if (rsp && rsp2_opcode == rsp)
			return 0;
	}
	printf("apr: no answer to %#x\n", opcode);
	return -1;
}

static int apr_cmd_ex(uint8_t svc, uint32_t opcode, uint16_t port, uint32_t token, const void *payload, uint16_t len,
		      uint32_t rsp)
{
	return apr_cmd_ports(svc, opcode, port, port, token, payload, len, rsp);
}

/* one period of the tone: amplitude 1000 of 32767 (about -30 dBFS), both channels */
static void pcm_fill(unsigned idx)
{
	int16_t s[PCM_PERIOD / 2];
	if (pcm_ring) {
		pcm_fill_ring(s);
		pm_wr(PCM_PA + (uint64_t)idx * PCM_PERIOD, s, sizeof(s));
		return;
	}
	for (unsigned i = 0; i < PCM_PERIOD / 4; i++) {
		int16_t v = edge_ms() < pcm_until ? (int16_t)(1000.0 * sin(pcm_phase)) : 0;
		pcm_phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
		s[2 * i] = s[2 * i + 1] = v;
	}
	pm_wr(PCM_PA + (uint64_t)idx * PCM_PERIOD, s, sizeof(s));
}

static int pcm_write(unsigned idx)
{
	uint32_t wr[8] = { (uint32_t)(PCM_PA + (uint64_t)idx * PCM_PERIOD), PCM_SID_MSW, pcm_mem_handle, PCM_PERIOD, idx,
			   0, 0, 0 };
	uint8_t b[sizeof(struct apr_hdr) + sizeof(wr)];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD_CMD, .pkt_size = sizeof(b),
		.src_svc = APR_SVC_ASM, .src_domain = APR_DOMAIN_APPS, .src_port = PCM_PORT,
		.dest_svc = APR_SVC_ASM, .dest_domain = APR_DOMAIN_ADSP, .dest_port = PCM_PORT,
		.token = idx, .opcode = ASM_DATA_CMD_WRITE_V2,
	};
	memcpy(b, &h, sizeof(h));
	memcpy(b + sizeof(h), wr, sizeof(wr));
	if (edge_send(CH_APR, b, sizeof(b)))
		return -1;
	pcm_inflight++;
	pcm_written++;
	return 0;
}

static void asm_write_done(uint32_t token, const uint32_t *w)
{
	if (pcm_inflight)
		pcm_inflight--;
	pcm_done++;
	if (w[3])
		printf("asm: write done %u status %#x\n", token, w[3]);
	if (pcm_open && (pcm_ring || edge_ms() < pcm_until + 500)) {
		pcm_fill(token % PCM_PERIODS);
		pcm_write(token % PCM_PERIODS);
	}
}

static int pcm_start(void)
{
	if (pcm_open)
		return 0;
	if (smmu_adsp_identity())
		return -1;
	/* what an earlier adspd left open in this ADSP boot (q6asm close, q6adm_device_close: dest_port = copp) */
	int c1 = apr_cmd_ex(APR_SVC_ASM, ASM_STREAM_CMD_CLOSE, PCM_PORT, PCM_SESSION, NULL, 0, 0);
	int c2 = apr_cmd_ports(APR_SVC_ADM, ADM_CMD_DEVICE_CLOSE_V5, AFE_PORT_QUAT_MI2S_RX, 0,
			       (uint32_t)AFE_PORT_QUAT_MI2S_RX << 16, NULL, 0, 0);
	printf("pcm: leftovers closed: session %d, copp 0 %d\n", c1, c2);
	FILE *hf = fopen(PCM_HANDLE_FILE, "r");
	uint32_t old = 0;
	if (hf) {
		if (fscanf(hf, "%x", &old) == 1 && old) {
			int c3 = apr_cmd_ex(APR_SVC_ASM, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, 0, PCM_SESSION << 8, &old, sizeof(old), 0);
			printf("pcm: unmapped the earlier ring handle %#x: %d\n", old, c3);
		}
		fclose(hf);
		unlink(PCM_HANDLE_FILE);
	}
	/* shared memory for the ring */
	struct __attribute__((packed)) {
		uint16_t pool, nregions;
		uint32_t flags;
		uint32_t lsw, msw, size;
	} map = { ADSP_MEMORY_MAP_SHMEM8_4K_POOL, 1, 0, (uint32_t)PCM_PA, PCM_SID_MSW, PCM_PERIOD * PCM_PERIODS };
	int st = apr_cmd_ex(APR_SVC_ASM, ASM_CMD_SHARED_MEM_MAP_REGIONS, 0, PCM_SESSION << 8, &map, sizeof(map),
			    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS);
	pcm_mem_handle = rsp2_w[0];
	printf("asm: map %llx+%x -> %d, handle %#x\n", (unsigned long long)PCM_PA, PCM_PERIOD * PCM_PERIODS, st,
	       pcm_mem_handle);
	if (st || !pcm_mem_handle)
		return -1;
	if ((hf = fopen(PCM_HANDLE_FILE, "w"))) {
		fprintf(hf, "%x\n", pcm_mem_handle);
		fclose(hf);
	}
	/* the stream */
	struct __attribute__((packed)) {
		uint32_t mode_flags;
		uint16_t sink, bits;
		uint32_t postproc, fmt;
	} open = { 0, 0, 16, ASM_NULL_POPP_TOPOLOGY, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
	st = apr_cmd_ex(APR_SVC_ASM, ASM_STREAM_CMD_OPEN_WRITE_V3, PCM_PORT, PCM_SESSION, &open, sizeof(open), 0);
	printf("asm: open write session %u -> %d\n", PCM_SESSION, st);
	if (st)
		return -1;
	/* the COPP on the speaker port */
	struct __attribute__((packed)) {
		uint16_t flags, mode, ep1, ep2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} adm = { 0, ADM_PATH_PLAYBACK, AFE_PORT_QUAT_MI2S_RX, 0xffff, NULL_COPP_TOPOLOGY, 2, 16, 48000, { 1, 2 } };
	st = apr_cmd_ex(APR_SVC_ADM, ADM_CMD_DEVICE_OPEN_V5, AFE_PORT_QUAT_MI2S_RX, (uint32_t)AFE_PORT_QUAT_MI2S_RX << 16,
			&adm, sizeof(adm), ADM_CMDRSP_DEVICE_OPEN_V5);
	if (!st && rsp2_opcode == ADM_CMDRSP_DEVICE_OPEN_V5)
		st = (int)rsp2_w[0];
	pcm_copp = st ? -1 : (int)(rsp2_w[1] & 0xffff);	/* { status, u16 copp_id, u16 reserved } */
	printf("adm: open port %#x -> %d, copp %d\n", AFE_PORT_QUAT_MI2S_RX, st, pcm_copp);
	if (st)
		return -1;
	/* route the session to it */
	struct __attribute__((packed)) {
		uint32_t matrix, nsessions;
		uint16_t session, ncopps;
		uint16_t copp, pad;
	} route = { 0, 1, PCM_SESSION, 1, (uint16_t)pcm_copp, 0 };
	st = apr_cmd_ex(APR_SVC_ADM, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, 0, 0, &route, sizeof(route), 0);
	printf("adm: route session %u -> copp %d: %d\n", PCM_SESSION, pcm_copp, st);
	if (st)
		return -1;
	/* format and run */
	struct __attribute__((packed)) {
		uint32_t blk_size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} fmt = { 20, 2, 16, 48000, 1, 0, { 1, 2 } };
	st = apr_cmd_ex(APR_SVC_ASM, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, PCM_PORT, PCM_SESSION, &fmt, sizeof(fmt), 0);
	printf("asm: media format 48000 Hz 2 ch 16-bit -> %d\n", st);
	if (st)
		return -1;
	uint32_t run[3] = { 0, 0, 0 };
	st = apr_cmd_ex(APR_SVC_ASM, ASM_SESSION_CMD_RUN_V2, PCM_PORT, PCM_SESSION, run, sizeof(run), 0);
	printf("asm: run -> %d\n", st);
	if (st)
		return -1;
	pcm_open = 1;
	return 0;
}

/* `tone MS`: the sine for MS milliseconds, then silence */
static void pcm_tone(unsigned ms)
{
	if (pcm_start())
		return;
	pcm_until = edge_ms() + ms;
	pcm_written = pcm_done = 0;
	if (!pcm_inflight)
		for (unsigned i = 0; i < PCM_INFLIGHT; i++) {
			pcm_fill(i);
			pcm_write(i);
		}
	printf("tone: 440 Hz for %u ms\n", ms);
}

/* `pcm on`: the stream runs on and plays SweetCodec's ring; `pcm off`: back to silence and stop */
static void pcm_ring_play(int on)
{
	if (!on) {
		pcm_ring = NULL;	/* the periods in flight drain, then nothing is refilled */
		pcm_until = 0;
		printf("pcm: ring off\n");
		return;
	}
	if (pcm_ring_open() || pcm_start())
		return;
	pcm_underruns = pcm_skips = 0;
	if (!pcm_inflight)
		for (unsigned i = 0; i < PCM_INFLIGHT; i++) {
			pcm_fill(i);
			pcm_write(i);
		}
	printf("pcm: playing the ring\n");
}

static void apr_rx_apps2(const uint8_t *d, uint32_t n)
{
	printf("[%6llu] apr_apps2: %u bytes\n", (unsigned long long)edge_ms(), n);
	apr_rx(d, n);
}

/*
 * ---- apps SMMU for the ADSP's DMA (as userland/wland/radio.c does for WLAN) ----
 * The ADSP reaches host memory through the apps SMMU (adsp_tbu, streams 0x1800-0x1bff): FastRPC sessions
 * 0x1b23-0x1b25 ("adsprpc-smd" context banks of the live DT), audio ION buffers 0x1b21, USB audio 0x1b2f.
 * sCR0.USFCFG is set and nothing matches those streams after ABL: the first CREATE_STATIC (physical
 * addresses, no stream set up) put the SoC into crashdump. Each stream goes to a context bank with
 * translation off, so the ADSP's DMA uses physical addresses.
 */
#define SMMU_PA		0x15000000ull
#define SMMU_SIZE	0x00100000u
#define CB_CBAR		0x0001f000u	/* S1 translation, S2 bypass, as ABL's banks */
#define CB_CBA2R	0x00000001u	/* AArch64 */
#define CB_SCTLR	0x000000e0u	/* CFCFG | CFIE | CFRE, M = 0: no translation */

static int smmu_done;

static int sid_match(uint32_t smr, uint32_t sid, uint32_t mask)
{
	uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
	return ((id ^ sid) & ~(m | mask) & 0x7fff) == 0;
}

/*
 * the streams, one SMR each: the hypervisor takes only the HLOS streams of the live DT (FastRPC 0x1b23-0x1b25,
 * audio ION 0x1b21, USB audio 0x1b2f); an SMR with a mask, or any other 0x1b2x, reads back 0
 */
static const uint16_t adsp_sids[] = { 0x1b21, 0x1b23, 0x1b24, 0x1b25, 0x1b2f };

static int smmu_adsp_identity(void)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	uint64_t gr1 = s + psize;
	unsigned nsid = sizeof(adsp_sids) / sizeof(adsp_sids[0]), ok = 0;
	int cb = -1;
	/* a bank already ours (our CBAR and SCTLR, M off), else the lowest free one */
	for (uint32_t c = 0; c < ncb && c < 256 && cb < 0; c++) {
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
		if (rd32(gr1 + 4 * c) == CB_CBAR && rd32(cbp) == CB_SCTLR && c >= 5)
			cb = (int)c;
	}
	uint64_t used[4] = { 0 };
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i), s2cr = rd32(s + 0xc00 + 4 * i);
		if ((smr >> 31) && ((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
	}
	for (uint32_t c = 0; c < ncb && c < 256 && cb < 0; c++) {
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbp) & 1) && rd32(gr1 + 4 * c) == 0)
			cb = (int)c;
	}
	if (cb < 0 || (uint64_t)(npage + cb + 1) * psize > SMMU_SIZE) {
		printf("smmu: no free bank\n");
		return -1;
	}
	uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
	if (rd32(gr1 + 4 * cb) != CB_CBAR || rd32(cbp) != CB_SCTLR) {
		wr32(cbp + 0x58, 0xffffffff);
		wr32(cbp + 0x20, 0);
		wr32(cbp + 0x24, 0);
		wr32(cbp + 0x30, 0);
		wr32(cbp + 0x10, 0);
		wr32(cbp + 0x38, 0);
		wr32(cbp + 0x3c, 0);
		wr32(gr1 + 0x800 + 4 * cb, CB_CBA2R);
		wr32(gr1 + 4 * cb, CB_CBAR);
		wr32(cbp, CB_SCTLR);
	}
	for (unsigned k = 0; k < nsid; k++) {
		int slot = -1, have = -1;
		for (uint32_t i = 0; i < nsmr && i < 128; i++) {
			uint32_t smr = rd32(s + 0x800 + 4 * i);
			if ((smr >> 31) && sid_match(smr, adsp_sids[k], 0))
				have = (int)i;
			else if (!(smr >> 31) && slot < 0)
				slot = (int)i;
		}
		if (have >= 0) {
			uint32_t s2cr = rd32(s + 0xc00 + 4 * have);
			int good = ((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == (uint32_t)cb;
			printf("smmu: %#x already in SMR%d (S2CR %08x)%s\n", adsp_sids[k], have, s2cr, good ? "" : " -> another bank");
			ok += good;
			continue;
		}
		if (slot < 0) {
			printf("smmu: no free SMR for %#x\n", adsp_sids[k]);
			continue;
		}
		wr32(s + 0xc00 + 4 * slot, (uint32_t)cb);
		wr32(s + 0x800 + 4 * slot, (1u << 31) | adsp_sids[k]);
		uint32_t smr = rd32(s + 0x800 + 4 * slot), s2cr = rd32(s + 0xc00 + 4 * slot);
		int good = (smr >> 31) && (smr & 0x7fff) == adsp_sids[k] && (s2cr & 0xff) == (uint32_t)cb;
		if (!good)
			wr32(s + 0xc00 + 4 * slot, 2u << 16);	/* leave nothing half-written: fault */
		printf("smmu: %#x -> CB%d via SMR%d: SMR %08x S2CR %08x -> %s\n", adsp_sids[k], cb, slot, smr, s2cr,
		       good ? "ok" : "NOT as written");
		ok += good;
	}
	wr32(s + 0x70, 0);
	for (int i = 0; i < 1000 && (rd32(s + 0x74) & 1); i++)
		usleep(1000);
	smmu_done = ok == nsid;
	printf("smmu: %u of %u ADSP streams through CB%d (SCTLR %08x CBAR %08x, translation off)\n", ok, nsid, cb, rd32(cbp),
	       rd32(gr1 + 4 * cb));
	return smmu_done ? 0 : -1;
}

/*
 * ---- FastRPC (Linux drivers/misc/fastrpc.c; downstream drivers/char/adsprpc.c of this phone) ----
 * Android starts the audio PD with `adsprpcd audiopd`: INIT_CREATE_STATIC (method 8 on the init handle 1)
 * with { pgid, namelen, pageslen }, the name and one page: the PD's remote heap. Without it AFE, ASM and ADM
 * (all in the audio PD) never answer. The ADSP session has no SMMU (no ADSP compute context bank in the DT)
 * and no remote-heap VMID, so addresses are physical (through the identity context bank above) and the heap is plain memory: the ADSP's CMA region
 * adsp_region of Android (0xfd800000, 16 MB below 4 GB), which XNU does not use (its RAM starts at 4 GB).
 */
#define RPC_HEAP_PA	0xfd800000ull
#define RPC_HEAP_SIZE	0x00f00000ull
#define RPC_BUF_PA	0xfe700000ull	/* the invoke buffer: metadata + copies of the arguments */
#define RPC_BUF_SIZE	0x00100000ull
#define RPC_ALIGN	128
#define RPC_INIT_HANDLE	1
/*
 * The session's context bank in bits 32+ of every address the ADSP is given for the invoke buffer (adsprpc.c
 * fastrpc_buf_alloc: buf->phys += smmu.cb << 32, cb = iommus SID & 0xf): the ADSP picks its stream from it.
 * The first ADSP session is 0x1b23, cb 3. Bare physical addresses meant cb 0 (stream 0x1b20, not set up):
 * most likely the crashdump of every earlier invoke. The audio PD's remote heap stays physical (CMA, no cb).
 */
#define RPC_CB		3ull
#define RPC_DSP(pa)	((RPC_CB << 32) | (pa))
#define RPC_SCALARS(m, in, out)	((((m) & 0x1f) << 24) | (((in) & 0xff) << 16) | (((out) & 0xff) << 8))

struct rpc_msg {
	int32_t pid, tid;
	uint64_t ctx;
	uint32_t handle, sc;
	uint64_t addr, size;
} __attribute__((packed));

/*
 * Calls by slot, each with its own part of the invoke buffer: slot 0 for calls we wait for, slot 1 for the
 * listener's next2, which the ADSP holds until the audio PD calls the HLOS. Answers are matched by ctx.
 */
#define RPC_SLOTS	2
#define RPC_SLOT_SIZE	0x80000u
#define RPC_SLOT_PA(s)	(RPC_BUF_PA + (uint64_t)(s) * RPC_SLOT_SIZE)
#define RPC_PID		1234	/* the audio PD's process (CREATE_STATIC pgid): user calls carry it as pid */
#define RPC_MAXBUF	8

struct rpc_call {
	int busy, done;
	uint64_t ctx;
	int32_t ret;
	uint32_t outoff[RPC_MAXBUF], outlen[RPC_MAXBUF];
	unsigned nout;
};
static struct rpc_call rpc_calls[RPC_SLOTS];

static void fastrpc_rx(const uint8_t *d, uint32_t n)
{
	uint64_t ctx = 0;
	int32_t ret = 0;
	if (n >= 12) {
		memcpy(&ctx, d, 8);
		memcpy(&ret, d + 8, 4);
	}
	for (unsigned s = 0; s < RPC_SLOTS; s++)
		if (rpc_calls[s].busy && rpc_calls[s].ctx == ctx) {
			rpc_calls[s].ret = ret;
			rpc_calls[s].done = 1;
			if (s == 0 || ret)
				printf("[%6llu] fastrpc: < slot %u ctx %#llx retval %d (%#x)\n", (unsigned long long)edge_ms(), s,
				       (unsigned long long)ctx, ret, (uint32_t)ret);
			return;
		}
	printf("[%6llu] fastrpc: < %u bytes for no call: ctx %#llx retval %d\n", (unsigned long long)edge_ms(), n,
	       (unsigned long long)ctx, ret);
}

/*
 * Send an invoke (fastrpc_get_args layout): nin in-buffers copied into the slot, nout out-buffers of outcap
 * bytes reserved after them. Addresses as the ADSP sees them carry the context bank (RPC_DSP).
 */
static int rpc_send(unsigned slot, int32_t pid, uint32_t handle, uint32_t method, const void *const *in,
		    const uint32_t *inlen, unsigned nin, const uint32_t *outcap, unsigned nout, uint32_t pd)
{
	struct rpc_call *c = &rpc_calls[slot];
	unsigned n = nin + nout;
	uint32_t meta = n * (16 + 8 + 16) + 8 * 16 + 4 * 64;	/* remote_buf, invoke_buf, phy_page; fdlist, crclist */
	static uint8_t b[RPC_SLOT_SIZE];
	uint64_t base = RPC_SLOT_PA(slot);
	if (n > RPC_MAXBUF)
		return -1;
	memset(b, 0, meta);
	uint32_t off = meta;
	for (unsigned i = 0; i < n; i++) {
		off = (off + RPC_ALIGN - 1) & ~(uint32_t)(RPC_ALIGN - 1);
		uint64_t len = i < nin ? inlen[i] : outcap[i - nin];
		if (off + len > RPC_SLOT_SIZE)
			return -1;
		uint64_t pv = base + off;
		uint64_t pg = pv & ~0xfffull, pgs = (((pv + (len ? len : 1) - 1) & ~0xfffull) - pg) / 0x1000 + 1;
		pg = RPC_DSP(pg);
		memcpy(b + 16 * i, &pv, 8);
		memcpy(b + 16 * i + 8, &len, 8);
		uint32_t num = len ? 1 : 0, pgidx = i;
		memcpy(b + 16 * n + 8 * i, &num, 4);
		memcpy(b + 16 * n + 8 * i + 4, &pgidx, 4);
		uint64_t psz = pgs * 0x1000;
		memcpy(b + 24 * n + 16 * i, &pg, 8);
		memcpy(b + 24 * n + 16 * i + 8, &psz, 8);
		if (i < nin) {
			memcpy(b + off, in[i], inlen[i]);
		} else {
			memset(b + off, 0, len);
			c->outoff[i - nin] = off;
			c->outlen[i - nin] = (uint32_t)len;
		}
		off += (uint32_t)len;
	}
	uint32_t size = (off + 0xfff) & ~0xfffu;
	pm_wr(base, b, (off + 3) & ~3u);
	static uint32_t ctxseq = 1;
	struct rpc_msg m = {
		.pid = pid, .tid = 1234 + (int32_t)slot, .ctx = ((uint64_t)(ctxseq++) << 8) | pd, .handle = handle,
		.sc = RPC_SCALARS(method, nin, nout), .addr = RPC_DSP(base), .size = size,
	};
	c->busy = 1;
	c->done = 0;
	c->ctx = m.ctx;
	c->nout = nout;
	if (slot == 0)
		printf("[%6llu] fastrpc: > handle %u method %u, %u in / %u out, buffer %llx+%x, ctx %#llx\n",
		       (unsigned long long)edge_ms(), handle, method, nin, nout, (unsigned long long)m.addr, size,
		       (unsigned long long)m.ctx);
	if (edge_send(CH_RPC, &m, sizeof(m))) {
		c->busy = 0;
		return -1;
	}
	return 0;
}

/* the call in slot answered: its retval, or -1 after ms */
static int rpc_wait(unsigned slot, unsigned ms)
{
	struct rpc_call *c = &rpc_calls[slot];
	for (unsigned t = 0; t < ms / 10 && !c->done; t++) {
		edge_poll(10);
		smp2p_service();
	}
	if (!c->done) {
		printf("fastrpc: slot %u: no answer in %u ms\n", slot, ms);
		c->busy = 0;
		return -1;
	}
	c->busy = 0;
	return c->ret;
}

/* out-buffer i of the answered call in slot */
static int rpc_out(unsigned slot, unsigned i, void *dst, uint32_t len)
{
	struct rpc_call *c = &rpc_calls[slot];
	if (i >= c->nout || len > c->outlen[i])
		return -1;
	return pm_rd(RPC_SLOT_PA(slot) + c->outoff[i], dst, len);
}

/* a kernel call (pid 0) with in-buffers only, as the driver's init calls */
static int fastrpc_invoke(uint32_t handle, uint32_t method, const void *const *in, const uint32_t *inlen, unsigned nin,
			  uint32_t pd)
{
	if (rpc_send(0, 0, handle, method, in, inlen, nin, NULL, 0, pd))
		return -1;
	return rpc_wait(0, 10000);
}

/* INIT_ATTACH (method 0 on the init handle, in: the pgid): joins the ADSP's guest OS process. No heap and no
 * new PD: only the transport and the argument buffer through the SMMU, to tell those from the audio PD. */
static int fastrpc_attach(void)
{
	if (!edge_ready(CH_RPC) || smmu_adsp_identity()) {
		printf("attach: channel or SMMU streams not ready\n");
		return -1;
	}
	int32_t pgid = 1234;
	const void *in[1] = { &pgid };
	const uint32_t len[1] = { sizeof(pgid) };
	int r = fastrpc_invoke(RPC_INIT_HANDLE, 0, in, len, 1, 0);
	printf("attach: -> %d\n", r);
	return r;
}

static int audiopd_create(void)
{
	if (!edge_ready(CH_RPC)) {
		printf("audiopd: %s not open (open it first)\n", CH_RPC->name);
		return -1;
	}
	if (smmu_adsp_identity()) {
		printf("audiopd: the ADSP's SMMU streams are not set up: not invoking (crashdump otherwise)\n");
		return -1;
	}
	static const char name[] = "audiopd";
	struct { int32_t pgid; uint32_t namelen, pageslen; } inbuf = { 1234, sizeof(name), 1 };
	struct { uint64_t addr, size; } page = { RPC_HEAP_PA, RPC_HEAP_SIZE };
	const void *in[3] = { &inbuf, name, &page };
	const uint32_t len[3] = { sizeof(inbuf), sizeof(name), sizeof(page) };
	printf("audiopd: CREATE_STATIC \"%s\", remote heap %llx+%llx\n", name, (unsigned long long)RPC_HEAP_PA,
	       (unsigned long long)RPC_HEAP_SIZE);
	int r = fastrpc_invoke(RPC_INIT_HANDLE, 8, in, len, 3, 1);
	printf("audiopd: -> %d\n", r);
	return r;
}

/*
 * ---- default listener (quic/fastrpc adsp_default_listener.c, listener_android.c; BSD-3-Clause) ----
 * Android's `adsprpcd audiopd` then opens "adsp_default_listener" through remotectl (handle 0), registers it
 * and serves the audio PD's calls to the HLOS in a loop of adsp_listener next2 (handle 3, method 4): each
 * answer carries the next call (ctx, handle, sc and its in-buffers packed as listener_buf.h: per buffer a
 * 32-bit length, 8-byte alignment, the bytes; then the out-buffer lengths). This first version only logs the
 * calls and answers AEE_EUNSUPPORTED, to see what the PD asks for.
 */
#define RPC_REMOTECTL_HANDLE	0
#define RPC_LISTENER_HANDLE	3
#define LST_BUFCAP		0x10000u
#define AEE_EUNSUPPORTED	20

static int lst_on;
static uint32_t lst_ctx;
static int32_t lst_result;
static uint8_t lst_out[LST_BUFCAP];
static uint32_t lst_outlen;
static unsigned lst_calls;

static int remotectl_open(const char *name, uint32_t *handle)
{
	uint32_t namelen = (uint32_t)strlen(name) + 1, prim[2] = { namelen, 128 };
	const void *in[2] = { prim, name };
	const uint32_t inlen[2] = { sizeof(prim), namelen }, outcap[2] = { 8, 128 };
	if (rpc_send(0, RPC_PID, RPC_REMOTECTL_HANDLE, 0, in, inlen, 2, outcap, 2, 1))
		return -1;
	int r = rpc_wait(0, 10000);
	uint32_t rout[2] = { 0, 0 };
	char dlerr[128] = { 0 };
	if (r == 0) {
		rpc_out(0, 0, rout, sizeof(rout));
		rpc_out(0, 1, dlerr, sizeof(dlerr));
		dlerr[sizeof(dlerr) - 1] = 0;
	}
	printf("remotectl: open \"%s\" -> %d, handle %#x, nErr %d%s%s\n", name, r, rout[0], (int32_t)rout[1],
	       dlerr[0] ? ", dlerror: " : "", dlerr);
	*handle = rout[0];
	return r ? r : (int32_t)rout[1];
}

static int rpc_call0(uint32_t handle, uint32_t method, const char *what)
{
	if (rpc_send(0, RPC_PID, handle, method, NULL, NULL, 0, NULL, 0, 1))
		return -1;
	int r = rpc_wait(0, 10000);
	printf("%s -> %d\n", what, r);
	return r;
}

/* next2: the answer to the previous call (lst_ctx, lst_result, lst_out) and room for the next one */
static int listener_next(void)
{
	uint32_t prim[4] = { lst_ctx, (uint32_t)lst_result, lst_outlen, LST_BUFCAP };
	const void *in[2] = { prim, lst_out };
	const uint32_t inlen[2] = { sizeof(prim), lst_outlen }, outcap[2] = { 16, LST_BUFCAP };
	return rpc_send(1, RPC_PID, RPC_LISTENER_HANDLE, 4, in, inlen, 2, outcap, 2, 1);
}

static void dump_buf(const char *what, const uint8_t *p, uint32_t n)
{
	int text = n > 1;
	for (uint32_t i = 0; i < n; i++)
		if ((p[i] < 0x20 || p[i] > 0x7e) && !(p[i] == 0 && i == n - 1))
			text = 0;
	if (text) {
		printf("%s%u bytes \"%.*s\"\n", what, n, (int)n, (const char *)p);
		return;
	}
	printf("%s%u bytes:", what, n);
	for (uint32_t i = 0; i < n && i < 48; i++)
		printf(" %02x", p[i]);
	printf("%s\n", n > 48 ? " ..." : "");
}

/* one call from the audio PD: log it, answer AEE_EUNSUPPORTED */
static void listener_dispatch(uint32_t handle, uint32_t sc, const uint8_t *bufs, uint32_t n)
{
	unsigned method = (sc >> 24) & 0x1f, nin = (sc >> 16) & 0xff, nout = (sc >> 8) & 0xff;
	printf("[%6llu] listener: call %u: handle %#x method %u, %u in / %u out (sc %#x)\n", (unsigned long long)edge_ms(),
	       ++lst_calls, handle, method, nin, nout, sc);
	uint32_t off = 0;
	for (unsigned i = 0; i < nin && off + 4 <= n; i++) {
		uint32_t len;
		memcpy(&len, bufs + off, 4);
		off += 4;
		if (len) {
			off = (off + 7) & ~7u;
			if (off + len > n) {
				printf("listener:   in %u: %u bytes past the end\n", i, len);
				break;
			}
			char what[32];
			snprintf(what, sizeof(what), "listener:   in %u: ", i);
			dump_buf(what, bufs + off, len);
			off += len;
		} else {
			printf("listener:   in %u: empty\n", i);
		}
	}
	for (unsigned i = 0; i < nout && off + 4 <= n; i++) {
		uint32_t len;
		memcpy(&len, bufs + off, 4);
		off += 4;
		printf("listener:   out %u: %u bytes\n", i, len);
	}
	lst_result = AEE_EUNSUPPORTED;
	lst_outlen = 0;
}

static int listener_start(void)
{
	uint32_t h = 0;
	if (remotectl_open("adsp_default_listener", &h))
		return -1;
	if (rpc_call0(h, 0, "listener: adsp_default_listener register") ||
	    rpc_call0(RPC_LISTENER_HANDLE, 3, "listener: adsp_listener init2"))
		return -1;
	lst_ctx = 0;
	lst_result = 0;
	lst_outlen = 0;
	lst_calls = 0;
	if (listener_next())
		return -1;
	lst_on = 1;
	printf("listener: waiting for the audio PD's calls\n");
	return 0;
}

/* from the serve loop: a call came in (the next2 answered) */
static void listener_service(void)
{
	struct rpc_call *c = &rpc_calls[1];
	if (!lst_on || !c->done)
		return;
	c->busy = 0;
	if (c->ret) {
		printf("listener: next2 -> %d (%#x), stopping\n", c->ret, (uint32_t)c->ret);
		lst_on = 0;
		return;
	}
	uint32_t rout[4];
	static uint8_t bufs[LST_BUFCAP];
	rpc_out(1, 0, rout, sizeof(rout));
	uint32_t n = rout[3] < LST_BUFCAP ? rout[3] : LST_BUFCAP;
	rpc_out(1, 1, bufs, n);
	lst_ctx = rout[0];
	listener_dispatch(rout[1], rout[2], bufs, n);
	if (listener_next()) {
		printf("listener: next2 send failed, stopping\n");
		lst_on = 0;
	}
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
	chans[3].rx = fastrpc_rx;
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
 *   APR command) | smmu (the ADSP's streams untranslated) | attach (FastRPC INIT_ATTACH) | audiopd (FastRPC CREATE_STATIC of the
 *   audio PD) | stop (graceful ADSP shutdown,
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
	} else if (!strncmp(line, "tone", 4)) {
		pcm_tone(line[4] ? (unsigned)atoi(line + 5) : 1000);
	} else if (!strcmp(line, "pcm on") || !strcmp(line, "pcm off")) {
		pcm_ring_play(!strcmp(line, "pcm on"));
	} else if (!strcmp(line, "pcm stats")) {
		printf("pcm: open %d copp %d, %u written, %u done, %u in flight, %u underruns, %u skips, ring %s wpos %llu rpos %llu\n",
		       pcm_open, pcm_copp, pcm_written, pcm_done, pcm_inflight, pcm_underruns, pcm_skips, pcm_ring ? "on" : "off",
		       pcm_ring ? (unsigned long long)pcm_ring->wpos : 0ull, pcm_ring ? (unsigned long long)pcm_ring->rpos : 0ull);
	} else if (!strcmp(line, "afe probe")) {
		afe_probe();
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
	} else if (!strncmp(line, "verbose ", 8)) {
		edge_verbose = atoi(line + 8);
	} else if (!strcmp(line, "smmu")) {
		smmu_adsp_identity();
	} else if (!strcmp(line, "attach")) {
		fastrpc_attach();
	} else if (!strcmp(line, "audiopd")) {
		audiopd_create();
	} else if (!strcmp(line, "listener")) {
		listener_start();
	} else if (!strncmp(line, "pd ", 3)) {
		servreg_query(line + 3);
	} else if (!strcmp(line, "status")) {
		apr_send(APR_SVC_ADSP_CORE, AVCS_EVENT_GET_STATE, NULL, 0);
	}
}

/*
 * The boot script (/var/local/adspd.boot, from the com.sweet.adspd job): serve's commands, one per line, run
 * once the ADSP is up, plus `sleep SECS` (the ADSP keeps being serviced) and `exec PROGRAM ARGS...` (waited
 * for, e.g. tfa init once AFE has the I2S clock running). '#' starts a comment.
 */
#define BOOT_SCRIPT "/var/local/adspd.boot"
extern char **environ;

static void command(char *line);

static void run_exec(char *args)
{
	char *argv[16];
	int n = 0;
	for (char *t = strtok(args, " "); t && n < 15; t = strtok(NULL, " "))
		argv[n++] = t;
	argv[n] = NULL;
	if (!n)
		return;
	pid_t pid;
	int st = 0;
	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ)) {
		printf("boot: cannot run %s\n", argv[0]);
		return;
	}
	while (waitpid(pid, &st, WNOHANG) == 0)
		idle(50);
	printf("boot: %s -> %d\n", argv[0], WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static void run_boot_script(void)
{
	FILE *f = fopen(BOOT_SCRIPT, "r");
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "#\r\n")] = 0;
		size_t n = strlen(line);
		while (n && line[n - 1] == ' ')
			line[--n] = 0;
		if (!n)
			continue;
		printf("[%6llu] boot: %s\n", (unsigned long long)edge_ms(), line);
		if (!strncmp(line, "sleep ", 6))
			idle(1000u * (unsigned)atoi(line + 6));
		else if (!strncmp(line, "exec ", 5))
			run_exec(line + 5);
		else
			command(line);
	}
	fclose(f);
	printf("boot: %s done\n", BOOT_SCRIPT);
}

static int serve(void)
{
	signal(SIGPIPE, SIG_IGN);
	/*
	 * ADSPD_BOOT=SECS (the com.sweet.adspd job): the ADSP is not running yet in this boot, so the state an
	 * adspd of the last boot saved is stale; wait SECS first (Wi-Fi comes up meanwhile: an ADSP started
	 * beside wland's bring-up is untried), then boot it fresh.
	 */
	const char *boot = getenv("ADSPD_BOOT");
	if (boot) {
		unlink(STATE_FILE);
		unlink(QRTR_STATE_FILE);
		unlink(PCM_HANDLE_FILE);
		printf("serve: boot start in %s s\n", boot);
		sleep((unsigned)atoi(boot));
	}
	if (bring_up(0))
		return 1;
	unlink(CMD_FIFO);
	if (mkfifo(CMD_FIFO, 0600)) {
		perror(CMD_FIFO);
		return 1;
	}
	int fd = open(CMD_FIFO, O_RDWR | O_NONBLOCK);	/* RDWR: never EOF when writers come and go */
	printf("serve: commands through %s\n", CMD_FIFO);
	run_boot_script();
	char buf[256];
	size_t have = 0;
	for (;;) {
		edge_poll(20);
		smp2p_service();
		listener_service();
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

/*
 * launchd gives daemons a 6 MB jetsam limit (JetsamMemoryLimit in our job is not applied), and the ADSP
 * firmware alone is 13 MB: the com.sweet.adspd job was killed (JETSAM_REASON_MEMORY_PERPROCESSLIMIT) in the
 * middle of the PAS boot. Raise our own limit as launchd would (xnu bsd/sys/kern_memorystatus.h).
 */
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES	7
struct memorystatus_memlimit_properties {
	int32_t memlimit_active;	/* MB */
	uint32_t memlimit_active_attr;
	int32_t memlimit_inactive;
	uint32_t memlimit_inactive_attr;
};
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

static void raise_memlimit(void)
{
	struct memorystatus_memlimit_properties m = { 256, 0, 256, 0 };
	if (memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, getpid(), 0, &m, sizeof(m)))
		perror("memorystatus_control");
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	raise_memlimit();
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

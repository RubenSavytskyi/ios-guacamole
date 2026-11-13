/*
 * GLINK over SMEM with the ADSP, several channels (see edge.h). The FIFOs:
 * SMEM items 478 (descriptor), 479 (ours), 480 (the remote's) in the
 * partition shared with the remote host, as Linux qcom_glink_smem_register.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "edge.h"
#include "../wland/glink.h"
#include "../wland/pmem.h"
#include "../wland/smem.h"

#define DESC_ITEM	478
#define FIFO_TX_ITEM	479
#define FIFO_RX_ITEM	480
#define APCS_IPC	0x17c0000cull
#define N_INTENTS	8
#define INTENT_SIZE	4096
#define FEAT_INTENT_REUSE 1
#define MAX_RINTENTS	256

int edge_verbose = 1;
static unsigned ipc;
static uint64_t desc, tx_fifo, rx_fifo;
static uint32_t tx_len, rx_len;
static int version_done;
static struct edge_chan *chan;
static unsigned nchan;
static struct { struct edge_chan *c; uint32_t size, iid; int busy; } rintent[MAX_RINTENTS];
static int nrintent;
static uint8_t rxbuf[65536];
static uint32_t rxoff;

#define TX_TAIL	(desc + 0)
#define TX_HEAD	(desc + 4)
#define RX_TAIL	(desc + 8)
#define RX_HEAD	(desc + 12)

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t t0;
uint64_t edge_ms(void)
{
	if (!t0)
		t0 = now_ms();
	return now_ms() - t0;
}

#define LOG(...) do { if (edge_verbose) { printf("[%6llu] ", (unsigned long long)edge_ms()); printf(__VA_ARGS__); } } while (0)

static void kick(void)
{
	wr32(APCS_IPC, 1u << ipc);
}

static int fifo_send(uint16_t cmd, uint16_t p1, uint32_t p2, const void *data, size_t len)
{
	uint8_t buf[8 + 8192 + 8 + 8] = { 0 };
	size_t n = 8 + len, padded = (n + 7) & ~(size_t)7;
	if (padded > sizeof(buf))
		return -1;
	memcpy(buf, &cmd, 2);
	memcpy(buf + 2, &p1, 2);
	memcpy(buf + 4, &p2, 4);
	if (len)
		memcpy(buf + 8, data, len);
	uint32_t head = rd32(TX_HEAD), tail = rd32(TX_TAIL);
	uint32_t avail = tail <= head ? tx_len - head + tail : tail - head;
	if (avail < padded + 16)
		return -1;
	/* device memory: whole words only */
	for (size_t i = 0; i < padded; i += 4) {
		uint32_t w;
		memcpy(&w, buf + i, 4);
		wr32(tx_fifo + head, w);
		head += 4;
		if (head >= tx_len)
			head -= tx_len;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(TX_HEAD, head);
	kick();
	return 0;
}

static size_t rx_avail(void)
{
	uint32_t head = rd32(RX_HEAD), tail = rd32(RX_TAIL);
	return head < tail ? rx_len - tail + head : head - tail;
}

static void rx_peek(void *buf, size_t off, size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)off;
	if (t >= rx_len)
		t -= rx_len;
	uint8_t *b = buf;
	for (size_t i = 0; i < len; i++) {
		uint32_t w = rd32(rx_fifo + (t & ~3u));
		b[i] = (uint8_t)(w >> (8 * (t & 3)));
		if (++t >= rx_len)
			t = 0;
	}
}

static void rx_advance(size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)len;
	if (t >= rx_len)
		t -= rx_len;
	wr32(RX_TAIL, t);
}

static struct edge_chan *by_rcid(uint32_t rcid)
{
	for (unsigned i = 0; i < nchan; i++)
		if (chan[i].remote_open && chan[i].rcid == rcid)
			return &chan[i];
	return NULL;
}

static struct edge_chan *by_lcid(uint32_t lcid)
{
	for (unsigned i = 0; i < nchan; i++)
		if (chan[i].lcid == lcid)
			return &chan[i];
	return NULL;
}

static void open_local(struct edge_chan *c)
{
	size_t nl = strlen(c->name) + 1;
	fifo_send(GLINK_CMD_OPEN, c->lcid, (uint32_t)nl, c->name, nl);
	c->local_open = 1;
}

static void advertise_intents(struct edge_chan *c)
{
	uint32_t p[N_INTENTS * 2];
	for (int i = 0; i < N_INTENTS; i++) {
		p[2 * i] = INTENT_SIZE;
		p[2 * i + 1] = i + 1;
	}
	fifo_send(GLINK_CMD_INTENT, c->lcid, N_INTENTS, p, sizeof(p));
}

/* one command from the FIFO; 0 if incomplete */
static int rx_one(void)
{
	size_t avail = rx_avail();
	if (avail < 8)
		return 0;
	uint8_t h[16];
	rx_peek(h, 0, 8);
	uint16_t cmd, p1;
	uint32_t p2;
	memcpy(&cmd, h, 2);
	memcpy(&p1, h + 2, 2);
	memcpy(&p2, h + 4, 4);
	size_t adv = 8;
	struct edge_chan *c;

	switch (cmd) {
	case GLINK_CMD_VERSION:
		LOG("glink: < VERSION %u features %#x\n", p1, p2);
		fifo_send(GLINK_CMD_VERSION_ACK, 1, p2 & FEAT_INTENT_REUSE, NULL, 0);
		if (!version_done)
			fifo_send(GLINK_CMD_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
		break;
	case GLINK_CMD_VERSION_ACK:
		LOG("glink: < VERSION_ACK %u features %#x\n", p1, p2);
		version_done = 1;
		break;
	case GLINK_CMD_OPEN: {
		size_t nl = (p2 + 7) & ~7u;
		if (avail < 8 + nl)
			return 0;
		char name[64] = { 0 };
		rx_peek(name, 8, p2 < 63 ? p2 : 63);
		adv += nl;
		fifo_send(GLINK_CMD_OPEN_ACK, p1, 0, NULL, 0);
		c = NULL;
		for (unsigned i = 0; i < nchan; i++)
			if (!strcmp(chan[i].name, name))
				c = &chan[i];
		LOG("glink: < OPEN rcid %u \"%s\"%s\n", p1, name, c ? "" : " (not ours)");
		if (c) {
			c->rcid = p1;
			c->remote_open = 1;
			if (!c->local_open)
				open_local(c);
		}
		break;
	}
	case GLINK_CMD_OPEN_ACK:
		c = by_lcid(p1);
		LOG("glink: < OPEN_ACK lcid %u (%s)\n", p1, c ? c->name : "?");
		if (c && !c->acked) {
			c->acked = 1;
			advertise_intents(c);
		}
		break;
	case GLINK_CMD_CLOSE:
		c = by_rcid(p1);
		LOG("glink: < CLOSE rcid %u (%s)\n", p1, c ? c->name : "?");
		fifo_send(GLINK_CMD_CLOSE_ACK, p1, 0, NULL, 0);
		if (c)
			c->remote_open = 0;
		break;
	case GLINK_CMD_CLOSE_ACK:
		LOG("glink: < CLOSE_ACK %u\n", p1);
		break;
	case GLINK_CMD_INTENT: {
		size_t len = 8 + (size_t)p2 * 8;
		if (avail < len)
			return 0;
		c = by_rcid(p1);
		for (uint32_t i = 0; i < p2; i++) {
			uint32_t pr[2];
			rx_peek(pr, 8 + i * 8, 8);
			if (c && nrintent == MAX_RINTENTS) {
				memmove(rintent, rintent + 1, sizeof(rintent[0]) * (MAX_RINTENTS - 1));
				nrintent--;
			}
			if (c) {
				rintent[nrintent].c = c;
				rintent[nrintent].size = pr[0];
				rintent[nrintent].iid = pr[1];
				rintent[nrintent].busy = 0;
				nrintent++;
			}
		}
		if (edge_verbose > 1)
			LOG("glink: < INTENT x%u on %s\n", p2, c ? c->name : "?");
		adv = (len + 7) & ~7u;
		break;
	}
	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE:
		c = by_rcid(p1);
		for (int i = 0; i < nrintent; i++)
			if (rintent[i].c == c && rintent[i].iid == p2) {
				if (cmd == GLINK_CMD_RX_DONE)
					rintent[i] = rintent[--nrintent];
				else
					rintent[i].busy = 0;
				break;
			}
		break;
	case GLINK_CMD_RX_INTENT_REQ:
		c = by_rcid(p1);
		if (c) {
			static uint32_t next_iid = 100;
			uint32_t p[2] = { p2 > INTENT_SIZE ? p2 : INTENT_SIZE, next_iid++ };
			fifo_send(GLINK_CMD_INTENT, c->lcid, 1, p, sizeof(p));
			fifo_send(GLINK_CMD_RX_INTENT_REQ_ACK, c->lcid, 1, NULL, 0);
		} else {
			fifo_send(GLINK_CMD_RX_INTENT_REQ_ACK, 0, 0, NULL, 0);
		}
		break;
	case GLINK_CMD_RX_INTENT_REQ_ACK:
		if (!p2 || edge_verbose > 1)
			LOG("glink: < RX_INTENT_REQ_ACK %u %s\n", p1, p2 ? "granted" : "refused");
		break;
	case GLINK_CMD_TX_DATA:
	case GLINK_CMD_TX_DATA_CONT: {
		if (avail < 16)
			return 0;
		rx_peek(h, 0, 16);
		uint32_t chunk, left;
		memcpy(&chunk, h + 8, 4);
		memcpy(&left, h + 12, 4);
		size_t len = 16 + chunk;
		if (avail < len)
			return 0;
		if (rxoff + chunk <= sizeof(rxbuf))
			rx_peek(rxbuf + rxoff, 16, chunk);
		rxoff += chunk;
		adv = (len + 7) & ~7u;
		if (!left) {
			c = by_rcid(p1);
			rx_advance(adv);
			uint32_t n = rxoff;
			rxoff = 0;
			if (c) {
				fifo_send(GLINK_CMD_RX_DONE_W_REUSE, c->lcid, p2, NULL, 0);
				if (c->rx)
					c->rx(rxbuf, n);
			} else {
				LOG("glink: < %u bytes on unknown rcid %u\n", n, p1);
			}
			return 1;
		}
		break;
	}
	case GLINK_CMD_READ_NOTIF:
		kick();
		break;
	case GLINK_CMD_SIGNALS:
		c = by_rcid(p1);
		LOG("glink: < SIGNALS %#x on %s\n", p2, c ? c->name : "?");
		break;
	default:
		LOG("glink: < unknown cmd %u (%u %#x); dropping the FIFO\n", cmd, p1, p2);
		adv = avail;
		break;
	}
	rx_advance(adv);
	return 1;
}

int edge_poll(unsigned ms)
{
	uint64_t end = now_ms() + ms;
	int n = 0;
	do {
		while (rx_one())
			n++;
		if (ms)
			delay_ms(1);
	} while (now_ms() < end);
	return n;
}

int edge_ready(const struct edge_chan *c)
{
	return c->remote_open && c->local_open && c->acked;
}

int edge_init(unsigned host, unsigned ipc_bit, struct edge_chan *chans, unsigned n)
{
	ipc = ipc_bit;
	chan = chans;
	nchan = n;
	for (unsigned i = 0; i < n; i++)
		chans[i].lcid = (uint16_t)(i + 1);
	if (smem_alloc(host, DESC_ITEM, 32, &desc) || smem_alloc(host, FIFO_TX_ITEM, 16384, &tx_fifo) ||
	    smem_get(host, FIFO_TX_ITEM, &tx_fifo, &tx_len)) {
		fprintf(stderr, "glink: smem items (host %u)\n", host);
		return -1;
	}
	if (smem_get(host, FIFO_RX_ITEM, &rx_fifo, &rx_len)) {
		fprintf(stderr, "glink: no rx fifo from host %u (item %u): is it running?\n", host, FIFO_RX_ITEM);
		return -1;
	}
	/* as Linux qcom_glink_smem_register: our ends of both pipes start over */
	wr32(RX_TAIL, 0);
	wr32(TX_HEAD, 0);
	printf("glink: host %u desc %llx tx %llx (%u) rx %llx (%u)\n", host, (unsigned long long)desc,
	       (unsigned long long)tx_fifo, tx_len, (unsigned long long)rx_fifo, rx_len);
	fifo_send(GLINK_CMD_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
	return 0;
}

static int find_intent(struct edge_chan *c, uint32_t len)
{
	int best = -1;
	for (int i = 0; i < nrintent; i++)
		if (rintent[i].c == c && !rintent[i].busy && rintent[i].size >= len &&
		    (best < 0 || rintent[i].size < rintent[best].size))
			best = i;
	return best;
}

/* One packet into one of the remote's intents on c (requested if none fits), 8K chunks. */
int edge_send(struct edge_chan *c, const void *data, uint32_t len)
{
	uint64_t end = now_ms() + 3000;
	int i, asked = 0;
	while ((i = find_intent(c, len)) < 0) {
		if (asked < 3) {
			if (edge_verbose > 1)
				LOG("glink: > RX_INTENT_REQ %u bytes on %s (lcid %u)\n", len, c->name, c->lcid);
			fifo_send(GLINK_CMD_RX_INTENT_REQ, c->lcid, len, NULL, 0);
			asked++;
		}
		if (now_ms() > end)
			goto fail;
		edge_poll(5);
	}
	rintent[i].busy = 1;
	const uint8_t *d = data;
	for (uint32_t off = 0; off < len;) {
		uint32_t chunk = len - off > 8192 ? 8192 : len - off;
		uint8_t b[8 + 8192];
		uint32_t hdr[2] = { chunk, len - off - chunk };
		memcpy(b, hdr, 8);
		memcpy(b + 8, d + off, chunk);
		while (fifo_send(off ? GLINK_CMD_TX_DATA_CONT : GLINK_CMD_TX_DATA, c->lcid, rintent[i].iid, b, 8 + chunk)) {
			if (now_ms() > end)
				goto fail;
			edge_poll(1);
		}
		off += chunk;
	}
	return 0;
fail:
	printf("glink: cannot send %u bytes on %s\n", len, c->name);
	return -1;
}

/* ---- handing the edge to a new process (the ADSP must not restart within a boot: see adspd.c) ---- */

#define STATE_V1_INTENTS 64	/* format 1 (EDGE): 64 remote intents; format 2 (EDG2): MAX_RINTENTS */
struct edge_state {
	uint32_t magic, nchan, nrintent, version_done;
	struct { uint32_t rcid, remote_open, local_open, acked; } c[8];
	struct { uint32_t chan, size, iid, busy; } ri[MAX_RINTENTS];
};
#define EDGE_STATE_MAGIC_V1 0x45444745u
#define EDGE_STATE_MAGIC 0x32474445u

int edge_save(const char *path)
{
	struct edge_state st = { .magic = EDGE_STATE_MAGIC, .nchan = nchan, .nrintent = (uint32_t)nrintent,
				 .version_done = (uint32_t)version_done };
	for (unsigned i = 0; i < nchan && i < 8; i++) {
		st.c[i].rcid = chan[i].rcid;
		st.c[i].remote_open = (uint32_t)chan[i].remote_open;
		st.c[i].local_open = (uint32_t)chan[i].local_open;
		st.c[i].acked = (uint32_t)chan[i].acked;
	}
	for (int i = 0; i < nrintent; i++) {
		st.ri[i].chan = (uint32_t)(rintent[i].c - chan);
		st.ri[i].size = rintent[i].size;
		st.ri[i].iid = rintent[i].iid;
		st.ri[i].busy = (uint32_t)rintent[i].busy;
	}
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	int ok = fwrite(&st, sizeof(st), 1, f) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

int edge_adopt(const char *path, unsigned host, unsigned ipc_bit, struct edge_chan *chans, unsigned n)
{
	struct edge_state st;
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	memset(&st, 0, sizeof(st));
	size_t got = fread(&st, 1, sizeof(st), f);
	fclose(f);
	size_t v1 = offsetof(struct edge_state, ri) + STATE_V1_INTENTS * sizeof(st.ri[0]);
	int ok = (st.magic == EDGE_STATE_MAGIC && got == sizeof(st)) ||
		 (st.magic == EDGE_STATE_MAGIC_V1 && got >= v1 && st.nrintent <= STATE_V1_INTENTS);
	if (!ok || st.nchan > n || st.nchan > 8)
		return -1;
	ipc = ipc_bit;
	chan = chans;
	nchan = n;
	if (smem_get(host, DESC_ITEM, &desc, &tx_len) || smem_get(host, FIFO_TX_ITEM, &tx_fifo, &tx_len) ||
	    smem_get(host, FIFO_RX_ITEM, &rx_fifo, &rx_len))
		return -1;
	version_done = (int)st.version_done;
	for (unsigned i = 0; i < n; i++) {
		chans[i].lcid = (uint16_t)(i + 1);
		if (i >= st.nchan)
			continue;	/* added since: closed until edge_open_late */
		chans[i].rcid = st.c[i].rcid;
		chans[i].remote_open = (int)st.c[i].remote_open;
		chans[i].local_open = (int)st.c[i].local_open;
		chans[i].acked = (int)st.c[i].acked;
	}
	nrintent = 0;
	for (uint32_t i = 0; i < st.nrintent && i < MAX_RINTENTS; i++) {
		rintent[nrintent].c = &chans[st.ri[i].chan < n ? st.ri[i].chan : 0];
		rintent[nrintent].size = st.ri[i].size;
		rintent[nrintent].iid = st.ri[i].iid;
		rintent[nrintent].busy = (int)st.ri[i].busy;
		nrintent++;
	}
	printf("glink: adopted the running edge (host %u, %d remote intents)\n", host, nrintent);
	return 0;
}

int edge_open_late(struct edge_chan *c, uint32_t rcid)
{
	c->rcid = rcid;
	c->remote_open = 1;
	if (!c->local_open)
		open_local(c);
	return 0;
}

/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}

/*
 * SWEETLOG reader (kernel patch 0047): the kernel keeps a 4 MiB console ring
 * and writes it to the LAST 4 MiB + 4 KiB of userdata, block 0 being
 *   "SWEETLOG" | u32 version | u32 size | u64 total | u64 seq.
 * Read-only. The dump below runs before XNU, so it shows the PREVIOUS boot.
 */
#define SWEETLOG_RING_MAX	(4u << 20)
#define SWEETLOG_BLOCKS		(1u + SWEETLOG_RING_MAX / BLOCK)
#define SWEETLOG_HITS		24
#define SWEETLOG_LINE		112

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int has_word(const char *s, const char *w)
{
	for (; *s; s++) {
		const char *a = s, *b = w;
		while (*a && *b && *a == *b) {
			a++;
			b++;
		}
		if (!*b)
			return 1;
	}
	return 0;
}

struct log_hits {
	char line[SWEETLOG_HITS][SWEETLOG_LINE];
	unsigned n, pos;
};

static void log_hit(struct log_hits *h, const char *line, unsigned len)
{
	char *d = h->line[h->pos];
	unsigned k = len < SWEETLOG_LINE - 1 ? len : SWEETLOG_LINE - 1;
	for (unsigned i = 0; i < k; i++)
		d[i] = line[i];
	d[k] = 0;
	h->pos = (h->pos + 1) % SWEETLOG_HITS;
	if (h->n < SWEETLOG_HITS)
		h->n++;
}

/* Find a GPT partition by ASCII name; LBAs are the device's 4 KiB units. */
static int find_partition(const char *want, uint32_t *first, uint32_t *last)
{
	static uint8_t hdr[BLOCK] __attribute__((aligned(4096)));
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));

	if (read_blocks(1, 1, hdr) || __builtin_memcmp(hdr, "EFI PART", 8))
		return 0;
	uint32_t nent = le32(hdr + 80), esz = le32(hdr + 84), elba = le32(hdr + 72);
	if (esz != 128 || nent == 0 || nent > 128)
		return 0;
	if (read_blocks(elba, (nent * 128u + BLOCK - 1) / BLOCK, ents))
		return 0;
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		const char *w = want;
		uint32_t f = (uint32_t)le64(e + 32), l = (uint32_t)le64(e + 40);
		int ok = f != 0;
		unsigned c = 0;
		for (; ok && w[c]; c++) {
			if (e[56 + 2 * c] != (uint8_t)w[c] || e[57 + 2 * c])
				ok = 0;
		}
		if (ok && (e[56 + 2 * c] || e[57 + 2 * c]))
			ok = 0;
		if (ok) {
			*first = f;
			*last = l;
			return 1;
		}
	}
	return 0;
}

struct log_state {
	struct log_hits hits;
	char tail[3][SWEETLOG_LINE];
	unsigned ntail, tpos;
	char cur[SWEETLOG_LINE];
	unsigned curlen;
};

static struct log_state st_log;	/* zero in .bss */

static void log_line(struct log_state *p)
{
	if (!p->curlen)
		return;
	if (has_word(p->cur, "SWEET TOUCH") || has_word(p->cur, "SWEET RPMH")) {
		log_hit(&p->hits, p->cur, p->curlen);
	} else {
		char *d = p->tail[p->tpos];
		unsigned n = p->curlen < SWEETLOG_LINE - 1 ? p->curlen : SWEETLOG_LINE - 1;
		for (unsigned i = 0; i < n; i++)
			d[i] = p->cur[i];
		d[n] = 0;
		p->tpos = (p->tpos + 1) % 3;
		if (p->ntail < 3)
			p->ntail++;
	}
	p->curlen = 0;
}

static void log_feed(struct log_state *p, const uint8_t *b, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		char c = (char)b[i];
		if (c == '\r')
			continue;
		if (c == '\n') {
			log_line(p);
			continue;
		}
		if (p->curlen < sizeof(p->cur))
			p->cur[p->curlen++] = c;
	}
}

/* Stream ring bytes [from, to); the ring itself is contiguous at base+1. */
static int log_read(uint32_t ring_lba, uint64_t from, uint64_t to)
{
	static uint8_t buf[BLOCK] __attribute__((aligned(4096)));

	while (from < to) {
		uint32_t blk = (uint32_t)(from / BLOCK), off = (uint32_t)(from % BLOCK);
		uint32_t n = BLOCK - off;
		if (n > to - from)
			n = (uint32_t)(to - from);
		if (read_blocks(ring_lba + blk, 1, buf))
			return 0;
		log_feed(&st_log, buf + off, n);
		from += n;
	}
	return 1;
}

void
sweet_log_dump(void)
{
	static uint8_t hdr[BLOCK] __attribute__((aligned(4096)));
	uint32_t first = 0, last = 0;
	uint64_t total, seq, oldest, valid;
	uint32_t size;

	con_str("LOG", "sweetlog dump (read-only, previous boot)");
	uint32_t hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("LOG", "UFS host not ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("LOG", "UFS requests outstanding: stop");
		return;
	}
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("LOG", "UFS list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0;
	static const uint8_t tur[6] = { 0x00 };
	for (int i = 0; i < 3; i++) {
		if (!scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st))
			break;
	}

	if (!find_partition("userdata", &first, &last)) {
		con_str("LOG", "userdata partition not found");
		return;
	}
	con_hex2("LOG", "userdata lba=", first, "last=", last);
	if (last + 1 < SWEETLOG_BLOCKS) {
		con_str("LOG", "partition too small");
		return;
	}
	uint32_t base = last + 1 - SWEETLOG_BLOCKS;

	if (read_blocks(base, 1, hdr) || __builtin_memcmp(hdr, "SWEETLOG", 8)) {
		con_str("LOG", "no SWEETLOG header at the partition end");
		return;
	}
	size = le32(hdr + 12);
	total = le64(hdr + 16);
	seq = le64(hdr + 24);
	if (size == 0 || size > SWEETLOG_RING_MAX || (size & (BLOCK - 1)) || total == 0) {
		con_str("LOG", "implausible header");
		return;
	}
	con_hex2("LOG", "total bytes=", (uint32_t)total, "flush seq=", (uint32_t)seq);

	oldest = total > size ? total % size : 0;
	valid = total < size ? total : size;
	if (!log_read(base + 1, oldest, oldest + valid < size ? oldest + valid : size)) {
		con_str("LOG", "read error");
		return;
	}
	if (oldest + valid > size &&
	    !log_read(base + 1, 0, oldest + valid - size)) {
		con_str("LOG", "read error");
		return;
	}
	log_line(&st_log);	/* final unterminated line */

	con_str("LOG", "last SWEET TOUCH / RPMH lines:");
	for (unsigned i = 0; i < st_log.hits.n; i++) {
		unsigned idx = (st_log.hits.pos + SWEETLOG_HITS - st_log.hits.n + i) % SWEETLOG_HITS;
		con_skip();
		con_text(st_log.hits.line[idx]);
	}
	con_skip();
	con_str("TAIL", "last console lines of that boot:");
	for (unsigned i = 0; i < st_log.ntail; i++) {
		unsigned idx = (st_log.tpos + 3 - st_log.ntail + i) % 3;
		con_skip();
		con_text(st_log.tail[idx]);
	}
}

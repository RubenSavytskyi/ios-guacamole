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
#define SWEETLOG_HITS		10
#define SWEETLOG_LINE		240

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
	char seen[6][SWEETLOG_LINE];	/* first GPU/Metal init lines: they scroll away early */
	unsigned nseen;
	char gpu[12][SWEETLOG_LINE];	/* first GPU bring-up lines, even if the ring starts late */
	unsigned ngpu;
	char fb[8][SWEETLOG_LINE];	/* last framebuffer stats: they carry the fps/GPU-busy numbers */
	unsigned nfb, posfb;
	char res[8][SWEETLOG_LINE];	/* last Metal results: device opened / refused, Turnip load */
	unsigned nres, posres;
	char tel[56][SWEETLOG_LINE];	/* last Metal telemetry rows ("tel ...", one row per line) */
	unsigned ntel, postel;
	char iok[8][SWEETLOG_LINE];	/* last "IOK ..." registry trace lines: what a daemon looks for before it stops */
	unsigned niok, posiok;
	/*
	 * The Metal plugin's own lines for backboardd ("MTL2[58 backboardd]: ..."). The generic ring above is flooded by
	 * the kernel's per-selector trace ("SWEET FB: backboardd sel 5 ..."), which hid whether the window server ever got
	 * a Metal device - the difference between GPU-composited CoreAnimation and its software fallback.
	 */
	char bbd[12][SWEETLOG_LINE];
	unsigned nbbd, posbbd;
	/* the newest "native-metal:" lines: per-frame bridge activity, whoever's drawing */
	char nm[8][SWEETLOG_LINE];
	unsigned nnm, posnm;
	/* who renders: "native-metal: work commit" lines counted per process name */
	char who[8][24];
	unsigned whon[8], nwho;
	unsigned refused, opened, noload, black;
};

static void log_copy(char *d, const char *line, unsigned len)
{
	unsigned k = len < SWEETLOG_LINE - 1 ? len : SWEETLOG_LINE - 1;
	for (unsigned i = 0; i < k; i++)
		d[i] = line[i];
	d[k] = 0;
}

/* The body of an "MTL2[<pid> <name>]: tel <row>" line: the row alone, it fits one screen row. */
static const char *log_tel_body(const char *s)
{
	for (; *s; s++)
		if (s[0] == 't' && s[1] == 'e' && s[2] == 'l' && s[3] == ' ')
			return s + 4;
	return s;
}

static char *log_dec(char *p, unsigned v)
{
	char tmp[10];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		*p++ = tmp[--n];
	return p;
}

static int log_gpu_line(const char *s)
{
	return has_word(s, "SWEET GPU") || has_word(s, "RedwoodGPU") ||
	       has_word(s, "Adreno") || has_word(s, "sweetgpu") ||
	       has_word(s, "SweetAccelerator");
}

static int log_rare(const char *s)
{
	return has_word(s, "IOAccelerator") || log_gpu_line(s) ||
	       has_word(s, "SweetMetal") || has_word(s, "MTL2") ||
	       has_word(s, "Metal plugin") || has_word(s, "sweetmtl");
}

static int log_metal_result(const char *s)
{
	return has_word(s, "MTL2") && (has_word(s, "refused") || has_word(s, "ready") ||
	       has_word(s, "waiting") || has_word(s, "images") || has_word(s, "cache") ||
	       has_word(s, "libvulkan"));
}

/* 0259: count a "work commit" line against the process named in its "MTL2[<pid> <name>]" prefix */
static void log_note_committer(struct log_hits *h, const char *s)
{
	const char *p = s;
	while (*p && !(p[0] == 'M' && p[1] == 'T' && p[2] == 'L' && p[3] == '2'))
		p++;
	if (!*p)
		return;
	p += 4;
	if (*p == '[')
		p++;
	while (*p >= '0' && *p <= '9')
		p++;
	if (*p == ' ')
		p++;
	char name[24];
	unsigned n = 0;
	while (*p && *p != ']' && *p != ':' && n < sizeof(name) - 1)
		name[n++] = *p++;
	name[n] = 0;
	if (n == 0)
		return;
	for (unsigned i = 0; i < h->nwho; i++) {
		const char *a = h->who[i];
		const char *b = name;
		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (!*a && !*b) {
			h->whon[i]++;
			return;
		}
	}
	if (h->nwho < 8) {
		for (unsigned i = 0; i <= n; i++)
			h->who[h->nwho][i] = name[i];
		h->whon[h->nwho] = 1;
		h->nwho++;
	}
}

static void log_hit(struct log_hits *h, const char *line, unsigned len)
{
	if (log_gpu_line(line) && h->ngpu < 12)
		log_copy(h->gpu[h->ngpu++], line, len);
	if (log_rare(line) && h->nseen < 6)
		log_copy(h->seen[h->nseen++], line, len);
	if (has_word(line, "SWEET FB") && has_word(line, "5s:")) {
		log_copy(h->fb[h->posfb], line, len);
		h->posfb = (h->posfb + 1) % 8;
		if (h->nfb < 8)
			h->nfb++;
	}
	if (log_metal_result(line)) {
		log_copy(h->res[h->posres], line, len);
		h->posres = (h->posres + 1) % 8;
		if (h->nres < 8)
			h->nres++;
		if (has_word(line, "refused"))
			h->refused++;
		else if (has_word(line, "cache"))
			h->opened++;
		if (has_word(line, "libvulkan"))
			h->noload++;
	}
	if (has_word(line, "black") && has_word(line, "display"))
		h->black++;
	/* the Metal bridge's telemetry rows: "MTL2[<pid> backboardd]: tel <row>" */
	if (has_word(line, "tel")) {
		log_copy(h->tel[h->postel], line, len);
		h->postel = (h->postel + 1) % 56;
		if (h->ntel < 56)
			h->ntel++;
	}
	/* every line backboardd wrote, kernel trace or plugin: its last one shows where it stops */
	if (has_word(line, "backboardd") && !has_word(line, "SWEET FB")) {
		log_copy(h->iok[h->posiok], line, len);
		h->posiok = (h->posiok + 1) % 8;
		if (h->niok < 8)
			h->niok++;
	}
	/* 0259: the Metal plugin's own backboardd lines, kept apart from the kernel's per-selector flood above */
	if (has_word(line, "MTL2") && has_word(line, "backboardd")) {
		log_copy(h->bbd[h->posbbd], line, len);
		h->posbbd = (h->posbbd + 1) % 12;
		if (h->nbbd < 12)
			h->nbbd++;
	}
	if (has_word(line, "native-metal")) {
		log_copy(h->nm[h->posnm], line, len);
		h->posnm = (h->posnm + 1) % 8;
		if (h->nnm < 8)
			h->nnm++;
	}
	if (has_word(line, "work commit")) {
		log_note_committer(h, line);
	}
	log_copy(h->line[h->pos], line, len);
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
	/* has_word() scans until a NUL: without this it read stale bytes past the line. */
	p->cur[p->curlen < sizeof(p->cur) - 1 ? p->curlen : sizeof(p->cur) - 1] = 0;
	/*
	 * Keep the lines that matter for the graphics bring-up as well as touch:
	 * the framebuffer driver's 5 s stats, the GPU stages and the Metal plugin.
	 */
	if (has_word(p->cur, "SWEET TOUCH") || has_word(p->cur, "SWEET RPMH") ||
	    has_word(p->cur, "SWEET FB") || has_word(p->cur, "SWEET GPU") ||
	    log_rare(p->cur)) {
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

	con_str("LOG", "backboardd Metal plugin lines (newest 5):");
	{
		unsigned show = st_log.hits.nbbd < 5 ? st_log.hits.nbbd : 5;
		for (unsigned i = 0; i < show; i++) {
			unsigned idx = (st_log.hits.posbbd + 12 - show + i) % 12;
			con_skip();
			con_text(st_log.hits.bbd[idx]);
		}
	}
	con_skip();
	con_str("LOG", "GPU users (native-metal work commits):");
	{
		unsigned show = st_log.hits.nwho < 5 ? st_log.hits.nwho : 5;
		for (unsigned i = 0; i < show; i++) {
			char text[48], *p = text;
			const char *a = st_log.hits.who[i];
			while (*a)
				*p++ = *a++;
			*p++ = ' ';
			p = log_dec(p, st_log.hits.whon[i]);
			*p = 0;
			con_skip();
			con_text(text);
		}
	}
	con_skip();
	{
		char text[64], *p = text;
		const char *a = "refused ";
		while ((*p = *a++))
			p++;
		p = log_dec(p, st_log.hits.refused);
		a = ", opened ";
		while ((*p = *a++))
			p++;
		p = log_dec(p, st_log.hits.opened);
		a = ", turnip-load-fail ";
		while ((*p = *a++))
			p++;
		p = log_dec(p, st_log.hits.noload);
		a = ", black ";
		while ((*p = *a++))
			p++;
		p = log_dec(p, st_log.hits.black);
		*p = 0;
		con_str("LOG", text);
	}
	con_str("LOG", "Metal results (newest 2):");
	{
		unsigned show = st_log.hits.nres < 2 ? st_log.hits.nres : 2;
		for (unsigned i = 0; i < show; i++) {
			unsigned idx = (st_log.hits.posres + 8 - show + i) % 8;
			con_skip();
			con_text(st_log.hits.res[idx]);
		}
	}
	con_skip();
	con_str("LOG", "framebuffer stats (newest 3):");
	{
		unsigned show = st_log.hits.nfb < 3 ? st_log.hits.nfb : 3;
		for (unsigned i = 0; i < show; i++) {
			unsigned idx = (st_log.hits.posfb + 8 - show + i) % 8;
			con_skip();
			con_text(st_log.hits.fb[idx]);
		}
	}
	con_skip();
	con_str("LOG", "Metal telemetry (newest rows):");
	{
		unsigned tshow = st_log.hits.ntel < 5 ? st_log.hits.ntel : 5;
		for (unsigned i = 0; i < tshow; i++) {
			unsigned idx = (st_log.hits.postel + 56 - tshow + i) % 56;
			con_skip();
			con_text(log_tel_body(st_log.hits.tel[idx]));
		}
	}
	con_skip();
	con_str("LOG", "newest native-metal lines:");
	{
		unsigned show = st_log.hits.nnm < 2 ? st_log.hits.nnm : 2;
		for (unsigned i = 0; i < show; i++) {
			unsigned idx = (st_log.hits.posnm + 8 - show + i) % 8;
			con_skip();
			con_text(st_log.hits.nm[idx]);
		}
	}
	con_skip();
	con_str("TAIL", "last console lines of that boot:");
	for (unsigned i = 0; i < st_log.ntail; i++) {
		unsigned idx = (st_log.tpos + 3 - st_log.ntail + i) % 3;
		con_skip();
		con_text(st_log.tail[idx]);
	}
}

/*
 * tfa: the speaker amps of guacamole, two NXP TFA9874 (0x34 right, 0x35 left;
 * Android tfa98xx v6.5.3) on QUP wrapper 0 SE4 (qupv3_se4_i2c, i2c@890000,
 * SDA/SCL GPIO51/52 function qup4), from userland over /dev/pmem. The GENI
 * I2C FIFO engine is the kernel touch driver's (bsd/dev/sweet_touch.c, after
 * Linux i2c-qcom-geni.c); clocks per Linux gcc-sm8150.c (wrapper 0: vote
 * 0x5200c bits 6 M_AHB, 7 S_AHB, 14 S4; S4 RCG 0x17608). TFA registers are
 * 16-bit big endian behind an 8-bit address (Linux tfa989x.c regmap).
 *
 *   tfa probe               clocks, pins, SE setup; REV (reg 0x03) of both amps
 *   tfa rd ADDR REG [N]     N registers from REG
 *   tfa wr ADDR REG VAL     one register
 *   tfa status              power state and status of both amps
 *   tfa init [PROFILE]      reset, settings for the revision, container items (profile 1 = speaker), out of powerdown
 *   tfa off                 both amps to powerdown
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../wland/pmem.h"

#define SE		0x00890000ull	/* QUP wrapper 0, SE4 */
#define GCC		0x00100000ull
#define GCC_VOTE	(GCC + 0x5200c)
#define GCC_VOTE_MASK	((1u << 6) | (1u << 7) | (1u << 14))
#define GCC_RCG		(GCC + 0x17608)
#define TLMM_NORTH	0x03900000ull
#define TLMM_SOUTH	0x03d00000ull
#define AMP_RIGHT	0x34
#define AMP_LEFT	0x35

#define SE_STATUS	0x040u
#define SE_MCLK		0x048u
#define SE_FW		0x068u
#define SE_CLK_SEL	0x07cu
#define SE_DMA		0x258u
#define SE_TX_LEN	0x26cu
#define SE_RX_LEN	0x270u
#define SE_SCL		0x278u
#define SE_CMD		0x600u
#define SE_CTRL		0x604u
#define SE_IRQ		0x610u
#define SE_IRQ_EN	0x614u
#define SE_IRQ_CLR	0x618u
#define SE_TX		0x700u
#define SE_RX		0x780u
#define SE_TX_COUNT	0x800u
#define SE_RX_COUNT	0x804u
#define SE_TX_WM	0x80cu
#define SE_HW_TX	0xe24u
#define SE_HW_RX	0xe28u
#define SE_RX_WM_IRQ	(1u << 26)
#define SE_RX_LAST	(1u << 27)
#define SE_TX_WM_IRQ	(1u << 30)
#define SE_DONE		(1u << 0)
#define SE_NACK		(1u << 10)
#define SE_ERRORS	(0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS	(SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
#define SE_PACK0	0x0007f8feu
#define SE_PACK1	0x000ffefeu

static uint32_t se_rd(uint32_t off) { return rd32(SE + off); }
static void se_wr(uint32_t off, uint32_t v) { wr32(SE + off, v); }

static int wait_clear(uint64_t pa, uint32_t mask)
{
	for (int i = 0; i < 5000; i++) {
		if (!(rd32(pa) & mask))
			return 0;
		usleep(1);
	}
	return -1;
}

static void gpio_mux(uint64_t tile, unsigned pin, unsigned mux, unsigned pull, unsigned drive)
{
	uint64_t cfg = tile + 0x1000ull * pin;
	wr32(cfg, (rd32(cfg) & ~0x3ffu) | (mux << 2) | pull | (drive << 6));
}

static int clocks(void)
{
	wr32(GCC_VOTE, rd32(GCC_VOTE) | GCC_VOTE_MASK);
	uint32_t cfg = rd32(GCC_RCG + 4), dfs = rd32(GCC_RCG + 0x14);
	printf("gcc: vote %#x rcg %#x cfg %#x dfs %#x\n", rd32(GCC_VOTE), rd32(GCC_RCG), cfg, dfs);
	unsigned clk_sel = 0;
	if (dfs & 1) {
		int level = -1;
		for (unsigned l = 0; l < 8; l++) {
			uint32_t lc = rd32(GCC_RCG + 0x1c + 4 * l);
			if (level < 0 && (lc & 0x371fu) == 1)
				level = (int)l;
		}
		if (level < 0) {
			printf("gcc: no XO/1 DFS level\n");
			return -1;
		}
		clk_sel = (unsigned)level;
		printf("gcc: DFS level %u (XO)\n", clk_sel);
	} else {
		wr32(GCC_RCG + 4, (cfg & ~(0x371fu | (1u << 20))) | 1u);
		wr32(GCC_RCG, rd32(GCC_RCG) | 1u);
		if (wait_clear(GCC_RCG, 1)) {
			printf("gcc: RCG update timeout\n");
			return -1;
		}
	}
	const uint32_t branches[] = { 0x17004, 0x17604 };	/* M_AHB, S4 (S_AHB is HW gated) */
	for (unsigned i = 0; i < 2; i++)
		if (wait_clear(GCC + branches[i], 1u << 31)) {
			printf("gcc: branch %#x halted (%#x)\n", branches[i], rd32(GCC + branches[i]));
			return -1;
		}
	se_wr(SE_CLK_SEL, clk_sel);
	return 0;
}

static int se_init(void)
{
	uint32_t fw = se_rd(SE_FW), tx = se_rd(SE_HW_TX), rx = se_rd(SE_HW_RX);
	printf("se4: fw %#x proto %u status %#x tx %#x rx %#x\n", fw, (fw >> 8) & 0xff, se_rd(SE_STATUS), tx, rx);
	if (((fw >> 8) & 0xff) != 3) {
		printf("se4: not I2C firmware\n");
		return -1;
	}
	if (se_rd(SE_STATUS) & 0x1001u) {
		printf("se4: busy\n");
		return -1;
	}
	se_wr(0xe18, 0);
	se_wr(0xe1c, 0);
	se_wr(0x644, 0);
	se_wr(SE_IRQ_CLR, 0xffffffffu);
	se_wr(0x648, 0xffffffffu);
	se_wr(SE_DMA, 0);
	se_wr(0x28, se_rd(0x28) | 0x7f);
	se_wr(0xe30, se_rd(0xe30) | 0x0f);
	se_wr(0x24, 0x7f);
	se_wr(0x20, 1);
	se_wr(SE_MCLK, (2u << 4) | 1u);		/* 400 kHz from 19.2 MHz (Linux geni_i2c_clk_map) */
	se_wr(SE_SCL, (5u << 20) | (12u << 10) | 24u);
	se_wr(0x254, 0);
	se_wr(0x260, SE_PACK0);
	se_wr(0x264, SE_PACK1);
	se_wr(0x284, SE_PACK0);
	se_wr(0x288, SE_PACK1);
	se_wr(SE_TX_WM, 0);
	se_wr(0x810, 0);
	se_wr(0x814, ((rx >> 16) & 0x3f) - 1);
	se_wr(SE_IRQ_EN, SE_EVENTS);
	return 0;
}

static void se_abort(void)
{
	se_wr(SE_CTRL, 2);
	for (int i = 0; i < 5000 && !(se_rd(SE_IRQ) & (1u << 5)); i++)
		usleep(1);
	for (int i = 0; i < 64 && (se_rd(SE_RX_COUNT) & 0x1ffffff); i++)
		(void)se_rd(SE_RX);
	se_wr(SE_IRQ_CLR, 0xffffffffu);
}

/* One FIFO message: write nt (<= 3) bytes or read nr (<= 64) bytes; stretch = no STOP (repeated start next) */
static int msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, int stretch)
{
	if (nt > 3 || nr > 64 || (nt && nr) || (!nt && !nr))
		return EINVAL;
	if (wait_clear(SE + SE_STATUS, 1))
		return EBUSY;
	unsigned sent = 0, got = 0;
	se_wr(SE_IRQ_CLR, 0xffffffffu);
	se_wr(SE_TX_WM, nt ? 1 : 0);
	se_wr(nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	se_wr(SE_CMD, ((nr ? 2u : 1u) << 27) | ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	for (int i = 0; i < 20000; i++) {
		uint32_t irq = se_rd(SE_IRQ);
		if (irq & SE_ERRORS) {
			int err = (irq & SE_NACK) ? ENXIO : EIO;
			for (int j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				usleep(1);
				irq = se_rd(SE_IRQ);
			}
			if ((irq & SE_DONE) && !(se_rd(SE_STATUS) & 1)) {
				se_wr(SE_IRQ_CLR, irq);
				return err;
			}
			se_abort();
			return err;
		}
		if (nt && !sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t w = 0;
			for (unsigned j = 0; j < nt; j++)
				w |= (uint32_t)tx[j] << (8 * j);
			se_wr(SE_TX, w);
			sent = nt;
			se_wr(SE_TX_WM, 0);
		}
		uint32_t count = se_rd(SE_RX_COUNT) & 0x1ffffff;
		while (count--) {
			uint32_t w = se_rd(SE_RX);
			for (unsigned j = 0; j < 4 && got < nr; j++)
				rx[got++] = (uint8_t)(w >> (8 * j));
		}
		if (irq & SE_DONE) {
			se_wr(SE_IRQ_CLR, irq);
			return (got == nr && sent == nt) ? 0 : EIO;
		}
		se_wr(SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		usleep(10);
	}
	se_abort();
	return ETIMEDOUT;
}

static int reg_rd(uint8_t addr, uint8_t reg, uint16_t *v)
{
	uint8_t b[2];
	int e = msg(addr, &reg, 1, NULL, 0, 1);
	if (!e)
		e = msg(addr, NULL, 0, b, 2, 0);
	if (!e)
		*v = (uint16_t)(b[0] << 8 | b[1]);
	return e;
}

static int reg_wr(uint8_t addr, uint8_t reg, uint16_t v)
{
	uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
	return msg(addr, b, 3, NULL, 0, 0);
}

/* bitfield: register << 8 | pos << 4 | (length - 1), as the NXP field names (tfa9874_tfafieldnames.h) */
static int bf_wr(uint8_t addr, uint16_t field, uint16_t value)
{
	uint8_t reg = (uint8_t)(field >> 8);
	unsigned pos = (field >> 4) & 15, len = (field & 15) + 1;
	uint16_t mask = (uint16_t)(((1u << len) - 1) << pos), v;
	int e = reg_rd(addr, reg, &v);
	if (e)
		return e;
	return reg_wr(addr, reg, (uint16_t)((v & ~mask) | ((value << pos) & mask)));
}

static int bf_rd(uint8_t addr, uint16_t field, uint16_t *value)
{
	unsigned pos = (field >> 4) & 15, len = (field & 15) + 1;
	uint16_t v;
	int e = reg_rd(addr, (uint8_t)(field >> 8), &v);
	if (!e)
		*value = (uint16_t)((v >> pos) & ((1u << len) - 1));
	return e;
}

/* TFA9874 fields (tfa9874_tfafieldnames.h) */
#define BF_PWDN		0x0000
#define BF_I2CR		0x0010
#define BF_AMPE		0x0030
#define BF_DCA		0x0040
#define BF_MANSCONF	0x0120
#define BF_AUDFS	0x0203
#define BF_FRACTDEL	0x0255
#define BF_NOCLK	0x1070
#define BF_PLLS		0x1160
#define BF_CLKS		0x1170
#define BF_AMPS		0x11e0
#define BF_MANSTATE	0x1433

/*
 * The container (vendor/firmware/tfa98xx.cnt, NXP "PM3" format: tfa98xx_parameters.h): device lists
 * (here P = 0x34, Q = 0x35) of register patches and bitfields, then profiles ("receiver", "speaker",
 * "calibrate.cal"). Descriptor pointers are { offset:24, type:8 } from the start of the file.
 */
enum { DSC_DEVICE, DSC_PROFILE, DSC_REGISTER, DSC_STRING, DSC_FILE, DSC_PATCH, DSC_MARKER, DSC_BITFIELD = 16, DSC_DEFAULT };
static uint8_t *cnt;
static size_t cnt_len;

static uint32_t cnt_u32(size_t off)
{
	uint32_t v = 0;
	if (off + 4 <= cnt_len)
		memcpy(&v, cnt + off, 4);
	return v;
}

static int cnt_item(uint8_t addr, uint32_t dsc)
{
	size_t off = dsc & 0xffffff;
	if (off + 5 > cnt_len)
		return EINVAL;
	if ((dsc >> 24) == DSC_REGISTER) {
		uint8_t reg = cnt[off];
		uint16_t val = (uint16_t)(cnt[off + 1] | cnt[off + 2] << 8), mask = (uint16_t)(cnt[off + 3] | cnt[off + 4] << 8), v = 0;
		int e = mask == 0xffff ? 0 : reg_rd(addr, reg, &v);
		return e ? e : reg_wr(addr, reg, (uint16_t)((v & ~mask) | (val & mask)));
	}
	if ((dsc >> 24) == DSC_BITFIELD) {
		uint16_t val = (uint16_t)(cnt[off] | cnt[off + 1] << 8), field = (uint16_t)(cnt[off + 2] | cnt[off + 3] << 8);
		return bf_wr(addr, field, val);
	}
	return 0;
}

/* Linux tfaContWriteRegsDev + tfaContWriteRegsProf for device dev, profile prof */
static int cnt_write(uint8_t addr, unsigned dev, unsigned prof, unsigned *nwritten)
{
	size_t dl = cnt_u32(0x2e + 4 * dev) & 0xffffff;
	if (dl + 12 > cnt_len || cnt[dl + 2] != addr) {
		printf("cnt: device %u is not %#x\n", dev, addr);
		return EINVAL;
	}
	unsigned n = cnt[dl], profiles = 0, w = 0;
	uint32_t prof_dsc = 0;
	int stop = 0, e;
	for (unsigned i = 0; i < n; i++) {
		uint32_t d = cnt_u32(dl + 12 + 4 * i), t = d >> 24;
		if (t == DSC_PROFILE && profiles++ == prof)
			prof_dsc = d;
		if (t == DSC_PATCH || t == DSC_FILE || t == DSC_PROFILE)
			stop = 1;
		if (!stop && (t == DSC_REGISTER || t == DSC_BITFIELD)) {
			if ((e = cnt_item(addr, d)))
				return e;
			w++;
		}
	}
	if (!prof_dsc) {
		printf("cnt: no profile %u\n", prof);
		return EINVAL;
	}
	size_t pl = prof_dsc & 0xffffff;
	unsigned pn = cnt[pl];
	for (unsigned i = 0; i + 1 < pn; i++) {
		uint32_t d = cnt_u32(pl + 8 + 4 * i);
		if ((d >> 24) == DSC_DEFAULT)
			break;
		if ((e = cnt_item(addr, d)))
			return e;
		w++;
	}
	*nwritten = w;
	return 0;
}

static void status(uint8_t addr)
{
	uint16_t r0 = 0, r10 = 0, r11 = 0, r13 = 0, r14 = 0, ms = 0;
	reg_rd(addr, 0x00, &r0);
	reg_rd(addr, 0x10, &r10);
	reg_rd(addr, 0x11, &r11);
	reg_rd(addr, 0x13, &r13);
	reg_rd(addr, 0x14, &r14);
	bf_rd(addr, BF_MANSTATE, &ms);
	printf("amp %#x: SYS_CTRL0 %#06x STATUS %#06x/%#06x/%#06x/%#06x MANSTATE %u%s%s%s%s\n", addr, r0, r10, r11, r13, r14, ms,
	       (r11 >> 6) & 1 ? " PLL-locked" : "", (r11 >> 7) & 1 ? " clocks-stable" : "", (r11 >> 14) & 1 ? " amp-on" : "",
	       (r0 & 1) ? " (powered down)" : "");
}

/* Linux tfa_dev_start for a TFA9874 (no CoolFlux): tfa98xx_init, tfa9874_specific (rev 0x0c74), the container's
 * device and profile items, then initCF/operating (MANSCONF, PWDN 0; SBSL/ACS do not exist on this part). */
static int amp_init(uint8_t addr, unsigned dev, unsigned prof)
{
	uint16_t rev = 0, audfs = 0, fractdel = 0, v = 0;
	int e = reg_rd(addr, 0x03, &rev);
	if (e)
		return e;
	bf_rd(addr, BF_AUDFS, &audfs);
	bf_rd(addr, BF_FRACTDEL, &fractdel);
	reg_wr(addr, 0x00, 1u << 1);			/* I2CR: all registers to their defaults */
	usleep(5000);
	/* tfa9874_specific: unlock key 1 and key 2, then the generated settings for this revision */
	reg_wr(addr, 0x0f, 0x5a6b);
	reg_rd(addr, 0xfb, &v);
	reg_wr(addr, 0xa0, v ^ 0x005a);
	reg_wr(addr, 0x0f, 0x5a6b);
	reg_wr(addr, 0xa1, 0x005a);
	reg_wr(addr, 0x0f, 0);
	if (rev == 0x0c74) {
		static const uint16_t v114[][2] = {
			{ 0x02, 0x22c8 }, { 0x52, 0x57dc }, { 0x53, 0x003e }, { 0x56, 0x0400 }, { 0x61, 0x0110 },
			{ 0x6f, 0x00a5 }, { 0x70, 0x07f8 }, { 0x73, 0x0047 }, { 0x74, 0x5098 }, { 0x75, 0x8d28 },
			{ 0x80, 0x0000 }, { 0x83, 0x0799 }, { 0x84, 0x0081 },
		};
		for (unsigned i = 0; i < sizeof(v114) / sizeof(v114[0]); i++)
			if ((e = reg_wr(addr, (uint8_t)v114[i][0], v114[i][1])))
				return e;
	} else {
		printf("amp %#x: revision %#x has no settings here\n", addr, rev);
	}
	bf_wr(addr, BF_AUDFS, audfs);
	bf_wr(addr, BF_FRACTDEL, fractdel);
	unsigned n = 0;
	if ((e = cnt_write(addr, dev, prof, &n)))
		return e;
	printf("amp %#x: rev %#x, %u container items written\n", addr, rev, n);
	/* initCF then operating */
	bf_wr(addr, BF_MANSCONF, 1);
	bf_wr(addr, BF_PWDN, 0);
	uint16_t clks = 0;
	for (int i = 0; i < 50 && !clks; i++) {
		bf_rd(addr, BF_CLKS, &clks);
		if (!clks)
			usleep(1000);
	}
	if (!clks)
		printf("amp %#x: no stable clocks (no I2S bit clock yet)\n", addr);
	return 0;
}

static int load_cnt(void)
{
	FILE *f = fopen("/var/local/fw/tfa98xx.cnt", "rb");
	if (!f) {
		printf("no /var/local/fw/tfa98xx.cnt\n");
		return -1;
	}
	cnt = malloc(65536);
	cnt_len = fread(cnt, 1, 65536, f);
	fclose(f);
	if (cnt_len < 0x40 || memcmp(cnt, "PM3", 3)) {
		printf("tfa98xx.cnt: not a PM3 container\n");
		return -1;
	}
	return 0;
}

static int setup(void)
{
	pm_init();
	/* SDA/SCL: function qup4 (mux 1), no pull, 2 mA as the DT's qupv3_se4_i2c_pins */
	gpio_mux(TLMM_SOUTH, 51, 1, 0, 0);
	gpio_mux(TLMM_SOUTH, 52, 1, 0, 0);
	printf("gpio51 cfg %#x gpio52 cfg %#x; reset gpio37 cfg %#x io %#x, gpio100 cfg %#x io %#x\n",
	       rd32(TLMM_SOUTH + 51 * 0x1000), rd32(TLMM_SOUTH + 52 * 0x1000), rd32(TLMM_NORTH + 37 * 0x1000),
	       rd32(TLMM_NORTH + 37 * 0x1000 + 4), rd32(TLMM_SOUTH + 100 * 0x1000), rd32(TLMM_SOUTH + 100 * 0x1000 + 4));
	if (clocks() || se_init())
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: tfa probe | status | init [PROFILE] | off | rd ADDR REG [N] | wr ADDR REG VAL\n");
		return 2;
	}
	if (setup())
		return 1;
	if (!strcmp(argv[1], "probe")) {
		const uint8_t amps[] = { AMP_RIGHT, AMP_LEFT };
		for (int i = 0; i < 2; i++) {
			uint16_t rev = 0, sys = 0;
			int e = reg_rd(amps[i], 0x03, &rev);
			if (!e)
				e = reg_rd(amps[i], 0x00, &sys);
			if (e)
				printf("amp %#x: error %d (%s)\n", amps[i], e, strerror(e));
			else
				printf("amp %#x: REV %#06x (TFA9874 is 0x0a74) SYS_CTRL0 %#06x\n", amps[i], rev, sys);
		}
		return 0;
	}
	if (!strcmp(argv[1], "status")) {
		status(AMP_RIGHT);
		status(AMP_LEFT);
		return 0;
	}
	if (!strcmp(argv[1], "init")) {
		/* profile 1 = "speaker" (0 receiver, 2 calibrate.cal) */
		unsigned prof = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
		if (load_cnt())
			return 1;
		int e1 = amp_init(AMP_RIGHT, 0, prof), e2 = amp_init(AMP_LEFT, 1, prof);
		if (e1 || e2)
			printf("init errors: right %d left %d\n", e1, e2);
		status(AMP_RIGHT);
		status(AMP_LEFT);
		return e1 || e2;
	}
	if (!strcmp(argv[1], "off")) {
		bf_wr(AMP_RIGHT, BF_PWDN, 1);
		bf_wr(AMP_LEFT, BF_PWDN, 1);
		status(AMP_RIGHT);
		status(AMP_LEFT);
		return 0;
	}
	if (!strcmp(argv[1], "rd") && argc >= 4) {
		uint8_t addr = (uint8_t)strtoul(argv[2], NULL, 0), reg = (uint8_t)strtoul(argv[3], NULL, 0);
		int n = argc > 4 ? atoi(argv[4]) : 1;
		for (int i = 0; i < n; i++) {
			uint16_t v;
			int e = reg_rd(addr, (uint8_t)(reg + i), &v);
			if (e) {
				printf("%#04x: error %d\n", reg + i, e);
				return 1;
			}
			printf("%#04x: %#06x\n", reg + i, v);
		}
		return 0;
	}
	if (!strcmp(argv[1], "wr") && argc >= 5) {
		uint8_t addr = (uint8_t)strtoul(argv[2], NULL, 0), reg = (uint8_t)strtoul(argv[3], NULL, 0);
		uint16_t v = (uint16_t)strtoul(argv[4], NULL, 0);
		int e = reg_wr(addr, reg, v);
		printf("wr %#x %#04x %#06x -> %d\n", addr, reg, v, e);
		return e ? 1 : 0;
	}
	fprintf(stderr, "bad command\n");
	return 2;
}

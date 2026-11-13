/*
 * btd: Bluetooth of guacamole (OnePlus 7 Pro, SM8150) from userland over /dev/pmem. The controller is the
 * WCN3990's BT core (live DT bt_wcn3990, "qca,wcn3990"), on the HS UART qupv3_se13 (QUP wrapper 2 SE3,
 * 0xc8c000; CTS/RTS/TX/RX = GPIO43/44/45/46, function qup13, EAST tile). Its rails (pm8150 l7, pm8150l l2,
 * pm8150l l11) and reference clock are the WLAN's: wland turns them on.
 *
 * UART: the GENI engine in FIFO mode, polled (Linux drivers/tty/serial/qcom_geni_serial.c and
 * drivers/soc/qcom/qcom-geni-se.c); QUP 1.3, so 32x oversampling. The SE's clock source is not in DFS mode
 * here, so the RCG (GCC 0x1e4d8) gets an M/N rate as in Linux gcc-sm8150.c's QUP table, but from GPLL0 main
 * (600 MHz; GPLL0 USER_CTL 0x103: main and even both on). 7.3728 MHz
 * (M/N 384/31250) for 2400 and 115200 baud, 102.4 MHz (64/375) for 3.2 Mbaud.
 * Controller bring-up as Linux drivers/bluetooth/hci_qca.c and btqca.c: power-off pulse 0xC0 at 2400 baud,
 * power-on pulse 0xFC at 115200, both without flow control; then HCI (H4) with RTS/CTS, at 3.2 Mbaud once the
 * version answers (oper_speed).
 *
 *   btd version             power the BT core on, read the SoC version (EDL_PATCH_VER_REQ)
 *   btd up                  and the rampatch + NVM (/var/local/fw/bt: linux-firmware qca/crbtfw21.tlv, crnv21.bin),
 *                           HCI reset, version and address
 *   btd scan [SECS]         and a classic inquiry plus an LE scan
 *   btd serve [NAME]        and hand the controller to bluetoothd: H4 both ways through the kernel control
 *                           NAME (default com.sweet.bt, xnu 0162, boot-arg sweetbt=1); bluetoothd's POSIX driver
 *                           connects to it with BT_HCI_TRANSPORT=H4:POSIX:NAME (its transport table: USB H4DS
 *                           H4BC H4 H5 APPLEBT VIRT PCIE PCIESW, drivers USB POSIX APPLEBT VIRT PCIE PCIESW)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../wland/pmem.h"

#define SE		0x00c8c000ull	/* QUP wrapper 2, SE3 (qupv3_se13 HS UART) */
#define GCC		0x00100000ull
#define GCC_VOTE	(GCC + 0x52014)	/* wrapper 2: bits 0-4 (touch), bit 7 = S3 */
#define GCC_S3_BIT	(1u << 7)
#define GCC_S3_CBCR	(GCC + 0x1e4d4)
#define RCG		(GCC + 0x1e4d8)
#define TLMM_EAST	0x03500000ull

/* GENI and UART registers (qcom-geni-se.h, qcom_geni_serial.c) */
#define GENI_FORCE_DEFAULT	0x020
#define GENI_OUTPUT_CTRL	0x024
#define GENI_CGC_CTRL		0x028
#define SE_GENI_STATUS		0x040
#define GENI_SER_M_CLK_CFG	0x048
#define GENI_SER_S_CLK_CFG	0x04c
#define GENI_FW_REVISION	0x068
#define SE_GENI_CLK_SEL		0x07c
#define SE_UART_LOOPBACK	0x22c
#define SE_GENI_BYTE_GRAN	0x254
#define SE_GENI_DMA_MODE_EN	0x258
#define SE_UART_TX_TRANS_CFG	0x25c
#define SE_GENI_TX_PACKING0	0x260
#define SE_GENI_TX_PACKING1	0x264
#define SE_UART_TX_WORD_LEN	0x268
#define SE_UART_TX_STOP_BIT	0x26c
#define SE_UART_TX_TRANS_LEN	0x270
#define SE_UART_RX_TRANS_CFG	0x280
#define SE_GENI_RX_PACKING0	0x284
#define SE_GENI_RX_PACKING1	0x288
#define SE_UART_RX_WORD_LEN	0x28c
#define SE_UART_RX_STALE_CNT	0x294
#define SE_UART_TX_PARITY	0x2a4
#define SE_UART_RX_PARITY	0x2a8
#define SE_UART_MANUAL_RFR	0x2ac
#define SE_GENI_M_CMD0		0x600
#define SE_GENI_M_IRQ_STATUS	0x610
#define SE_GENI_M_IRQ_EN	0x614
#define SE_GENI_M_IRQ_CLEAR	0x618
#define SE_GENI_S_CMD0		0x630
#define SE_GENI_S_CMD_CTRL	0x634
#define SE_GENI_S_IRQ_STATUS	0x640
#define SE_GENI_S_IRQ_EN	0x644
#define SE_GENI_S_IRQ_CLEAR	0x648
#define SE_GENI_TX_FIFO		0x700
#define SE_GENI_RX_FIFO		0x780
#define SE_GENI_TX_FIFO_STATUS	0x800
#define SE_GENI_RX_FIFO_STATUS	0x804
#define SE_GENI_TX_WATERMARK	0x80c
#define SE_GENI_RX_WATERMARK	0x810
#define SE_GENI_RX_RFR_WM	0x814
#define SE_GSI_EVENT_EN		0xe18
#define SE_IRQ_EN		0xe1c
#define SE_DMA_GENERAL_CFG	0xe30
#define SE_HW_PARAM_0		0xe24
#define SE_HW_PARAM_1		0xe28

#define M_CMD_DONE		(1u << 0)
#define S_CMD_DONE		(1u << 0)
#define S_CMD_CANCEL		(1u << 2)
#define S_CMD_ABORT		(1u << 1)
#define UART_START_TX		1u
#define UART_START_READ		1u
#define UART_CTS_MASK		(1u << 1)
#define CMD_SHIFT		27

/* a journal that survives a frozen SoC: every step is on disk before the next one runs */
static void step(const char *what)
{
	printf("step: %s\n", what);
	fflush(stdout);
	fsync(1);
}

static uint32_t se_rd(uint32_t off) { return rd32(SE + off); }
static void se_wr(uint32_t off, uint32_t v) { wr32(SE + off, v); }

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int wait_clear(uint64_t pa, uint32_t mask, unsigned ms)
{
	uint64_t end = now_us() + 1000ull * ms;
	while (now_us() < end) {
		if (!(rd32(pa) & mask))
			return 0;
		usleep(10);
	}
	return -1;
}

/* ---- clocks: the RCG at an M/N rate (Linux clk_rcg2: M at +8, N = ~(n - m), D = ~n, dual-edge mode) ---- */

static unsigned rcg_khz;

static int rcg_set(uint32_t m, uint32_t n, unsigned khz)
{
	if (rcg_khz == khz)
		return 0;
	wr32(RCG + 0x08, m);
	wr32(RCG + 0x0c, ~(n - m) & 0xffff);
	wr32(RCG + 0x10, ~n & 0xffff);
	uint32_t cfg = rd32(RCG + 4) & ~(0x1fu | (7u << 8) | (3u << 12));
	cfg |= 1u | (1u << 8) | (2u << 12);	/* pre-div 1, GPLL0 main (parent map 0: 1), dual edge */
	wr32(RCG + 4, cfg);
	wr32(RCG, rd32(RCG) | 1u);		/* UPDATE */
	if (wait_clear(RCG, 1u, 50)) {
		printf("clk: RCG update timeout (cmd %#x)\n", rd32(RCG));
		return -1;
	}
	rcg_khz = khz;
	return 0;
}

static int clocks_on(void)
{
	step("gcc: RCG and branch before");
	printf("clk: vote %#x RCG cmd %#x cfg %#x CBCR %#x; GPLL0 mode %#x\n", rd32(GCC_VOTE), rd32(RCG), rd32(RCG + 4),
	       rd32(GCC_S3_CBCR), rd32(GCC + 0x0));
	step("gcc: vote the S3 branch");
	wr32(GCC_VOTE, rd32(GCC_VOTE) | GCC_S3_BIT);
	step("gcc: RCG to 7.3728 MHz from GPLL0 main");
	if (rcg_set(384, 31250, 7373))
		return -1;
	step("gcc: wait for the S3 branch");
	if (wait_clear(GCC_S3_CBCR, 1u << 31, 50)) {
		printf("clk: S3 branch halted (%#x)\n", rd32(GCC_S3_CBCR));
		return -1;
	}
	printf("clk: vote %#x, RCG cmd %#x cfg %#x M %#x N %#x D %#x, CBCR %#x\n", rd32(GCC_VOTE), rd32(RCG),
	       rd32(RCG + 4), rd32(RCG + 8), rd32(RCG + 0xc), rd32(RCG + 0x10), rd32(GCC_S3_CBCR));
	return 0;
}

/* ---- the UART ---- */

static unsigned tx_depth = 16;

static void rx_stop(void)
{
	if (!(se_rd(SE_GENI_STATUS) & (1u << 12)))	/* S_GENI_CMD_ACTIVE */
		return;
	se_wr(SE_GENI_S_CMD_CTRL, S_CMD_CANCEL);
	if (wait_clear(SE + SE_GENI_STATUS, 1u << 12, 20)) {
		/* qcom_geni_serial stop_rx: drain what holds the cancel back, then abort */
		uint32_t st = se_rd(SE_GENI_RX_FIFO_STATUS);
		for (unsigned i = 0; i < (st & 0x1ffffff); i++)
			(void)se_rd(SE_GENI_RX_FIFO);
		se_wr(SE_GENI_S_CMD_CTRL, S_CMD_ABORT);
		if (wait_clear(SE + SE_GENI_STATUS, 1u << 12, 20))
			printf("uart: RX will not stop (status %#x, rx fifo %#x, s_irq %#x)\n", se_rd(SE_GENI_STATUS),
			       se_rd(SE_GENI_RX_FIFO_STATUS), se_rd(SE_GENI_S_IRQ_STATUS));
	}
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
}

/* qcom_geni_serial stop_tx: a TX waiting for CTS survives the process (a stalled firmware segment left
 * "TX busy" for the next btd); cancel, then abort */
#define SE_GENI_M_CMD_CTRL	0x604
#define M_CMD_CANCEL		(1u << 2)
#define M_CMD_ABORT		(1u << 1)

static void tx_stop(void)
{
	if (!(se_rd(SE_GENI_STATUS) & 1u))	/* M_GENI_CMD_ACTIVE */
		return;
	se_wr(SE_GENI_M_CMD_CTRL, M_CMD_CANCEL);
	if (wait_clear(SE + SE_GENI_STATUS, 1u, 20)) {
		se_wr(SE_GENI_M_CMD_CTRL, M_CMD_ABORT);
		if (wait_clear(SE + SE_GENI_STATUS, 1u, 20))
			printf("uart: TX will not stop (status %#x, tx fifo %#x, m_irq %#x)\n", se_rd(SE_GENI_STATUS),
			       se_rd(SE_GENI_TX_FIFO_STATUS), se_rd(SE_GENI_M_IRQ_STATUS));
	}
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
}

static void rx_start(void)
{
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_GENI_S_CMD0, UART_START_READ << CMD_SHIFT);
}

/* baud from the RCG rate: SE divider = rate / (32 * baud), integer for the rates used here */
static int set_baud(unsigned baud)
{
	unsigned khz;
	if (baud == 3200000) {
		if (rcg_set(64, 375, 102400))
			return -1;
		khz = 102400;
	} else {
		if (rcg_set(384, 31250, 7373))
			return -1;
		khz = 7373;	/* 7372.8 kHz */
	}
	uint64_t hz = khz == 7373 ? 7372800ull : 1000ull * khz;
	uint32_t div = (uint32_t)(hz / (32ull * baud));
	if (!div || hz % (32ull * baud)) {
		printf("uart: %u baud is not an integer divider of %llu Hz\n", baud, (unsigned long long)hz);
		return -1;
	}
	rx_stop();
	se_wr(SE_GENI_CLK_SEL, 0);
	se_wr(GENI_SER_M_CLK_CFG, (div << 4) | 1u);
	se_wr(GENI_SER_S_CLK_CFG, (div << 4) | 1u);
	rx_start();
	return 0;
}

/* qcom_geni_serial set_mctrl: RTS held off by hand, or left to the hardware */
#define UART_MANUAL_RFR_EN	(1u << 31)
#define UART_RFR_NOT_READY	(1u << 1)

static void rts_hold(int hold)
{
	se_wr(SE_UART_MANUAL_RFR, hold ? UART_MANUAL_RFR_EN | UART_RFR_NOT_READY : 0);
}

static void flow_control(int on)
{
	uint32_t v = se_rd(SE_UART_TX_TRANS_CFG);
	se_wr(SE_UART_TX_TRANS_CFG, on ? (v & ~UART_CTS_MASK) : (v | UART_CTS_MASK));
}

static int uart_init(void)
{
	uint32_t fw = se_rd(GENI_FW_REVISION), p0 = se_rd(SE_HW_PARAM_0), p1 = se_rd(SE_HW_PARAM_1);
	printf("se13: fw %#x proto %u status %#x tx fifo %u x %u bits, rx %u\n", fw, (fw >> 8) & 0xff,
	       se_rd(SE_GENI_STATUS), (p0 >> 16) & 0x3f, (p0 >> 24) & 0x3f, (p1 >> 16) & 0x3f);
	if (((fw >> 8) & 0xff) != 2) {
		printf("se13: not UART firmware\n");
		return -1;
	}
	tx_depth = (p0 >> 16) & 0x3f;
	unsigned rx_depth = (p1 >> 16) & 0x3f;
	rx_stop();
	/* geni_se_irq_clear, geni_se_io_init, FIFO mode */
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_IRQ_EN, 0);	/* no SE interrupt to the GIC: XNU has no handler (0xffffffff, as Linux, froze the SoC) */
	se_wr(GENI_CGC_CTRL, se_rd(GENI_CGC_CTRL) | 0x7f);
	se_wr(SE_DMA_GENERAL_CFG, se_rd(SE_DMA_GENERAL_CFG) | 0x0f);
	se_wr(GENI_OUTPUT_CTRL, 0x7f);
	se_wr(GENI_FORCE_DEFAULT, 1);
	se_wr(SE_GENI_DMA_MODE_EN, 0);
	/* geni_se_config_packing(8 bits, 4 per word, LSB first, tx, rx) */
	se_wr(SE_GENI_TX_PACKING0, 0x4380e);
	se_wr(SE_GENI_TX_PACKING1, 0xc3e0e);
	se_wr(SE_GENI_RX_PACKING0, 0x4380e);
	se_wr(SE_GENI_RX_PACKING1, 0xc3e0e);
	se_wr(SE_GENI_BYTE_GRAN, 0);
	se_wr(SE_GENI_RX_WATERMARK, 2);
	se_wr(SE_GENI_RX_RFR_WM, rx_depth - 2);
	se_wr(SE_GENI_TX_WATERMARK, 0);
	se_wr(SE_GENI_M_IRQ_EN, 0xffffffffu);
	se_wr(SE_GENI_S_IRQ_EN, 0xffffffffu);
	/* 8N1, no parity, RFR by hardware, stale timeout 10 bits x 16 */
	se_wr(SE_UART_LOOPBACK, 0);
	se_wr(SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
	se_wr(SE_UART_RX_TRANS_CFG, 0);
	se_wr(SE_UART_TX_PARITY, 0);
	se_wr(SE_UART_RX_PARITY, 0);
	se_wr(SE_UART_TX_WORD_LEN, 8);
	se_wr(SE_UART_RX_WORD_LEN, 8);
	se_wr(SE_UART_TX_STOP_BIT, 0);
	se_wr(SE_UART_RX_STALE_CNT, 10 * 16);
	se_wr(SE_UART_MANUAL_RFR, 0);
	return 0;
}

static int uart_write(const uint8_t *b, unsigned n)
{
	if (wait_clear(SE + SE_GENI_STATUS, 1u, 100)) {	/* M_GENI_CMD_ACTIVE */
		printf("uart: TX busy, stopping it\n");
		tx_stop();
		if (se_rd(SE_GENI_STATUS) & 1u)
			return -1;
	}
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_UART_TX_TRANS_LEN, n);
	se_wr(SE_GENI_M_CMD0, UART_START_TX << CMD_SHIFT);
	unsigned off = 0;
	uint64_t end = now_us() + 2000000;
	while (off < n && now_us() < end) {
		unsigned used = se_rd(SE_GENI_TX_FIFO_STATUS) & 0x0fffffff;
		while (used < tx_depth && off < n) {
			uint32_t w = 0;
			for (unsigned j = 0; j < 4 && off + j < n; j++)
				w |= (uint32_t)b[off + j] << (8 * j);
			se_wr(SE_GENI_TX_FIFO, w);
			off += 4;
			used++;
		}
	}
	while (now_us() < end)
		if (se_rd(SE_GENI_M_IRQ_STATUS) & M_CMD_DONE) {
			se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
			return 0;
		}
	printf("uart: TX of %u bytes not done (status %#x m_irq %#x fifo %#x)\n", n, se_rd(SE_GENI_STATUS),
	       se_rd(SE_GENI_M_IRQ_STATUS), se_rd(SE_GENI_TX_FIFO_STATUS));
	tx_stop();
	return -1;
}

/* received bytes, as they come (qcom_geni_serial handle_rx_uart: the last word may be partial) */
static uint8_t rxq[8192];
static unsigned rxq_n;
static unsigned soc_log_bytes;	/* the controller's own log (ACL 0x2edc), dropped */

static void uart_poll(void)
{
	uint32_t st = se_rd(SE_GENI_RX_FIFO_STATUS);
	unsigned words = st & 0x1ffffff;
	if (!words)
		return;
	unsigned last = (st >> 31) & 1, lastb = (st >> 28) & 7;
	for (unsigned i = 0; i < words; i++) {
		uint32_t w = se_rd(SE_GENI_RX_FIFO);
		unsigned nb = (i == words - 1 && last && lastb) ? lastb : 4;
		for (unsigned j = 0; j < nb && rxq_n < sizeof(rxq); j++)
			rxq[rxq_n++] = (uint8_t)(w >> (8 * j));
	}
	se_wr(SE_GENI_S_IRQ_CLEAR, se_rd(SE_GENI_S_IRQ_STATUS));
}

/* ---- HCI over H4 ---- */

static int hci_cmd(uint16_t opcode, const uint8_t *p, uint8_t plen)
{
	uint8_t b[260] = { 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), plen };
	memcpy(b + 4, p, plen);
	printf("hci: > cmd %#06x (%u bytes)\n", opcode, plen);
	return uart_write(b, 4u + plen);
}

/* the next HCI event (H4 type 4): code and parameters; -1 on timeout */
static int hci_event(uint8_t *code, uint8_t *par, unsigned *plen, unsigned ms)
{
	uint64_t end = now_us() + 1000ull * ms;
	for (;;) {
		uart_poll();
		/* H4: ACL (2) and SCO (3) are skipped whole: the chip sends its own log as ACL on handle 0x2edc
		 * (hci_qca QCA_DEBUG_HANDLE); anything else is noise (e.g. during the pulses) */
		for (;;) {
			unsigned skip = 0;
			if (rxq_n >= 5 && rxq[0] == 0x02)
				skip = 5u + (rxq[3] | rxq[4] << 8);
			else if (rxq_n >= 4 && rxq[0] == 0x03)
				skip = 4u + rxq[3];
			else if (rxq_n && rxq[0] != 0x02 && rxq[0] != 0x03 && rxq[0] != 0x04) {
				printf("hci: dropping byte %#x\n", rxq[0]);
				skip = 1;
			}
			if (!skip || skip > rxq_n)
				break;
			if (skip > 1) {
				unsigned h = (rxq[1] | rxq[2] << 8) & 0xfff;
				if (h != (0x2edc & 0xfff))
					printf("hci: < %s handle %#x, %u bytes (skipped)\n", rxq[0] == 2 ? "ACL" : "SCO", h, skip);
				else
					soc_log_bytes += skip;
			}
			memmove(rxq, rxq + skip, rxq_n - skip);
			rxq_n -= skip;
		}
		if (rxq_n >= 3 && rxq[0] == 0x04 && rxq_n >= 3u + rxq[2]) {
			*code = rxq[1];
			*plen = rxq[2];
			memcpy(par, rxq + 3, rxq[2]);
			unsigned used = 3u + rxq[2];
			memmove(rxq, rxq + used, rxq_n - used);
			rxq_n -= used;
			return 0;
		}
		if (now_us() > end)
			return -1;
		usleep(200);
	}
}

static void dump(const char *what, const uint8_t *p, unsigned n)
{
	printf("%s:", what);
	for (unsigned i = 0; i < n && i < 64; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

/* ---- WCN3990 (btqca.h) ---- */

#define EDL_PATCH_CMD_OPCODE	0xfc00
#define EDL_PATCH_VER_REQ_CMD	0x19
#define EDL_APP_VER_RES_EVT	0x02
#define HCI_EV_VENDOR		0xff

static int power_on(void)
{
	static const uint8_t off = 0xc0, on = 0xfc;
	if (getenv("BT_NO_PULSE")) {	/* talk to a chip left running */
		flow_control(1);
		return set_baud(getenv("BT_BAUD") ? (unsigned)atoi(getenv("BT_BAUD")) : 115200);
	}
	flow_control(0);
	if (!getenv("BT_NO_OFF") && (set_baud(2400) || uart_write(&off, 1)))
		return -1;
	const char *e = getenv("BT_OFF_MS");
	usleep(1000u * (e ? (unsigned)atoi(e) : 2000));	/* 50 ms was not always enough for a running chip */
	if (set_baud(115200) || uart_write(&on, 1))
		return -1;
	e = getenv("BT_ON_MS");
	usleep(1000u * (e ? (unsigned)atoi(e) : 1000));	/* the SoC boots (Linux: 100 ms) */
	rxq_n = 0;
	(void)se_rd(SE_GENI_RX_FIFO_STATUS);
	flow_control(1);
	printf("bt: power pulses sent (2400 baud off, 115200 on)\n");
	return 0;
}

static int read_version(void)
{
	uint8_t c = EDL_PATCH_VER_REQ_CMD, code, par[256];
	unsigned n;
	if (hci_cmd(EDL_PATCH_CMD_OPCODE, &c, 1))
		return -1;
	for (int i = 0; i < 4; i++) {
		if (hci_event(&code, par, &n, 2000)) {
			printf("hci: no event (rx fifo status %#x, s_irq %#x, status %#x)\n", se_rd(SE_GENI_RX_FIFO_STATUS),
			       se_rd(SE_GENI_S_IRQ_STATUS), se_rd(SE_GENI_STATUS));
			return -1;
		}
		printf("hci: < event %#x, %u bytes\n", code, n);
		dump("hci:  ", par, n);
		if (code == HCI_EV_VENDOR && n >= 2 + 12 && par[1] == EDL_APP_VER_RES_EVT) {
			uint32_t product, soc;
			uint16_t patch, rom;
			memcpy(&product, par + 2, 4);
			memcpy(&patch, par + 6, 2);
			memcpy(&rom, par + 8, 2);
			memcpy(&soc, par + 10, 4);
			/* the QTI HAL picks the files by SoC id: Cherokee 2.1 (0x40010214) and 2.1.1 (0x40010224) use crbtfw21 */
			printf("bt: product %#x patch %#x rom %#x soc %#x%s\n", product, patch, rom, soc,
			       soc == 0x40010224 || soc == 0x40010214 ? " (Cherokee 2.1: crbtfw21.tlv, crnv21.bin)" : "");
			return 0;
		}
	}
	return -1;
}

/*
 * Operating speed, as hci_qca.c qca_set_speed(QCA_OPER_SPEED) for the WCN3990 (oper_speed 3200000): before the
 * firmware, RTS held off and CTS ignored, EDL_SET_BAUDRATE (0xfc48, QCA_BAUDRATE_3200000 = 0x11), the host to
 * the new rate, flow control back; the controller then sends a vendor event at the new rate (Linux drops it).
 * The NVM's HCI tag gets the same index (btqca.c qca_tlv_check_data). 115200 baud carries ~11 KB/s, less than
 * A2DP AAC at 224 kbit/s: bluetoothd flushed packets ("Audio Queue full") and the sound broke up.
 * /var/local/btd.slow keeps 115200.
 */
#define EDL_SET_BAUDRATE_OPCODE	0xfc48
#define QCA_BAUDRATE_115200	0x00
#define QCA_BAUDRATE_3200000	0x11
static unsigned oper_baud = 115200;

static int oper_speed(void)
{
	uint8_t idx = QCA_BAUDRATE_3200000, code, par[256];
	unsigned n;
	rts_hold(1);
	flow_control(0);
	if (hci_cmd(EDL_SET_BAUDRATE_OPCODE, &idx, 1))
		return -1;
	usleep(10000);	/* Linux: 1-10 ms for the controller to take it */
	if (set_baud(3200000))
		return -1;
	rxq_n = 0;
	flow_control(1);
	rts_hold(0);
	if (hci_event(&code, par, &n, 300) == 0)
		printf("bt: 3.2 Mbaud, controller event %#x (%u bytes)\n", code, n);
	else
		printf("bt: 3.2 Mbaud, no event from the controller\n");
	oper_baud = 3200000;
	return 0;
}

/* ---- firmware (btqca.c qca_download_firmware): the whole TLV file in 243-byte EDL_PATCH_TLV_REQ segments ---- */

#define EDL_PATCH_TLV_REQ_CMD	0x1e
#define EDL_TVL_DNLD_RES_EVT	0x04
#define MAX_TLV_SEGMENT		243
#define FW_DIR			"/var/local/fw/bt"

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 1);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

/* NVM: the HCI tag (17) gets the operating baud index in data[2] and in-band sleep off (Linux turns it on and
 * speaks the IBS wake protocol); deep sleep (tag 27) stays as the file has it */
static void nvm_fixup(uint8_t *d, size_t n)
{
	size_t off = 0;
	uint32_t tl;
	memcpy(&tl, d, 4);
	if ((tl & 0xff) == 4)
		off = 4;
	memcpy(&tl, d + off, 4);
	if ((tl & 0xff) != 2)
		return;
	size_t len = tl >> 8, idx = 0;
	uint8_t *t = d + off + 4;
	while (idx + 12 <= len && off + 4 + idx + 12 <= n) {
		uint16_t id, tlen;
		memcpy(&id, t + idx, 2);
		memcpy(&tlen, t + idx + 2, 2);
		uint8_t *data = t + idx + 12;
		if (id == 17 && tlen >= 3) {
			uint8_t bi = oper_baud == 3200000 ? QCA_BAUDRATE_3200000 : QCA_BAUDRATE_115200;
			printf("nvm: HCI tag %02x %02x %02x -> in-band sleep off, baud index %#x (%u)\n", data[0], data[1],
			       data[2], bi, oper_baud);
			data[0] &= ~0x80;	/* IBS: the host would have to wake the chip with 0xfd before each packet */
			data[2] = bi;
		}
		if (id == 2 && tlen == 6) {
			/* the ADT's local-mac-address (tools/mkadt.py), what bluetoothd believes; LSB first here */
			static const uint8_t addr[6] = { 0x02, 0x45, 0x45, 0x57, 0x53, 0x02 };
			printf("nvm: BD address %02x:%02x:%02x:%02x:%02x:%02x -> 02:53:57:45:45:02\n", data[5], data[4], data[3],
			       data[2], data[1], data[0]);
			memcpy(data, addr, 6);
		}
		idx += 12u + tlen;
	}
}

static int dnld_mode;	/* from the patch header; Linux reuses it for the NVM file too (one qca_fw_config) */

static int download(const char *name, int is_patch)
{
	char path[128];
	size_t n;
	snprintf(path, sizeof(path), FW_DIR "/%s", name);
	uint8_t *d = slurp(path, &n);
	if (!d) {
		printf("fw: no %s\n", path);
		return -1;
	}
	/* download_mode: 0 every segment answered (VSE + CC); 1/3 no vendor event per segment, maybe one after the last */
	if (is_patch && n > 14) {
		dnld_mode = d[14];
		uint16_t product, rom, patch;
		memcpy(&product, d + 16, 2);
		memcpy(&rom, d + 18, 2);
		memcpy(&patch, d + 20, 2);
		printf("fw: %s: %zu bytes, product %#x rom build %#x patch %#x, download mode %d\n", name, n, product, rom, patch,
		       dnld_mode);
	} else {
		nvm_fixup(d, n);
		printf("fw: %s: %zu bytes\n", name, n);
	}
	size_t off = 0;
	unsigned segs = 0;
	while (off < n) {
		unsigned len = n - off > MAX_TLV_SEGMENT ? MAX_TLV_SEGMENT : (unsigned)(n - off);
		uint8_t pkt[4 + 2 + MAX_TLV_SEGMENT] = { 0x01, (uint8_t)EDL_PATCH_CMD_OPCODE, (uint8_t)(EDL_PATCH_CMD_OPCODE >> 8),
							 (uint8_t)(len + 2), EDL_PATCH_TLV_REQ_CMD, (uint8_t)len };
		memcpy(pkt + 6, d + off, len);
		if (uart_write(pkt, 6 + len))
			goto fail;
		/* btqca: in mode 1/3 (the patch here) only the last segment gets its vendor event; the NVM file is mode
		 * none, every segment answered. Up to HCI_INIT_TIMEOUT: the last patch segment takes the chip a while */
		int skip = is_patch && (dnld_mode == 1 || dnld_mode == 3);
		if (!skip || off + len == n || len < MAX_TLV_SEGMENT) {
			uint8_t code, par[256];
			unsigned pl;
			for (int got = 0; !got;) {
				if (hci_event(&code, par, &pl, 10000)) {
					printf("fw: no answer to segment %u (offset %zu)\n", segs, off);
					if (skip)	/* Linux injects a Command Complete here and goes on */
						break;
					goto fail;
				}
				if (skip && code == 0x0e && pl >= 3 && !par[1] && !par[2]) {
					/* this chip ends the patch with a no-op Command Complete (opcode 0), not the vendor event */
					printf("fw: patch taken (no-op Command Complete)\n");
					got = 1;
					continue;
				}
				if (code == HCI_EV_VENDOR && pl >= 3 && par[1] == EDL_TVL_DNLD_RES_EVT) {
					if (par[0] != 0 || par[2] != 0) {
						printf("fw: segment %u refused: cresp %#x result %#x\n", segs, par[0], par[2]);
						goto fail;
					}
					got = 1;
				} else if (code != 0x0e || pl < 3 ||
					   ((par[1] | par[2] << 8) != EDL_PATCH_CMD_OPCODE && (par[1] | par[2]))) {
					/* the patched chip completes the segments with opcode 0 */
					printf("fw: (event %#x while downloading)\n", code);
					dump("fw:  ", par, pl);
				}
			}
		}
		off += len;
		segs++;
	}
	/* the Command Completes of the last segments */
	uint8_t code, par[256];
	unsigned pl;
	while (!hci_event(&code, par, &pl, 300))
		if ((code != 0x0e || pl < 3 || ((par[1] | par[2] << 8) != EDL_PATCH_CMD_OPCODE && (par[1] | par[2]))) &&
		    !(code == HCI_EV_VENDOR && pl >= 3 && par[1] == EDL_TVL_DNLD_RES_EVT && !par[0] && !par[2])) {
			printf("fw: (event %#x after the download)\n", code);
			dump("fw:  ", par, pl);
		}
	printf("fw: %s sent in %u segments\n", name, segs);
	free(d);
	return 0;
fail:
	free(d);
	return -1;
}

/* a command and its Command Complete (0x0e): the return parameters after the status */
static int hci_sync(uint16_t opcode, const uint8_t *p, uint8_t plen, uint8_t *ret, unsigned *rlen)
{
	if (hci_cmd(opcode, p, plen))
		return -1;
	for (int i = 0; i < 64; i++) {
		uint8_t code, par[256];
		unsigned n;
		if (hci_event(&code, par, &n, 10000)) {	/* HCI_INIT_TIMEOUT */
			printf("hci: no Command Complete for %#06x\n", opcode);
			return -1;
		}
		uint16_t op = n >= 3 ? (uint16_t)(par[1] | par[2] << 8) : 0;
		if (code == 0x0e && op == opcode) {
			if (ret && n > 3) {
				memcpy(ret, par + 3, n - 3);
				*rlen = n - 3;
			}
			if (n > 3 && par[3])
				printf("hci: %#06x status %#x\n", opcode, par[3]);
			return n > 3 ? par[3] : 0;
		}
		if (code == 0x0f && n >= 4 && (uint16_t)(par[2] | par[3] << 8) == opcode) {
			if (par[0])
				printf("hci: %#06x status %#x\n", opcode, par[0]);
			return par[0];
		}
		printf("hci: < event %#x (%u bytes) while waiting for %#06x\n", code, n, opcode);
		dump("hci:  ", par, n);
	}
	return -1;
}

static int controller_up(void)
{
	if (download("crbtfw21.tlv", 1) || download("crnv21.bin", 0))
		return -1;
	uint8_t r[256];
	unsigned rn = 0;
	if (hci_sync(0x0c03, NULL, 0, r, &rn)) {	/* HCI_Reset */
		printf("bt: reset failed\n");
		return -1;
	}
	if (hci_sync(0x1001, NULL, 0, r, &rn) == 0 && rn >= 9)	/* Read_Local_Version */
		printf("bt: HCI version %u rev %#x, LMP %u subver %#x, manufacturer %#x\n", r[1], r[2] | r[3] << 8, r[4],
		       r[7] | r[8] << 8, r[5] | r[6] << 8);
	if (hci_sync(0x1009, NULL, 0, r, &rn) == 0 && rn >= 7)	/* Read_BD_ADDR */
		printf("bt: address %02x:%02x:%02x:%02x:%02x:%02x\n", r[6], r[5], r[4], r[3], r[2], r[1]);
	return 0;
}

/* classic inquiry (GIAC, 10.24 s) and an LE scan, printing who answers */
static void scan(unsigned secs)
{
	uint8_t mask[8] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };	/* Linux default event mask */
	hci_sync(0x0c01, mask, 8, NULL, NULL);
	uint8_t lemask[8] = { 0x1f, 0, 0, 0, 0, 0, 0, 0 };
	hci_sync(0x2001, lemask, 8, NULL, NULL);
	uint8_t mode = 1;	/* inquiry with RSSI */
	hci_sync(0x0c45, &mode, 1, NULL, NULL);
	uint8_t lep[7] = { 1, 0x10, 0, 0x10, 0, 0, 0 };	/* active, interval = window = 10 ms, public, all */
	hci_sync(0x200b, lep, 7, NULL, NULL);
	uint8_t len[2] = { 1, 0 };
	hci_sync(0x200c, len, 2, NULL, NULL);
	uint8_t inq[5] = { 0x33, 0x8b, 0x9e, 8, 0 };	/* GIAC, 8 x 1.28 s */
	hci_cmd(0x0401, inq, 5);
	uint64_t end = now_us() + 1000000ull * secs;
	unsigned le = 0, br = 0;
	while (now_us() < end) {
		uint8_t code, par[256];
		unsigned n;
		if (hci_event(&code, par, &n, 500))
			continue;
		if (code == 0x22 && n >= 15) {		/* Inquiry Result with RSSI */
			printf("scan: BR/EDR %02x:%02x:%02x:%02x:%02x:%02x class %02x%02x%02x rssi %d\n", par[6], par[5], par[4],
			       par[3], par[2], par[1], par[11], par[10], par[9], (int8_t)par[14]);
			br++;
		} else if (code == 0x2f && n >= 15) {	/* Extended Inquiry Result */
			printf("scan: BR/EDR %02x:%02x:%02x:%02x:%02x:%02x class %02x%02x%02x rssi %d (EIR)\n", par[6], par[5],
			       par[4], par[3], par[2], par[1], par[11], par[10], par[9], (int8_t)par[14]);
			br++;
		} else if (code == 0x3e && n >= 12 && par[0] == 2) {	/* LE Advertising Report */
			unsigned dl = par[10];
			int8_t rssi = (int8_t)par[11 + dl];
			printf("scan: LE %02x:%02x:%02x:%02x:%02x:%02x (%s) rssi %d, %u bytes of data\n", par[9], par[8], par[7],
			       par[6], par[5], par[4], par[3] ? "random" : "public", rssi, dl);
			le++;
		} else if (code == 0x01) {
			printf("scan: inquiry complete\n");
		} else if (code != 0x0e && code != 0x0f && code != 0x02) {
			printf("scan: event %#x (%u bytes)\n", code, n);
		}
	}
	len[0] = 0;
	hci_sync(0x200c, len, 2, NULL, NULL);
	printf("scan: %u BR/EDR and %u LE reports in %u s (%u bytes of controller log dropped)\n", br, le, secs,
	       soc_log_bytes);
}

/* ---- serve: the controller to bluetoothd (BT_HCI_TRANSPORT=H4:POSIX:com.sweet.bt) ---- */

#define SWEET_BT_DEVICE 0x5357	/* sweet_btctl.c: this end is the controller */

/* xnu bsd/sys/kern_control.h, sys_domain.h (not in the iOS SDK) */
#define SYSPROTO_CONTROL	2
#define AF_SYS_CONTROL		2
struct ctl_info {
	uint32_t ctl_id;
	char ctl_name[96];
};
struct sockaddr_ctl {
	uint8_t sc_len;
	uint8_t sc_family;
	uint16_t ss_sysaddr;
	uint32_t sc_id;
	uint32_t sc_unit;
	uint32_t sc_reserved[5];
};
#define CTLIOCGINFO		_IOWR('N', 3, struct ctl_info)

/* the next whole H4 packet the controller sent, 0 if none yet; noise bytes are dropped */
static unsigned h4_take(uint8_t *out)
{
	for (;;) {
		unsigned n = 0;
		if (!rxq_n)
			return 0;
		switch (rxq[0]) {
		case 0x02:
			if (rxq_n >= 5)
				n = 5u + (rxq[3] | rxq[4] << 8);
			break;
		case 0x03:
			if (rxq_n >= 4)
				n = 4u + rxq[3];
			break;
		case 0x04:
			if (rxq_n >= 3)
				n = 3u + rxq[2];
			break;
		default:
			printf("serve: dropping byte %#x\n", rxq[0]);
			memmove(rxq, rxq + 1, --rxq_n);
			continue;
		}
		if (!n || n > rxq_n)
			return 0;
		memcpy(out, rxq, n);
		memmove(rxq, rxq + n, rxq_n - n);
		rxq_n -= n;
		return n;
	}
}

/* the kernel's relay (xnu 0162, bsd/dev/sweet_btctl.c), opened before the hardware is touched */
static int serve_fd = -1;

static int serve_open(const char *name)
{
	int m = socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL);
	struct ctl_info ci;
	memset(&ci, 0, sizeof(ci));
	strlcpy(ci.ctl_name, name, sizeof(ci.ctl_name));
	if (m < 0 || ioctl(m, CTLIOCGINFO, &ci)) {
		printf("serve: no kernel control %s (boot-arg sweetbt=1?): %s\n", name, strerror(errno));
		return -1;
	}
	struct sockaddr_ctl sc = { .sc_len = sizeof(sc), .sc_family = AF_SYSTEM, .ss_sysaddr = AF_SYS_CONTROL,
				   .sc_id = ci.ctl_id };
	int one = 1;
	if (connect(m, (struct sockaddr *)&sc, sizeof(sc)) ||
	    setsockopt(m, SYSPROTO_CONTROL, SWEET_BT_DEVICE, &one, sizeof(one))) {
		printf("serve: %s: %s\n", name, strerror(errno));
		return -1;
	}
	fcntl(m, F_SETFL, O_NONBLOCK);
	serve_fd = m;
	return 0;
}

/*
 * Commands bluetoothd needs to pass that this controller does not know: settings with nothing to return,
 * answered as if done. Every other refusal is logged and passed on.
 */
static const struct {
	uint16_t op;
	uint8_t octet, bit;	/* in Read Local Supported Commands: bluetoothd only sends what is listed there */
} fake_ok_ops[] = {
	{ 0x0c5b, 18, 3 },	/* Write Default Erroneous Data Reporting */
};

/* Read Local Supported Commands (0x1002): list the commands answered here */
static void supported_commands(uint8_t *pkt, unsigned n)
{
	if (n < 7u + 64 || pkt[6])	/* H4 type, code, length, ncmd, opcode, status, 64 octets */
		return;
	for (unsigned i = 0; i < sizeof(fake_ok_ops) / sizeof(fake_ok_ops[0]); i++)
		pkt[7 + fake_ok_ops[i].octet] |= (uint8_t)(1u << fake_ok_ops[i].bit);
}

static void fake_ok(const uint8_t *pkt, uint16_t op, uint8_t *status)
{
	(void)pkt;
	int fake = 0;
	for (unsigned i = 0; i < sizeof(fake_ok_ops) / sizeof(fake_ok_ops[0]); i++)
		fake |= fake_ok_ops[i].op == op;
	printf("serve: %#06x refused with status %#x%s\n", op, *status, fake ? ", answered as done" : "");
	if (fake)
		*status = 0;
}

static int serve(const char *name)
{
	int m = serve_fd;
	const char *sn = name;
	int trace = getenv("BTD_TRACE") != NULL || !access("/var/local/btd.trace", F_OK);
	printf("serve: kernel control %s, H4 to the controller\n", sn);
	static uint8_t pkt[sizeof(rxq)];
	static uint8_t up[4096];
	unsigned up_n = 0, own_addr_logged = 0;
	int random_set = 0;
	unsigned long up_cmds = 0, up_bytes = 0, down_pkts = 0, logs = 0, vsc = 0;
	uint64_t next_stat = now_us() + 60000000ull;
	for (;;) {
		ssize_t r = read(m, up + up_n, sizeof(up) - up_n);
		if (r > 0) {
			up_n += (unsigned)r;
			up_bytes += (unsigned long)r;
			/* whole H4 packets from the host: commands, ACL, SCO */
			for (;;) {
				unsigned len = 0;
				if (up_n >= 4 && up[0] == 0x01)
					len = 4u + up[3];
				else if (up_n >= 5 && up[0] == 0x02)
					len = 5u + (up[3] | up[4] << 8);
				else if (up_n >= 4 && up[0] == 0x03)
					len = 4u + up[3];
				else if (up_n && up[0] != 0x01 && up[0] != 0x02 && up[0] != 0x03) {
					printf("serve: host sent %#x, dropping\n", up[0]);
					memmove(up, up + 1, --up_n);
					continue;
				}
				if (!len || len > up_n)
					break;
				uint16_t op = up[0] == 0x01 ? (uint16_t)(up[1] | up[2] << 8) : 0;
				if (up[0] == 0x01) {
					up_cmds++;
					if (trace) {
						printf("serve: > cmd %#06x (%u bytes)", op, up[3]);
						for (unsigned i = 0; i < up[3] && i < 16; i++)
							printf(" %02x", up[4 + i]);
						printf("\n");
					}
				}
				if (up[0] == 0x01 && (op == 0x200b || op == 0x2006) && up[3] >= 6 && up[4 + 5] == 1) {
					/* own address type random: bluetoothd sets the random address with a Broadcom
					 * command that never reaches this controller, which then refuses (0x0c); use
					 * the public address until the random one is set the standard way (0x2005) */
					if (!random_set) {
						up[4 + 5] = 0;
						if (!own_addr_logged++)
							printf("serve: LE own address random -> public (no random address set)\n");
					}
				}
				if (up[0] == 0x01 && op == 0x2005)
					random_set = 1;
				if (up[0] == 0x01 && op == 0x204a) {
					/* an extended-advertising-era LE command: once the controller has seen one it
					 * refuses the legacy LE commands bluetoothd goes on to use (Set Scan Parameters,
					 * Read Advertising Channel Tx Power: 0x0c Command Disallowed) */
					uint8_t cc[7] = { 0x04, 0x0e, 4, 1, (uint8_t)op, (uint8_t)(op >> 8), 0x01 };
					if (write(m, cc, sizeof(cc)) != (ssize_t)sizeof(cc))
						printf("serve: reply to %#06x lost\n", op);
					memmove(up, up + len, up_n - len);
					up_n -= len;
					continue;
				}
				if (up[0] == 0x01 && op >> 10 == 0x3f) {
					/* vendor commands are Broadcom's here (BD_VSC_*): never to the Qualcomm firmware,
					 * whose own vendor commands share the range (0xfc00 is its download command) */
					/* Unknown HCI Command, with two bytes after the status: the commands whose reply
					 * goes through _BCM_ByteStreamEventParser (e.g. BD_VSC_FACTORY_CAL_READ_TABLE)
					 * only complete, with the error, when it has 2 bytes to read */
					uint8_t cc[9 + 32] = { 0x04, 0x0e, 6, 1, (uint8_t)op, (uint8_t)(op >> 8), 0x01,
							       up[3] ? up[4] : 0, 0x01 };
					if (op == 0xfce2 && up[3]) {
						/* BD_VSC_OLYMPIC_*: one opcode, the sub-command in the first byte. Its reply
						 * goes through _BCM_ByteStreamEventParser, which wants at least 2 bytes
						 * (sub-command, its status) before it calls the command's callback;
						 * without that ANTENNA_PREF_B (0x15) never completes and after 15 s
						 * bluetoothd restarts ("timedout waiting for controller") */
						cc[2] = 6 + 32;
						cc[6] = 0;
						cc[7] = up[4];
						cc[8] = 0;
						/* and zeros for what the sub-command's own callback reads (0x0d, power
						 * statistics, asserts "ByteStream_NumReadBytesAvail(*pBs) >= 2") */
					}
					if (write(m, cc, 3u + cc[2]) != (ssize_t)(3u + cc[2]))
						printf("serve: reply to %#06x lost\n", op);
					vsc++;
				} else {
					for (unsigned off = 0; off < len;) {
						unsigned c = len - off > 240 ? 240 : len - off;
						if (uart_write(up + off, c))
							printf("serve: UART TX failed\n");
						off += c;
					}
				}
				memmove(up, up + len, up_n - len);
				up_n -= len;
			}
		}
		uart_poll();
		unsigned n;
		while ((n = h4_take(pkt))) {
			if (pkt[0] == 0x02 && ((pkt[1] | pkt[2] << 8) & 0xfff) == (0x2edc & 0xfff)) {
				logs++;
				continue;
			}
			if (trace && pkt[0] == 0x04)
				printf("serve: < event %#x (%u bytes)%s\n", pkt[1], pkt[2],
				       pkt[1] == 0x0e && n >= 7 && pkt[6] ? " (status != 0)" : "");
			if (pkt[0] == 0x04 && pkt[1] == 0x0e && n >= 7 && (pkt[4] | pkt[5] << 8) == 0x1002)
				supported_commands(pkt, n);
			if (pkt[0] == 0x04 && pkt[1] == 0x0e && n >= 7 && pkt[6])
				fake_ok(pkt, pkt[4] | pkt[5] << 8, &pkt[6]);
			else if (pkt[0] == 0x04 && pkt[1] == 0x0f && n >= 7 && pkt[3])
				fake_ok(pkt, pkt[5] | pkt[6] << 8, &pkt[3]);
			down_pkts++;
			for (unsigned off = 0; off < n;) {
				ssize_t w = write(m, pkt + off, n - off);
				if (w > 0)
					off += (unsigned)w;
				else if (errno == EAGAIN)
					usleep(200);
				else
					break;
			}
		}
		if (now_us() > next_stat) {
			printf("serve: %lu commands (%lu vendor, answered here), %lu bytes up; %lu packets down, %lu controller "
			       "log packets dropped\n", up_cmds, vsc, up_bytes, down_pkts, logs);
			next_stat = now_us() + 60000000ull;
		}
		if (r <= 0)
			usleep(300);
	}
}

static void pins(void)
{
	/* function qup13 (mux 1), 2 mA: CTS 43 and RX 46 no pull, RTS 44 pull-down, TX 45 pull-up (live DT) */
	static const struct { unsigned pin, pull; } p[] = { { 43, 0 }, { 44, 1 }, { 45, 3 }, { 46, 0 } };
	for (unsigned i = 0; i < 4; i++) {
		uint64_t cfg = TLMM_EAST + 0x1000ull * p[i].pin;
		wr32(cfg, (rd32(cfg) & ~0x3ffu) | (1u << 2) | p[i].pull);
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2 || (strcmp(argv[1], "version") && strcmp(argv[1], "up") && strcmp(argv[1], "scan") &&
			 strcmp(argv[1], "serve"))) {
		fprintf(stderr, "usage: btd version | up | scan [SECS] | serve [NAME]\n");
		return 2;
	}
	if (!strcmp(argv[1], "serve")) {
		/* at boot: nothing to do without the kernel's relay, or when switched off */
		if (!access("/var/local/btd.off", F_OK)) {
			printf("serve: /var/local/btd.off, not starting\n");
			return 0;
		}
		if (serve_open(argc > 2 ? argv[2] : "com.sweet.bt"))
			return 0;
	}
	pm_init();
	step("pins");
	pins();
	if (clocks_on())
		return 1;
	step("uart: init");
	if (uart_init())
		return 1;
	/* a chip left running by an earlier btd does not always take the pulses: off, a pause, on, again */
	/* at boot (launchd job com.sweet.btd) the rails may not be up yet: wland turns them on for WLAN */
	/* 3.2 Mbaud twice, then 115200: a boot once stalled TX (no CTS) on the first patch segment at 3.2 Mbaud */
	int fast = !getenv("BT_NO_PULSE") && access("/var/local/btd.slow", F_OK) != 0 ? 2 : 0;
	for (;;) {
		int ok = 0, tries = strcmp(argv[1], "serve") ? 4 : 30;
		for (int i = 0; i < tries && !ok; i++) {
			step("bt: power pulses");
			oper_baud = 115200;
			if (power_on())
				return 1;
			step("bt: version");
			ok = !read_version();
			if (ok && fast) {
				step("bt: 3.2 Mbaud");
				ok = !oper_speed() && !read_version();
				if (!ok) {
					printf("bt: no answer at 3.2 Mbaud, power cycling\n");
					fast--;
				}
			}
			if (!ok)
				usleep(i < 3 ? 200000 : 3000000);
		}
		if (!ok)
			return 1;
		if (!strcmp(argv[1], "version"))
			return 0;
		step("bt: firmware");
		if (!controller_up())
			break;
		if (!fast)
			return 1;
		fast--;
		printf("bt: bring-up at %u baud failed, again%s\n", oper_baud, fast ? "" : " at 115200");
	}
	if (!strcmp(argv[1], "scan"))
		scan(argc > 2 ? (unsigned)atoi(argv[2]) : 12);
	if (!strcmp(argv[1], "serve"))
		return serve(argc > 2 ? argv[2] : "com.sweet.bt") ? 1 : 0;
	return 0;
}

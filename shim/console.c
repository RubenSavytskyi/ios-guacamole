/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

/* * * * * * * * * * * * * * * *
 * Capture: the same text, but kept as finished screen rows (see console.h).
 */
#define CAP_LINES	400
#define CAP_COLS	64

static char cap[CAP_LINES][CAP_COLS];
static unsigned ncap, cap_col;
static int capturing;

static void cap_newline(void)
{
	if (ncap + 1 >= CAP_LINES)
		return;
	ncap++;
	cap[ncap][0] = 0;
	cap_col = 0;
}

static void cap_putc(char c)
{
	if (c == '\r')
		return;
	if (c == '\n') {
		cap_newline();
		return;
	}
	if (cap_col >= (unsigned)(FB_W / CH_W))
		cap_newline();
	if (cap_col + 1 < CAP_COLS) {
		cap[ncap][cap_col++] = c;
		cap[ncap][cap_col] = 0;
	}
}

static void cap_puts(const char *s)
{
	while (*s)
		cap_putc(*s++);
}

void con_capture_begin(void)
{
	ncap = 0;
	cap_col = 0;
	cap[0][0] = 0;
	capturing = 1;
}

static void wait_ms(unsigned ms)
{
#if defined(__aarch64__)
	uint64_t freq = 0, start = 0, now = 0, ticks;

	__asm__ volatile("isb; mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
	ticks = freq / 1000ull * ms;
	do {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	} while (now - start < ticks);
#else
	(void)ms;	/* host_test: no pager, the layout is what matters */
#endif
}

void con_capture_show(unsigned rows, unsigned hold_ms, unsigned passes)
{
	static const char hex[] = "0123456789";
	const unsigned maxrows = (unsigned)(FB_H / CH_H);
	unsigned pages;
	char hdr[CAP_COLS];

	if (rows == 0 || rows > maxrows - 1)
		rows = maxrows - 1;
	if (cap_col > 0)
		cap_newline();
	if (ncap == 0 || passes == 0)
		return;
	pages = (ncap + rows - 1) / rows;

	for (unsigned p = 0; p < passes; p++) {
		for (unsigned pg = 0; pg < pages; pg++) {
			unsigned i = 0;

			capturing = 0;	/* draw, do not record the page header itself */
			con_clear(BLACK);
			{
				char tmp[8];
				int n = 0, col = 0;
				unsigned v = pg + 1;

				while (v) {
					tmp[n++] = hex[v % 10];
					v /= 10;
				}
				while (n)
					hdr[col++] = tmp[--n];
				hdr[col++] = '/';
				v = pages;
				n = 0;
				while (v) {
					tmp[n++] = hex[v % 10];
					v /= 10;
				}
				while (n)
					hdr[col++] = tmp[--n];
				hdr[col] = 0;
			}
			con_str("PAGE", hdr);
			for (unsigned li = pg * rows; li < (pg + 1) * rows && li < ncap; li++, i++) {
				put_str(0, cap[li], WHITE);
				row++;
			}
			wait_ms(hold_ms);
		}
	}
	capturing = 0;
}

void con_skip(void)
{
	if (capturing) {
		cap_newline();
		return;
	}
	row++;
}

void con_str(const char *label, const char *s)
{
	if (capturing) {
		cap_puts(label);
		while (cap_col < LABEL_COLS)
			cap_putc(' ');
		cap_puts(s);
		cap_newline();
		return;
	}
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

/* Long free-form lines: wrap at the right edge instead of clipping. */
void con_text(const char *s)
{
	const int maxcol = FB_W / CH_W;
	const int maxrow = FB_H / CH_H;
	int col = 0;

	if (capturing) {
		while (*s)
			cap_putc(*s++);
		cap_newline();
		return;
	}
	while (*s) {
		char ch = *s++;
		if (ch == '\r') {
			continue;
		}
		if (ch == '\n' || col >= maxcol) {
			row++;
			col = 0;
			if (ch == '\n') {
				continue;
			}
		}
		if (row >= maxrow - 1) {
			return;
		}
		put_char(col++, ch, WHITE);
	}
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);

	if (capturing) {
		cap_puts(label);
		while (cap_col < LABEL_COLS)
			cap_putc(' ');
		cap_puts(a);
		fmt_hex(buf, va, 8);
		cap_puts(buf);
		if (b) {
			cap_putc(' ');
			cap_puts(b);
			fmt_hex(buf, vb, 8);
			cap_puts(buf);
		}
		cap_newline();
		return;
	}
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}

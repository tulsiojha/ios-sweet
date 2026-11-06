/*
 * stage0 v8 AArch64 payload: polled ChipIdea USB device-mode console plus
 * an SDHCI eMMC block layer (8-bit/HS switch, multi-block reads, GPT scan).
 *
 * Runs at EL1 with the MMU and caches off, entered at 0x8000a000 by the
 * ARM32 secure-monitor stub (x0 = dtb). The USB controller is driven the
 * same way this phone's LK drives it for fastboot (platform/msm_shared/
 * hsusb.c), but with polling instead of interrupts:
 *   GCC USB clocks (80 MHz from GPLL0) -> block reset -> controller reset
 *   -> ULPI VBUS override -> qHead list -> USBMODE device -> attach.
 *
 * The device enumerates as a CDC-ACM serial (1209:0001) whose bulk-IN
 * endpoint streams a text log, so on the host it is just
 * /dev/tty.usbmodemXXXX and `cat` shows the log. A stale bulk-IN chunk is
 * retried instead of dropped, and a boot-info line is replayed whenever
 * the host opens the port (SET_LINE_CODING).
 *
 * The eMMC probe re-initializes SDCC1 from scratch (LK shuts it down to
 * put the card to sleep before the kernel handoff), walks CMD0/1/2/3/9/7/16
 * at 400 kHz and reads LBA0/LBA1 through ADMA2, logging the MBR/GPT
 * signatures. It then reads EXT_CSD (CMD8), switches the card to 8-bit
 * bus + high speed (CMD6), raises SDCC1 to 25 MHz, verifies with a fresh
 * LBA0 read (falling back to 1-bit/390 kHz on failure) and finally reads
 * the GPT entry array with CMD23 + a multi-block CMD18, listing the
 * named partitions. Everything is polled; the GIC stays off.
 */

#include <stdarg.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define NULL ((void *)0)

/* ---------------- MMIO, memory, timer ---------------- */

static inline u32 rd(u64 a) { return *(volatile u32 *)a; }
static inline void wr(u64 a, u32 v) { *(volatile u32 *)a = v; }
static inline u8 rd8(u64 a) { return *(volatile u8 *)a; }
static inline u16 rd16(u64 a) { return *(volatile u16 *)a; }
static inline void wr8(u64 a, u8 v) { *(volatile u8 *)a = v; }
static inline void wr16(u64 a, u16 v) { *(volatile u16 *)a = v; }
static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

void *memcpy(void *dst, const void *src, unsigned long n)
{
	u8 *d = dst;
	const u8 *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
	u8 *d = dst;
	while (n--)
		*d++ = (u8)c;
	return dst;
}

/* reference implementations in case the compiler emits libcalls */
u64 __udivdi3(u64 a, u64 b)
{
	u64 q = 0, r = 0;
	int i;
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((a >> i) & 1);
		if (r >= b) {
			r -= b;
			q |= 1ull << i;
		}
	}
	return q;
}

u64 __umoddi3(u64 a, u64 b)
{
	u64 r = 0;
	int i;
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((a >> i) & 1);
		if (r >= b)
			r -= b;
	}
	return r;
}

static u32 g_tick_ms;
static u32 g_spin_budget;	/* fallback when no counter: loops per ms */

static inline u32 ticks(void)
{
	u64 t;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	return (u32)t;
}

static void timer_init(void)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	g_spin_budget = 400000;
	if (f >= 1000000 && f <= 100000000) {
		u32 t0 = ticks();
		volatile u32 spin = 20000;
		while (spin--)
			;
		if (ticks() != t0)
			g_tick_ms = (u32)(f / 1000);
	}
}

static u32 elapsed_ms(u32 start)
{
	if (!g_tick_ms)
		return 0;
	return (ticks() - start) / g_tick_ms;
}

/* spin (no timer) or tick based delay */
static void delay_ms(u32 ms)
{
	if (g_tick_ms) {
		u32 s = ticks();
		while (elapsed_ms(s) < ms)
			;
	} else {
		volatile u32 n = ms * g_spin_budget;
		while (n)
			n--;
	}
}

/* wait until (rd(addr) & mask) == 0, bounded */
static void wait_clear(u64 addr, u32 mask, u32 ms)
{
	if (g_tick_ms) {
		u32 s = ticks();
		while ((rd(addr) & mask) && elapsed_ms(s) < ms)
			;
	} else {
		volatile u32 n = ms * g_spin_budget;
		while ((rd(addr) & mask) && n)
			n--;
	}
}

/* ---------------- log ring ---------------- */

#define RINGSZ 16384

static u8 g_ring[RINGSZ];
static u32 g_ring_r, g_ring_w, g_ring_drops;

static void log_putc(char c)
{
	u32 n = (g_ring_w + 1) & (RINGSZ - 1);
	if (n == g_ring_r) {
		g_ring_drops++;
		return;
	}
	g_ring[g_ring_w] = (u8)c;
	g_ring_w = n;
}

static void log_puts(const char *s)
{
	while (*s)
		log_putc(*s++);
}

static void log_hex(u64 v, int digits)
{
	int i;
	for (i = (digits - 1) * 4; i >= 0; i -= 4) {
		int d = (int)(v >> i) & 0xf;
		log_putc(d < 10 ? '0' + d : 'a' + d - 10);
	}
}

static void log_dec(u32 v)
{
	char b[12];
	int i = 0;
	if (!v) {
		log_putc('0');
		return;
	}
	while (v) {
		b[i++] = '0' + v % 10;
		v /= 10;
	}
	while (i--)
		log_putc(b[i]);
}

static void log_strn(const char *s, u32 max)
{
	u32 i;

	for (i = 0; i < max && s[i]; i++) {
		char c = s[i];
		log_putc((c >= 0x20 && c <= 0x7e) ? c : '.');
	}
}

void log_printf(const char *fmt, ...)
{
	va_list ap;
	const char *p;

	va_start(ap, fmt);
	for (p = fmt; *p; p++) {
		int l = 0;

		if (*p != '%') {
			log_putc(*p);
			continue;
		}
		p++;
		if (*p == 'l') {
			l++;
			p++;
		}
		switch (*p) {
		case '%':
			log_putc('%');
			break;
		case 'c':
			log_putc((char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			log_puts(s ? s : "(null)");
			break;
		}
		case 'd': {
			int v = va_arg(ap, int);
			if (v < 0) {
				log_putc('-');
				v = -v;
			}
			log_dec((u32)v);
			break;
		}
		case 'u':
			log_dec(va_arg(ap, u32));
			break;
		case 'x':
			if (l)
				log_hex(va_arg(ap, u64), 16);
			else
				log_hex((u64)va_arg(ap, u32), 8);
			break;
		default:
			log_putc('?');
			break;
		}
	}
	va_end(ap);
}

static u32 ring_read(u8 *dst, u32 max)
{
	u32 n = 0;
	while (n < max && g_ring_r != g_ring_w) {
		dst[n++] = g_ring[g_ring_r];
		g_ring_r = (g_ring_r + 1) & (RINGSZ - 1);
	}
	return n;
}

/* ---------------- USB controller ---------------- */

#define USB_BASE		0x078d9000u

#define USB_SBUSCFG		0x090
#define USB_AHB_MODE		0x098
#define USB_GENCONFIG_2		0x0a0
#define USB_CAPLENGTH		0x100
#define USB_HCSPARAMS		0x104
#define USB_USBCMD		0x140
#define USB_USBSTS		0x144
#define USB_USBINTR		0x148
#define USB_DEVICEADDR		0x154
#define USB_ENDPOINTLISTADDR	0x158
#define USB_ULPI_VIEWPORT	0x170
#define USB_PORTSC		0x184
#define USB_USBMODE		0x1a8
#define USB_ENDPTSETUPSTAT	0x1ac
#define USB_ENDPTPRIME		0x1b0
#define USB_ENDPTFLUSH		0x1b4
#define USB_ENDPTCOMPLETE	0x1bc
#define USB_ENDPTCTRL(n)	(0x1c0 + 4 * (n))

#define USBCMD_ITC8_RS		0x00080001u
#define USBCMD_ITC8_RST		0x00080002u

#define STS_URI			(1u << 6)
#define STS_PCI			(1u << 2)
#define STS_UEI			(1u << 1)
#define STS_UI			(1u << 0)

#define EPT_TX(n)		(1u << ((n) + 16))
#define EPT_RX(n)		(1u << (n))

#define CTRL_TXE		(1u << 23)
#define CTRL_TXR		(1u << 22)
#define CTRL_TXS		(1u << 16)
#define CTRL_RXE		(1u << 7)
#define CTRL_RXR		(1u << 6)
#define CTRL_RXS		(1u << 0)
#define CTRL_TXT_BULK		(2u << 18)
#define CTRL_RXT_BULK		(2u << 2)

#define CONFIG_ZLT		(1u << 29)
#define CONFIG_IOS		(1u << 15)

#define INFO_IOC		(1u << 15)
#define INFO_ACTIVE		(1u << 7)

#define ULPI_MISC_A		0x96
#define ULPI_MISC_A_SET		0x97
#define ULPI_MISC_A_VBUSVLDEXTSEL (1u << 1)
#define ULPI_MISC_A_VBUSVLDEXT	(1u << 0)

struct qh {
	u32 config;
	u32 current;
	u32 next;
	u32 info;
	u32 page0, page1, page2, page3, page4;
	u32 reserved0;
	u8 setup[8];
	u32 reserved1, reserved2, reserved3, reserved4;
};

struct td {
	u32 next;
	u32 info;
	u32 page0, page1, page2, page3, page4;
	u32 reserved;
};

_Static_assert(sizeof(struct qh) == 64, "qh layout");
_Static_assert(sizeof(struct td) == 32, "td layout");

static struct qh g_qh[32] __attribute__((aligned(4096)));
static u8 g_ep0_buf[4096] __attribute__((aligned(4096)));
static u8 g_tx_buf[4096] __attribute__((aligned(4096)));
static struct td g_td_ep0in __attribute__((aligned(64)));
static struct td g_td_ep0out __attribute__((aligned(64)));
static struct td g_td_ep3 __attribute__((aligned(64)));

static u32 g_configured;
static u32 g_config_value;
static u32 g_tx_busy;
static u32 g_tx_start;
static u32 g_tx_len;

/* kept for the boot-info replay on host open */
static u64 g_boot_dtb, g_boot_cntfrq;
static u32 g_boot_dtb_ok, g_boot_midr, g_boot_wdt_en;
static u32 g_emmc_state = 0xffff;	/* 0 = lba0 read ok */

static void boot_replay(void);

enum {
	EP0_IDLE,
	EP0_DATA_IN,
	EP0_STATUS_IN,
	EP0_STATUS_OUT,
	EP0_DATA_OUT,
};

static u32 g_ep0_state = EP0_IDLE;

static u32 ulpi_read(u32 reg)
{
	volatile u32 n = 1000000;

	wr(USB_BASE + USB_ULPI_VIEWPORT, (1u << 30) | ((reg & 0xff) << 16));
	while ((rd(USB_BASE + USB_ULPI_VIEWPORT) & (1u << 30)) && n)
		n--;
	return (rd(USB_BASE + USB_ULPI_VIEWPORT) >> 8) & 0xff;
}

static void ulpi_write(u32 reg, u32 val)
{
	volatile u32 n = 1000000;

	wr(USB_BASE + USB_ULPI_VIEWPORT,
	   (1u << 30) | (1u << 29) | ((reg & 0xff) << 16) | (val & 0xff));
	while ((rd(USB_BASE + USB_ULPI_VIEWPORT) & (1u << 30)) && n)
		n--;
}

static void qh_prime(u32 ep, u32 in, struct td *td, const void *buf, u32 len)
{
	u32 addr = (u32)(u64)buf;
	u32 base = addr & ~0xfffu;
	struct qh *h = &g_qh[ep * 2 + in];

	td->next = 1;		/* TERMINATE */
	td->info = (len << 16) | INFO_IOC | INFO_ACTIVE;
	td->page0 = addr;
	td->page1 = base + 0x1000;
	td->page2 = base + 0x2000;
	td->page3 = base + 0x3000;
	td->page4 = base + 0x4000;
	td->reserved = 0;
	barrier();

	h->next = (u32)(u64)td;
	h->info = 0;
	barrier();

	wr(USB_BASE + USB_ENDPTPRIME, in ? EPT_TX(ep) : EPT_RX(ep));
}

static u32 bus_highspeed(void)
{
	return ((rd(USB_BASE + USB_PORTSC) >> 26) & 3) == 2;
}

static void ep0_status_in(void)
{
	qh_prime(0, 1, &g_td_ep0in, g_ep0_buf, 0);
	g_ep0_state = EP0_STATUS_IN;
}

static void ep0_status_out(void)
{
	qh_prime(0, 0, &g_td_ep0out, g_ep0_buf, 0);
	g_ep0_state = EP0_STATUS_OUT;
}

static void ep0_recv(u32 maxlen)
{
	if (maxlen > sizeof(g_ep0_buf))
		maxlen = sizeof(g_ep0_buf);
	qh_prime(0, 0, &g_td_ep0out, g_ep0_buf, maxlen);
	g_ep0_state = EP0_DATA_OUT;
}

static void ep0_send(const u8 *data, u32 len, u32 wlen)
{
	if (len > wlen)
		len = wlen;
	if (len > sizeof(g_ep0_buf))
		len = sizeof(g_ep0_buf);
	if (len)
		memcpy(g_ep0_buf, data, len);
	qh_prime(0, 1, &g_td_ep0in, g_ep0_buf, len);
	g_ep0_state = len ? EP0_DATA_IN : EP0_STATUS_IN;
}

static void ep0_stall(void)
{
	wr(USB_BASE + USB_ENDPTCTRL(0),
	   rd(USB_BASE + USB_ENDPTCTRL(0)) | CTRL_TXS | CTRL_RXS);
	g_ep0_state = EP0_IDLE;
}

/* ---------------- descriptors ---------------- */

static const u8 g_dev_desc[18] = {
	0x12, 0x01, 0x00, 0x02, 0x02, 0x00, 0x00, 0x40,
	0x09, 0x12,		/* idVendor 0x1209 */
	0x01, 0x00,		/* idProduct 0x0001 */
	0x00, 0x01,		/* bcdDevice 1.00 */
	0x01, 0x02, 0x03,	/* iManufacturer, iProduct, iSerial */
	0x01,			/* bNumConfigurations */
};

#define CONFIG_DESC_LEN 67
static u8 g_config_desc[CONFIG_DESC_LEN] = {
	0x09, 0x02, 0x43, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
	/* CDC communication interface, ACM */
	0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
	0x05, 0x24, 0x00, 0x10, 0x01,			/* header */
	0x05, 0x24, 0x01, 0x00, 0x01,			/* call management */
	0x04, 0x24, 0x02, 0x02,				/* ACM */
	0x05, 0x24, 0x06, 0x00, 0x01,			/* union */
	0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x10,	/* EP1 IN int */
	/* CDC data interface */
	0x09, 0x04, 0x01, 0x00, 0x02, 0x0a, 0x00, 0x00, 0x00,
	0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,	/* EP2 OUT bulk */
	0x07, 0x05, 0x83, 0x02, 0x00, 0x02, 0x00,	/* EP3 IN bulk */
};

static const u8 g_qual_desc[10] = {
	0x0a, 0x06, 0x00, 0x02, 0x02, 0x00, 0x00, 0x40, 0x01, 0x00,
};

static const u8 g_str_lang[4] = { 0x04, 0x03, 0x09, 0x04 };
static u8 g_str_mfr[2 + 64], g_str_prod[2 + 64], g_str_ser[2 + 64];
static u32 g_str_mfr_len, g_str_prod_len, g_str_ser_len;

static void build_string(u8 *dst, u32 *lenp, const char *s)
{
	u32 n = 0, i;
	while (s[n] && n < 32)
		n++;
	dst[0] = (u8)(2 + n * 2);
	dst[1] = 0x03;
	for (i = 0; i < n; i++) {
		dst[2 + 2 * i] = (u8)s[i];
		dst[3 + 2 * i] = 0;
	}
	*lenp = 2 + n * 2;
}

static void patch_config_desc(void)
{
	u32 mp = bus_highspeed() ? 512 : 64;
	g_config_desc[57] = (u8)(mp & 0xff);
	g_config_desc[58] = (u8)(mp >> 8);
	g_config_desc[64] = (u8)(mp & 0xff);
	g_config_desc[65] = (u8)(mp >> 8);
}

/* ---------------- enumeration ---------------- */

static void usb_set_config(u32 v)
{
	if (v) {
		u32 mp = bus_highspeed() ? 512 : 64;
		g_qh[7].config = (mp << 16) | CONFIG_ZLT;
		wr(USB_BASE + USB_ENDPTCTRL(3),
		   rd(USB_BASE + USB_ENDPTCTRL(3)) |
		   CTRL_TXE | CTRL_TXR | CTRL_TXT_BULK);
		barrier();
		g_configured = 1;
		log_printf("usb: configured, %s speed, ep3 maxpkt %u\n",
			   mp == 512 ? "high" : "full", mp);
	} else {
		wr(USB_BASE + USB_ENDPTCTRL(3), 0);
		g_configured = 0;
		g_tx_busy = 0;
	}
	g_config_value = v;
}

static void usb_handle_setup(void)
{
	u8 s[8];
	u8 type, req;
	u16 val, idx, len;

	memcpy(s, g_qh[0].setup, 8);
	wr(USB_BASE + USB_ENDPTSETUPSTAT, EPT_RX(0));

	type = s[0];
	req = s[1];
	val = (u16)(s[2] | (s[3] << 8));
	idx = (u16)(s[4] | (s[5] << 8));
	len = (u16)(s[6] | (s[7] << 8));
	log_printf("setup %x %x %x %x %x\n", type, req, val, idx, len);

	/* CDC class requests (host -> device except GET_LINE_CODING) */
	if ((type & 0x60) == 0x20) {
		if (!(type & 0x80)) {
			if (len)
				ep0_recv(len);
			else
				ep0_status_in();
			/* SET_LINE_CODING means a tty was just opened: replay
			 * the boot facts in case the early log was lost. */
			if (req == 0x20)
				boot_replay();
		} else if (req == 0x21) {
			static const u8 lc[7] = {
				0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08
			};
			ep0_send(lc, sizeof(lc), len);
		} else {
			ep0_stall();
		}
		return;
	}

	if ((type & 0x60) != 0) {
		/* vendor requests: ack, nobody sends any we need */
		ep0_status_in();
		return;
	}

	switch (req) {
	case 0x00: {	/* GET_STATUS (device, interface, endpoint) */
		static const u8 z[2] = { 0, 0 };
		ep0_send(z, 2, len);
		return;
	}
	case 0x01:	/* CLEAR_FEATURE */
		if ((type & 0x1f) == 2 && (idx & 0x80) && (idx & 0xf) == 3) {
			u32 c = rd(USB_BASE + USB_ENDPTCTRL(3));
			if (c & CTRL_TXS)
				wr(USB_BASE + USB_ENDPTCTRL(3),
				   c | CTRL_TXS);
		}
		ep0_status_in();
		return;
	case 0x03:	/* SET_FEATURE */
		ep0_status_in();
		return;
	case 0x05:	/* SET_ADDRESS */
		wr(USB_BASE + USB_DEVICEADDR, ((u32)val << 25) | (1u << 24));
		ep0_status_in();
		return;
	case 0x06:	/* GET_DESCRIPTOR */
		break;
	case 0x08:	/* GET_CONFIGURATION */
		if (val == 0 && idx == 0 && len == 1) {
			u8 v = (u8)g_config_value;
			ep0_send(&v, 1, 1);
		} else {
			ep0_stall();
		}
		return;
	case 0x09:	/* SET_CONFIGURATION */
		usb_set_config(val);
		ep0_status_in();
		return;
	case 0x0a: {	/* GET_INTERFACE */
		static const u8 z = 0;
		ep0_send(&z, 1, len);
		return;
	}
	default:
		ep0_stall();
		return;
	}

	/* GET_DESCRIPTOR */
	{
		u32 dtype = val >> 8, dnum = val & 0xff;
		switch (dtype) {
		case 0x01:
			ep0_send(g_dev_desc, sizeof(g_dev_desc), len);
			return;
		case 0x02:
			patch_config_desc();
			ep0_send(g_config_desc, sizeof(g_config_desc), len);
			return;
		case 0x06:
			ep0_send(g_qual_desc, sizeof(g_qual_desc), len);
			return;
		case 0x03:
			if (dnum == 0) {
				ep0_send(g_str_lang, sizeof(g_str_lang), len);
			} else if (dnum == 1) {
				ep0_send(g_str_mfr, g_str_mfr_len, len);
			} else if (dnum == 2) {
				ep0_send(g_str_prod, g_str_prod_len, len);
			} else if (dnum == 3) {
				ep0_send(g_str_ser, g_str_ser_len, len);
			} else {
				ep0_stall();
			}
			return;
		default:
			ep0_stall();
			return;
		}
	}
}

static void usb_bus_reset(void)
{
	wr(USB_BASE + USB_ENDPTCOMPLETE,
	   rd(USB_BASE + USB_ENDPTCOMPLETE));
	wr(USB_BASE + USB_ENDPTSETUPSTAT,
	   rd(USB_BASE + USB_ENDPTSETUPSTAT));
	wr(USB_BASE + USB_ENDPTFLUSH, 0xffffffff);
	wait_clear(USB_BASE + USB_ENDPTFLUSH, 0xffffffff, 100);
	wr(USB_BASE + USB_ENDPTCTRL(2), 0);
	wr(USB_BASE + USB_ENDPTCTRL(3), 0);
	g_configured = 0;
	g_config_value = 0;
	g_tx_busy = 0;
	g_ep0_state = EP0_IDLE;
	log_printf("usb: bus reset\n");
}

static void usb_complete(u32 cpl)
{
	if (cpl & EPT_TX(0)) {
		if (g_ep0_state == EP0_DATA_IN) {
			ep0_status_out();
		} else if (g_ep0_state == EP0_STATUS_IN) {
			g_ep0_state = EP0_IDLE;
		}
	}
	if (cpl & EPT_RX(0)) {
		if (g_ep0_state == EP0_STATUS_OUT) {
			g_ep0_state = EP0_IDLE;
		} else if (g_ep0_state == EP0_DATA_OUT) {
			ep0_status_in();
		}
	}
	if (cpl & EPT_TX(3)) {
		u32 info = g_td_ep3.info;
		if (info & 0xff)
			log_printf("usb: tx3 error %x\n", info & 0xff);
		g_tx_busy = 0;
	}
}

static void usb_poll(void)
{
	u32 sts = rd(USB_BASE + USB_USBSTS);
	u32 setup, cpl;

	if (!sts)
		return;
	wr(USB_BASE + USB_USBSTS, sts);

	if (sts & STS_URI)
		usb_bus_reset();
	if (!(sts & (STS_UI | STS_UEI | STS_PCI)))
		return;

	setup = rd(USB_BASE + USB_ENDPTSETUPSTAT);
	if (setup & EPT_RX(0))
		usb_handle_setup();

	cpl = rd(USB_BASE + USB_ENDPTCOMPLETE);
	if (cpl) {
		wr(USB_BASE + USB_ENDPTCOMPLETE, cpl);
		usb_complete(cpl);
	}
}

static void usb_pump(void)
{
	u32 n;

	if (!g_configured)
		return;
	if (g_tx_busy) {
		if (elapsed_ms(g_tx_start) > 5000) {
			static u32 retries;
			u32 c;

			/* The host may simply not be reading yet (tty closed).
			 * Flush the stuck transfer but keep the chunk: clear
			 * the endpoint status and prime the very same data
			 * again instead of dropping it. */
			wr(USB_BASE + USB_ENDPTFLUSH, EPT_TX(3));
			wait_clear(USB_BASE + USB_ENDPTFLUSH, EPT_TX(3), 100);
			c = rd(USB_BASE + USB_ENDPTCTRL(3));
			if (c & CTRL_TXS)
				wr(USB_BASE + USB_ENDPTCTRL(3), c);
			c = rd(USB_BASE + USB_ENDPTCOMPLETE);
			if (c & EPT_TX(3))
				wr(USB_BASE + USB_ENDPTCOMPLETE, EPT_TX(3));
			qh_prime(3, 1, &g_td_ep3, g_tx_buf, g_tx_len);
			g_tx_start = ticks();
			if (retries++ < 4)
				log_printf("usb: tx3 stalled, retrying\n");
		}
		return;
	}
	n = ring_read(g_tx_buf, sizeof(g_tx_buf));
	if (!n)
		return;
	g_tx_len = n;
	qh_prime(3, 1, &g_td_ep3, g_tx_buf, n);
	g_tx_busy = 1;
	g_tx_start = ticks();
}

/* ---------------- clocks / platform ---------------- */

#define GCC_USB_HS_BCR			0x01841000u
#define GCC_USB_HS_SYSTEM_CBCR		0x01841004u
#define GCC_USB_HS_AHB_CBCR		0x01841008u
#define GCC_USB_HS_SYSTEM_CMD_RCGR	0x01841010u
#define GCC_USB_HS_SYSTEM_CFG_RCGR	0x01841014u

static void branch_enable(u64 cbcr)
{
	wr(cbcr, rd(cbcr) | 1u);
	wait_clear(cbcr, 1u << 31, 100);
}

static void usb_clocks_init(void)
{
	u32 v;

	/* usb_hs_system_src: 80 MHz = GPLL0 (800 MHz) / 10 */
	v = rd(GCC_USB_HS_SYSTEM_CFG_RCGR);
	v &= ~((0x7u << 8) | 0x1fu);
	v |= (1u << 8) | 19u;
	wr(GCC_USB_HS_SYSTEM_CFG_RCGR, v);
	wr(GCC_USB_HS_SYSTEM_CMD_RCGR,
	   rd(GCC_USB_HS_SYSTEM_CMD_RCGR) | 1u);
	wait_clear(GCC_USB_HS_SYSTEM_CMD_RCGR, 1u, 100);

	/* LK's hsusb_clock_init: enable, disable, reset the block, enable */
	branch_enable(GCC_USB_HS_AHB_CBCR);
	branch_enable(GCC_USB_HS_SYSTEM_CBCR);
	delay_ms(20);
	wr(GCC_USB_HS_AHB_CBCR, rd(GCC_USB_HS_AHB_CBCR) & ~1u);
	wr(GCC_USB_HS_SYSTEM_CBCR, rd(GCC_USB_HS_SYSTEM_CBCR) & ~1u);
	delay_ms(20);
	wr(GCC_USB_HS_BCR, 1);
	delay_ms(20);
	wr(GCC_USB_HS_BCR, 0);
	delay_ms(20);
	branch_enable(GCC_USB_HS_AHB_CBCR);
	branch_enable(GCC_USB_HS_SYSTEM_CBCR);
	delay_ms(20);

	log_printf("usb: clk cfg=%x cmd=%x sys=%x ahb=%x\n",
		   rd(GCC_USB_HS_SYSTEM_CFG_RCGR),
		   rd(GCC_USB_HS_SYSTEM_CMD_RCGR),
		   rd(GCC_USB_HS_SYSTEM_CBCR),
		   rd(GCC_USB_HS_AHB_CBCR));
}

static void wdt_disable_and_log(void)
{
	u32 base = 0x0b017000;

	g_boot_wdt_en = rd(base + 0x08);
	log_printf("wdt: 00=%x rst=%x en=%x sts=%x bark=%x bite=%x\n",
		   rd(base + 0x00), rd(base + 0x04), rd(base + 0x08),
		   rd(base + 0x0c), rd(base + 0x10), rd(base + 0x14));
	wr(base + 0x04, 1);	/* kick */
	wr(base + 0x08, 0);	/* disable */
	barrier();
	log_printf("wdt: disabled en=%x\n", rd(base + 0x08));
}

static void gic_disable_and_log(void)
{
	u32 dist = 0x0b000000, cpu = 0x0b002000;
	int i;

	log_printf("gic: ctlr=%x cpu=%x, disabling\n", rd(dist), rd(cpu));
	wr(dist, 0);
	wr(cpu, 0);
	for (i = 0; i < 32; i++) {
		wr(dist + 0x180 + 4 * i, 0xffffffff);
		wr(dist + 0x280 + 4 * i, 0xffffffff);
	}
	barrier();
}

static void usb_hw_init(void)
{
	wr(USB_BASE + USB_USBCMD, USBCMD_ITC8_RST);
	delay_ms(20);
	wait_clear(USB_BASE + USB_USBCMD, 2, 100);

	wr(USB_BASE + USB_PORTSC, 0x80000000);	/* PTS = ULPI */

	/* external VBUS valid, like LK's target_usb_init */
	ulpi_write(ULPI_MISC_A_SET,
		   ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
	wr(USB_BASE + USB_GENCONFIG_2,
	   rd(USB_BASE + USB_GENCONFIG_2) | (1u << 7));
	wr(USB_BASE + USB_USBCMD, rd(USB_BASE + USB_USBCMD) | (1u << 25));

	wr(USB_BASE + USB_SBUSCFG, 0);
	wr(USB_BASE + USB_AHB_MODE, 0x08);

	memset(g_qh, 0, sizeof(g_qh));
	barrier();
	wr(USB_BASE + USB_ENDPOINTLISTADDR, (u32)(u64)g_qh);
	wr(USB_BASE + USB_USBMODE, 2);

	wr(USB_BASE + USB_ENDPTFLUSH, 0xffffffff);
	wait_clear(USB_BASE + USB_ENDPTFLUSH, 0xffffffff, 100);

	g_qh[0].config = (64u << 16) | CONFIG_ZLT | CONFIG_IOS;
	g_qh[1].config = (64u << 16) | CONFIG_ZLT;

	wr(USB_BASE + USB_USBSTS, 0xffffffff);
	wr(USB_BASE + USB_USBINTR, 0);

	log_printf("usb: id %x:%x misc_a %x mode %x caplen %x\n",
		   ulpi_read(0x00) | (ulpi_read(0x01) << 8),
		   ulpi_read(0x02) | (ulpi_read(0x03) << 8),
		   ulpi_read(ULPI_MISC_A), rd(USB_BASE + USB_USBMODE),
		   rd(USB_BASE + USB_CAPLENGTH) & 0xff);
	log_printf("usb: usbcmd=%x portsc=%x hcsp=%x\n",
		   rd(USB_BASE + USB_USBCMD), rd(USB_BASE + USB_PORTSC),
		   rd(USB_BASE + USB_HCSPARAMS));
}

static void usb_attach(void)
{
	wr(USB_BASE + USB_USBCMD, rd(USB_BASE + USB_USBCMD) | USBCMD_ITC8_RS);
	log_printf("usb: attached usbcmd=%x\n", rd(USB_BASE + USB_USBCMD));
}

/* keep the console moving while a long polled operation runs; if the step
 * hangs the CPU, everything logged so far is already on the wire */
static void probe_drain(void)
{
	u32 s = ticks();

	if (g_tick_ms) {
		while (elapsed_ms(s) < 30) {
			usb_poll();
			usb_pump();
		}
	} else {
		volatile u32 n = 200000;

		while (n--) {
			usb_poll();
			usb_pump();
		}
	}
}

/* ---------------- SDHCI / eMMC read-only probe ---------------- */

#define SDHCI_BASE		0x07824900u
#define SDCC_CORE		0x07824000u

#define GCC_SDCC1_BCR		0x01842000u
#define GCC_SDCC1_CMD_RCGR	0x01842004u
#define GCC_SDCC1_CFG_RCGR	0x01842008u
#define GCC_SDCC1_M		0x0184200cu
#define GCC_SDCC1_N		0x01842010u
#define GCC_SDCC1_D		0x01842014u
#define GCC_SDCC1_APPS_CBCR	0x01842018u
#define GCC_SDCC1_AHB_CBCR	0x0184201cu

#define SDHCI_BLKSZ		0x004
#define SDHCI_BLK_CNT		0x006
#define SDHCI_ARGUMENT		0x008
#define SDHCI_TRANS_MODE	0x00c
#define SDHCI_CMD		0x00e
#define SDHCI_RESP		0x010
#define SDHCI_PRESENT_STATE	0x024
#define SDHCI_HOST_CTRL1	0x028
#define SDHCI_PWR_CTRL		0x029
#define SDHCI_CLK_CTRL		0x02c
#define SDHCI_TIMEOUT_REG	0x02e
#define SDHCI_RESET_REG		0x02f
#define SDHCI_NRML_INT_STS	0x030
#define SDHCI_ERR_INT_STS	0x032
#define SDHCI_NRML_INT_EN	0x034
#define SDHCI_ERR_INT_EN	0x036
#define SDHCI_NRML_INT_SIG	0x038
#define SDHCI_ERR_INT_SIG	0x03a
#define SDHCI_HOST_CTRL2	0x03e
#define SDHCI_CAPS1		0x040
#define SDHCI_CAPS2		0x044
#define SDHCI_ADMA_ADDR		0x058
#define SDHCI_VENDOR_SPEC	0x10c
#define SDHCI_VENDOR_CAPS0	0x11c

#define SDCC_MCI_VERSION	0x050
#define SDCC_MCI_HC_MODE	0x078
#define SDCC_PWRCTL_STATUS	0x0dc
#define SDCC_PWRCTL_MASK	0x0e0
#define SDCC_PWRCTL_CLEAR	0x0e4
#define SDCC_PWRCTL_CTL		0x0e8

struct adma2_desc {
	u16 attr;
	u16 len;
	u32 addr;
};

static struct adma2_desc g_adma[8] __attribute__((aligned(64)));
static u8 g_emmc_blk[1024] __attribute__((aligned(64)));
static u8 g_ext_csd[512] __attribute__((aligned(64)));
static u8 g_gpt_hdr[512] __attribute__((aligned(64)));
static u8 g_gpt_ent[16384] __attribute__((aligned(64)));
static u32 g_sd_resp[4];
static u32 g_emmc_fast;		/* 1 = 8-bit, 2 = high speed, 4 = 25 MHz */

static u32 wait8_clear(u64 addr, u8 mask, u32 ms)
{
	u32 s = ticks();
	volatile u32 n = ms * g_spin_budget;

	for (;;) {
		if (!(rd8(addr) & mask))
			return 0;
		if (g_tick_ms) {
			if (elapsed_ms(s) >= ms)
				return 1;
		} else if (!n) {
			return 1;
		} else {
			n--;
		}
	}
}

static void sdcc_pwr_irq_ack(void)
{
	u32 st = rd(SDCC_CORE + SDCC_PWRCTL_STATUS);
	u32 ctl;

	if (!st)
		return;
	wr(SDCC_CORE + SDCC_PWRCTL_CLEAR, st);
	ctl = rd(SDCC_CORE + SDCC_PWRCTL_CTL);
	if (st & 3)
		ctl |= 1;
	if (st & 0xc)
		ctl |= 4;
	wr(SDCC_CORE + SDCC_PWRCTL_CTL, ctl);
}

static void sdcc_err_reset(void)
{
	wr8(SDHCI_BASE + SDHCI_RESET_REG, 0x06);
	wait8_clear(SDHCI_BASE + SDHCI_RESET_REG, 0x06, 100);
}

static u32 sdcc_wait_idle(u32 ms)
{
	u32 s = ticks();
	volatile u32 n = ms * g_spin_budget;

	while (rd(SDHCI_BASE + SDHCI_PRESENT_STATE) & 3) {
		if (g_tick_ms) {
			if (elapsed_ms(s) >= ms)
				return 1;
		} else if (!n) {
			return 1;
		} else {
			n--;
		}
	}
	return 0;
}

/*
 * kind: 0 = no response, 1 = R2 (136 bit, CRC),
 *       2 = 48 bit with CRC+index checks (R1/R6/R7), 3 = 48 bit R3,
 *       4 = 48 bit with busy (R1b, CMD6)
 */
static u32 sdcc_cmd(u32 idx, u32 arg, u32 kind)
{
	u32 flags, st, s;
	volatile u32 n;

	if (sdcc_wait_idle(100))
		return 0x100;

	wr16(SDHCI_BASE + SDHCI_NRML_INT_STS, 0xffff);
	wr16(SDHCI_BASE + SDHCI_ERR_INT_STS, 0xffff);
	wr8(SDHCI_BASE + SDHCI_TIMEOUT_REG, 0x0f);
	wr(SDHCI_BASE + SDHCI_ARGUMENT, arg);

	if (kind == 1)
		flags = 1 | (1u << 3);
	else if (kind == 2)
		flags = 2 | (1u << 3) | (1u << 4);
	else if (kind == 3)
		flags = 2;
	else if (kind == 4)
		flags = 3 | (1u << 3) | (1u << 4);
	else
		flags = 0;
	wr16(SDHCI_BASE + SDHCI_CMD, (u16)((idx << 8) | flags));

	s = ticks();
	n = 1000 * g_spin_budget;
	for (;;) {
		st = rd16(SDHCI_BASE + SDHCI_NRML_INT_STS);
		if (st & 1u)
			break;
		if (rd16(SDHCI_BASE + SDHCI_ERR_INT_STS) || (st & 0x8000u)) {
			u32 e = rd16(SDHCI_BASE + SDHCI_ERR_INT_STS);
			log_printf("emmc: cmd%u err=%x\n", idx, e);
			sdcc_err_reset();
			return 0x200 | (e & 0xff);
		}
		if (g_tick_ms) {
			if (elapsed_ms(s) >= 1000)
				goto timeout;
		} else if (!n--) {
			goto timeout;
		}
	}
	wr16(SDHCI_BASE + SDHCI_NRML_INT_STS, 1);

	if (kind == 1) {
		int i;
		for (i = 0; i < 4; i++) {
			g_sd_resp[i] =
				rd(SDHCI_BASE + SDHCI_RESP + 4 * i) << 8;
			if (i)
				g_sd_resp[i] |=
					rd(SDHCI_BASE + SDHCI_RESP + 4 * (i - 1)) >> 24;
		}
	} else if (kind) {
		g_sd_resp[0] = rd(SDHCI_BASE + SDHCI_RESP);
	}
	return 0;

timeout:
	log_printf("emmc: cmd%u timeout st=%x\n", idx,
		   rd16(SDHCI_BASE + SDHCI_NRML_INT_STS));
	sdcc_err_reset();
	return 0x300;
}

/* chunked ADMA2 table, at most 8 x 32 KiB */
static u32 sdcc_adma_setup(void *buf, u32 bytes)
{
	u32 left = bytes, i = 0, len;
	u8 *p = buf;

	while (left) {
		len = left > 32768u ? 32768u : left;
		if (i == 8)
			return 1;
		g_adma[i].attr = (u16)(0x0021 | (left == len ? 0x0002 : 0));
		g_adma[i].len = (u16)len;
		g_adma[i].addr = (u32)(u64)p;
		left -= len;
		p += len;
		i++;
	}
	barrier();
	return 0;
}

/*
 * Data read: idx 17 (single block, arg = lba) or 18 (multi block,
 * arg = lba, preceded by CMD23 so the card stops after n blocks).
 */
static u32 sdcc_read_data(u32 idx, u32 arg, void *buf, u32 nblk)
{
	u32 st = 0, s;
	volatile u32 n;
	u32 done_data = 0;

	if (sdcc_wait_idle(100))
		return 0x100;

	wr16(SDHCI_BASE + SDHCI_NRML_INT_STS, 0xffff);
	wr16(SDHCI_BASE + SDHCI_ERR_INT_STS, 0xffff);
	if (sdcc_adma_setup(buf, nblk * 512))
		return 0x101;

	wr(SDHCI_BASE + SDHCI_ADMA_ADDR, (u32)(u64)&g_adma);
	wr16(SDHCI_BASE + SDHCI_BLKSZ, 512);
	wr16(SDHCI_BASE + SDHCI_BLK_CNT, (u16)nblk);
	wr8(SDHCI_BASE + SDHCI_TIMEOUT_REG, 0x0f);

	if (nblk > 1) {
		u32 r;

		log_printf("emmc: multi %u blk @ %x\n", nblk, arg);
		probe_drain();
		r = sdcc_cmd(23, nblk, 2);
		if (r) {
			log_printf("emmc: cmd23 %u\n", r);
			return 0x400 | r;
		}
	}

	wr(SDHCI_BASE + SDHCI_ARGUMENT, arg);
	wr16(SDHCI_BASE + SDHCI_TRANS_MODE,
	     (u16)(nblk > 1 ? 0x0033 : 0x0011));	/* DMA | count | read | multi */
	wr16(SDHCI_BASE + SDHCI_CMD,
	     (u16)((idx << 8) | 2 | (1u << 3) | (1u << 4) | (1u << 5)));

	s = ticks();
	n = 2000 * g_spin_budget;
	for (;;) {
		u32 err = rd16(SDHCI_BASE + SDHCI_ERR_INT_STS);

		st = rd16(SDHCI_BASE + SDHCI_NRML_INT_STS);
		if (err || (st & 0x8000u)) {
			log_printf("emmc: read err=%x st=%x\n", err, st);
			sdcc_err_reset();
			return 0x200 | (err & 0xff);
		}
		if (st & 1u)
			wr16(SDHCI_BASE + SDHCI_NRML_INT_STS, 1);
		if (st & 2u) {
			wr16(SDHCI_BASE + SDHCI_NRML_INT_STS, 2);
			done_data = 1;
			break;
		}
		if (g_tick_ms) {
			if (elapsed_ms(s) >= 2000)
				break;
		} else if (!n--) {
			break;
		}
	}
	if (!done_data) {
		log_printf("emmc: read timeout st=%x\n", st);
		sdcc_err_reset();
		return 0x300;
	}
	return 0;
}

static u32 sdcc_read_block(u32 lba, void *buf)
{
	return sdcc_read_data(17, lba, buf, 1);
}

/* base clock 50 MHz, sclk = 50 MHz / (2 * div) */
static void sdcc_set_clock(u32 div)
{
	u32 s;
	volatile u32 n;

	wr16(SDHCI_BASE + SDHCI_CLK_CTRL, 0);
	n = g_spin_budget;
	while (n--)
		;
	wr16(SDHCI_BASE + SDHCI_CLK_CTRL, (u16)((div << 8) | 1u));
	s = ticks();
	n = 100 * g_spin_budget;
	while (!(rd16(SDHCI_BASE + SDHCI_CLK_CTRL) & 2)) {
		if (g_tick_ms) {
			if (elapsed_ms(s) >= 100)
				break;
		} else if (!n) {
			break;
		} else {
			n--;
		}
	}
	wr16(SDHCI_BASE + SDHCI_CLK_CTRL,
	     (u16)(rd16(SDHCI_BASE + SDHCI_CLK_CTRL) | 4));
}

/* CMD6 write-byte switch, then CMD13 until the card is back in TRAN */
static u32 emmc_switch(u32 index, u32 value)
{
	u32 r, i, st = 0;

	log_printf("emmc: sw %u=%u\n", index, value);
	probe_drain();
	r = sdcc_cmd(6, (3u << 24) | (index << 16) | (value << 8), 4);
	if (r) {
		log_printf("emmc: cmd6 idx=%u val=%u -> %u\n", index, value, r);
		return r;
	}
	sdcc_wait_idle(500);
	for (i = 0; i < 20; i++) {
		r = sdcc_cmd(13, 2u << 16, 2);
		if (!r) {
			st = (g_sd_resp[0] >> 9) & 0xfu;
			if (st == 4)
				return 0;
		}
		delay_ms(1);
	}
	log_printf("emmc: switch idx=%u state=%x r=%u\n", index, st, r);
	return 0x500 | st;
}

/* LBA0 read must come back with the 0xAA55 signature */
static u32 emmc_verify(u8 *blk)
{
	u32 r = sdcc_read_block(0, blk);

	return r || blk[510] != 0x55 || blk[511] != 0xaa;
}

/*
 * Read EXT_CSD (CMD8), try 8-bit bus + high speed like stock LK does
 * (LK does not consult the card's BUS_WIDTH), raise the clock to 25 MHz
 * and verify with a fresh LBA0 read. Unwind one step at a time on
 * failure, verifying after each, down to 1-bit SDR at 390 kHz.
 */
static u32 emmc_fast(u8 *blk)
{
	u32 r, widths, dev_type;

	log_puts("emmc: ext_csd read\n");
	probe_drain();
	r = sdcc_read_data(8, 0, g_ext_csd, 1);
	if (r) {
		log_printf("emmc: ext_csd %u\n", r);
		probe_drain();
		return 1;
	}
	widths = g_ext_csd[183];
	dev_type = g_ext_csd[196] | (g_ext_csd[197] << 8);
	log_printf("emmc: bus_w=%x dev_type=%x hs_timing=%x rev=%u\n",
		   widths, dev_type, g_ext_csd[185], g_ext_csd[192]);
	log_printf("emmc: ecsd[176..191]=%x %x %x %x\n",
		   *(u32 *)(g_ext_csd + 176), *(u32 *)(g_ext_csd + 180),
		   *(u32 *)(g_ext_csd + 184), *(u32 *)(g_ext_csd + 188));
	log_printf("emmc: sec_count=%x\n", *(u32 *)(g_ext_csd + 212));
	probe_drain();

	r = emmc_switch(183, 2);
	log_printf("emmc: 8bit %u\n", r);
	probe_drain();
	if (!r) {
		wr8(SDHCI_BASE + SDHCI_HOST_CTRL1,
		    (u8)((rd8(SDHCI_BASE + SDHCI_HOST_CTRL1) & ~2u) | 0x20u));
		g_emmc_fast |= 1;
	}

	r = emmc_switch(185, 1);
	log_printf("emmc: hs %u\n", r);
	probe_drain();
	if (!r) {
		log_puts("emmc: raise clk\n");
		probe_drain();
		wr8(SDHCI_BASE + SDHCI_HOST_CTRL1,
		    (u8)(rd8(SDHCI_BASE + SDHCI_HOST_CTRL1) | 4u));
		sdcc_set_clock(1);
		g_emmc_fast |= 4;
		log_printf("emmc: clk ctl=%x host1=%x\n",
			   rd16(SDHCI_BASE + SDHCI_CLK_CTRL),
			   rd8(SDHCI_BASE + SDHCI_HOST_CTRL1));
		probe_drain();
	}

	r = emmc_verify(blk);
	if (!r) {
		log_printf("emmc: fast ok %x\n", g_emmc_fast);
		probe_drain();
		return 0;
	}
	log_printf("emmc: fast verify failed %u\n", r);
	probe_drain();

	if (g_emmc_fast & 1) {		/* step back to 1-bit, keep HS */
		emmc_switch(183, 1);
		wr8(SDHCI_BASE + SDHCI_HOST_CTRL1,
		    (u8)(rd8(SDHCI_BASE + SDHCI_HOST_CTRL1) & ~0x20u));
		g_emmc_fast &= ~1u;
		r = emmc_verify(blk);
		if (!r) {
			log_printf("emmc: 1bit ok %x\n", g_emmc_fast);
			probe_drain();
			return 0;
		}
	}

	if (g_emmc_fast & 4) {		/* drop HS and the clock */
		sdcc_set_clock(64);
		emmc_switch(185, 0);
		wr8(SDHCI_BASE + SDHCI_HOST_CTRL1,
		    (u8)(rd8(SDHCI_BASE + SDHCI_HOST_CTRL1) & ~4u));
		g_emmc_fast &= ~4u;
		r = emmc_verify(blk);
		if (!r) {
			log_printf("emmc: slow ok %x\n", g_emmc_fast);
			probe_drain();
			return 0;
		}
	}

	sdcc_set_clock(64);
	wr8(SDHCI_BASE + SDHCI_HOST_CTRL1,
	    (u8)((rd8(SDHCI_BASE + SDHCI_HOST_CTRL1) & ~(0x22u)) | 0x10u));
	g_emmc_fast = 0;
	r = sdcc_read_block(0, blk);
	log_printf("emmc: slow retry %u sig=%x\n",
		   r, blk[510] | (blk[511] << 8));
	probe_drain();
	return r || blk[510] != 0x55 || blk[511] != 0xaa;
}

/* read the GPT entry array (CMD23 + CMD18) and list used partitions */
static void gpt_scan(void)
{
	u32 r, i, n_ent, e_sz, blocks, count = 0;
	u64 ent_lba;
	u32 *w = (u32 *)g_gpt_hdr;

	r = sdcc_read_block(1, g_gpt_hdr);
	if (r) {
		log_printf("gpt: hdr read %u\n", r);
		return;
	}
	if (w[0] != 0x20494645u || w[1] != 0x54524150u) {
		log_puts("gpt: bad hdr\n");
		return;
	}
	ent_lba = (u64)w[18] | ((u64)w[19] << 32);
	n_ent = w[20];
	e_sz = w[21];
	log_printf("gpt: entries lba=%lx n=%u esz=%u\n", ent_lba, n_ent, e_sz);
	probe_drain();
	if (!n_ent || e_sz != 128) {
		log_puts("gpt: unexpected layout\n");
		return;
	}
	if (n_ent > sizeof(g_gpt_ent) / e_sz)
		n_ent = sizeof(g_gpt_ent) / e_sz;
	if (ent_lba > 0xffffffffull) {
		log_puts("gpt: entries beyond 32-bit lba\n");
		return;
	}

	blocks = (n_ent * e_sz + 511) / 512;
	log_printf("gpt: reading %u blocks\n", blocks);
	probe_drain();
	r = sdcc_read_data(18, (u32)ent_lba, g_gpt_ent, blocks);
	log_printf("gpt: read %u blocks -> %u\n", blocks, r);
	probe_drain();
	if (r)
		return;

	for (i = 0; i < n_ent; i++) {
		u8 *e = g_gpt_ent + i * e_sz;
		u32 j, first, last, nz = 0;
		char name[25];

		for (j = 0; j < 16; j++)
			nz |= e[j];
		if (!nz)
			continue;
		first = *(u32 *)(e + 32);
		last = *(u32 *)(e + 40);
		for (j = 0; j < 24 && e[56 + j * 2]; j++) {
			u8 c = e[56 + j * 2];	/* UTF-16LE, ASCII in the low byte */

			name[j] = (c >= 0x20 && c <= 0x7e) ? c : '.';
		}
		name[j] = 0;
		log_printf("gpt: %u %s %x+%x\n", i, name, first,
			   last - first + 1);
		count++;
	}
	log_printf("gpt: %u partitions\n", count);
	probe_drain();
}

static void sdcc_host_init(void)
{
	u32 caps, ver, v;
	u8 volt = 0;

	/* interface (AHB) clock, then apps clock = GPLL0(800)/16 = 50 MHz */
	branch_enable(GCC_SDCC1_AHB_CBCR);
	wr(GCC_SDCC1_M, 0);
	wr(GCC_SDCC1_N, 0);
	wr(GCC_SDCC1_D, 0xffffffffu);
	v = rd(GCC_SDCC1_CFG_RCGR);
	v &= ~((0x7u << 8) | 0x1fu | (0x3u << 12));
	v |= (1u << 8) | 31u;
	wr(GCC_SDCC1_CFG_RCGR, v);
	wr(GCC_SDCC1_CMD_RCGR, rd(GCC_SDCC1_CMD_RCGR) | 1u);
	wait_clear(GCC_SDCC1_CMD_RCGR, 1u, 100);
	branch_enable(GCC_SDCC1_APPS_CBCR);
	log_printf("emmc: clk bcr=%x cfg=%x cmd=%x apps=%x ahb=%x\n",
		   rd(GCC_SDCC1_BCR), rd(GCC_SDCC1_CFG_RCGR),
		   rd(GCC_SDCC1_CMD_RCGR), rd(GCC_SDCC1_APPS_CBCR),
		   rd(GCC_SDCC1_AHB_CBCR));

	wr(SDHCI_BASE + SDHCI_VENDOR_SPEC, 0xa1c);
	wr(SDCC_CORE + SDCC_MCI_HC_MODE,
	   rd(SDCC_CORE + SDCC_MCI_HC_MODE) | 1u | (1u << 13));

	wr8(SDHCI_BASE + SDHCI_RESET_REG, 1);
	if (wait8_clear(SDHCI_BASE + SDHCI_RESET_REG, 1, 100))
		log_puts("emmc: reset stuck\n");

	/* drop whatever the reset raised, then unmask the power irq */
	sdcc_pwr_irq_ack();
	wr(SDCC_CORE + SDCC_PWRCTL_MASK, 0xf);

	ver = rd(SDCC_CORE + SDCC_MCI_VERSION);
	caps = rd(SDHCI_BASE + SDHCI_CAPS1);
	log_printf("emmc: ver=%x caps=%x cap2=%x\n",
		   ver, caps, rd(SDHCI_BASE + SDHCI_CAPS2));

	if ((ver >> 28) >= 1 && (ver & 0xff) != 0x11 && (ver & 0xff) != 0x12)
		wr(SDHCI_BASE + SDHCI_VENDOR_CAPS0, caps | (1u << 26));

	if (caps & (1u << 24))
		volt = 7;
	else if (caps & (1u << 25))
		volt = 6;
	else if (caps & (1u << 26))
		volt = 5;

	wr8(SDHCI_BASE + SDHCI_PWR_CTRL, (u8)(volt << 1));
	wr8(SDHCI_BASE + SDHCI_PWR_CTRL, (u8)((volt << 1) | 1));
	{
		int i;
		for (i = 0; i < 8; i++) {
			delay_ms(1);
			sdcc_pwr_irq_ack();
		}
	}
	log_printf("emmc: pwr=%x status=%x\n",
		   rd8(SDHCI_BASE + SDHCI_PWR_CTRL),
		   rd(SDCC_CORE + SDCC_PWRCTL_STATUS));

	/* 1-bit bus, ADMA2 32-bit */
	wr8(SDHCI_BASE + SDHCI_HOST_CTRL1,
	    (u8)((rd8(SDHCI_BASE + SDHCI_HOST_CTRL1) & ~0x20u) | (1u << 4)));

	/* ~390 kHz: 50 MHz / (2 * 64) */
	sdcc_set_clock(64);
	log_printf("emmc: clk ctl=%x host1=%x\n",
		   rd16(SDHCI_BASE + SDHCI_CLK_CTRL),
		   rd8(SDHCI_BASE + SDHCI_HOST_CTRL1));

	wr16(SDHCI_BASE + SDHCI_NRML_INT_EN, 0x000b);
	wr16(SDHCI_BASE + SDHCI_ERR_INT_EN, 0xffff);
	wr16(SDHCI_BASE + SDHCI_NRML_INT_SIG, 0x000b);
	wr16(SDHCI_BASE + SDHCI_ERR_INT_SIG, 0xffff);
}

static void emmc_probe(void)
{
	u32 r, ocr = 0, i;
	u8 *blk = g_emmc_blk;

	log_puts("emmc: probe start\n");
	probe_drain();
	sdcc_host_init();
	probe_drain();

	r = sdcc_cmd(0, 0, 0);
	log_printf("emmc: cmd0 %u\n", r);
	probe_drain();
	if (r) {
		g_emmc_state = 1;
		return;
	}

	for (i = 0; i < 1000; i++) {
		r = sdcc_cmd(1, 0x40ff8000u, 3);
		if (r) {
			g_emmc_state = 2;
			log_printf("emmc: cmd1 %u\n", r);
			return;
		}
		ocr = g_sd_resp[0];
		if (ocr & 0x80000000u)
			break;
		delay_ms(1);
	}
	log_printf("emmc: cmd1 ocr=%x tries=%u\n", ocr, i);
	probe_drain();
	if (!(ocr & 0x80000000u)) {
		g_emmc_state = 3;
		return;
	}

	r = sdcc_cmd(2, 0, 1);
	log_printf("emmc: cmd2 %u\n", r);
	if (r) {
		g_emmc_state = 4;
		return;
	}
	{
		u8 pnm[7];
		u32 c0 = g_sd_resp[0], c1 = g_sd_resp[1], c2 = g_sd_resp[2];

		pnm[0] = (u8)c0;
		pnm[1] = (u8)(c1 >> 24);
		pnm[2] = (u8)(c1 >> 16);
		pnm[3] = (u8)(c1 >> 8);
		pnm[4] = (u8)c1;
		pnm[5] = (u8)(c2 >> 24);
		pnm[6] = 0;
		for (i = 0; i < 6; i++)
			if (pnm[i] < 0x20 || pnm[i] > 0x7e)
				pnm[i] = '.';
		log_printf("emmc: cid mid=%x oid=%x pnm=%s\n",
			   (c0 >> 24) & 0xff, (c0 >> 8) & 0xffff, pnm);
	}
	probe_drain();

	r = sdcc_cmd(3, 2u << 16, 2);
	log_printf("emmc: cmd3 %u resp=%x\n", r, g_sd_resp[0]);
	probe_drain();
	if (r) {
		g_emmc_state = 5;
		return;
	}

	r = sdcc_cmd(9, 2u << 16, 1);
	log_printf("emmc: cmd9 %u\n", r);
	probe_drain();
	if (r) {
		g_emmc_state = 6;
		return;
	}

	r = sdcc_cmd(7, 2u << 16, 2);
	log_printf("emmc: cmd7 %u resp=%x\n", r, g_sd_resp[0]);
	probe_drain();
	if (r) {
		g_emmc_state = 7;
		return;
	}

	r = sdcc_cmd(16, 512, 2);
	log_printf("emmc: cmd16 %u resp=%x\n", r, g_sd_resp[0]);
	probe_drain();
	if (r) {
		g_emmc_state = 8;
		return;
	}

	r = sdcc_read_block(0, blk);
	log_printf("emmc: lba0 %u\n", r);
	probe_drain();
	if (r) {
		g_emmc_state = 9;
		return;
	}
	{
		u32 *w = (u32 *)blk;

		log_printf("emmc: lba0 %x %x %x %x sig=%x ptype=%x\n",
			   w[0], w[1], w[2], w[3],
			   blk[510] | (blk[511] << 8), blk[0x1c2]);
	}
	probe_drain();

	r = sdcc_read_block(1, blk + 512);
	if (!r) {
		u32 *w = (u32 *)(blk + 512);

		log_printf("emmc: lba1 %x %x gpt=%s\n", w[0], w[1],
			   (w[0] == 0x20494645u && w[1] == 0x54524150u) ?
			   "yes" : "no");
	} else {
		log_printf("emmc: lba1 %u\n", r);
	}
	probe_drain();

	if (!emmc_fast(blk))
		gpt_scan();
	g_emmc_state = 0;
}

static void boot_replay(void)
{
	static u32 last;

	/* macOS re-sends SET_LINE_CODING several times around open */
	if (last && elapsed_ms(last) < 5000)
		return;
	last = ticks();

	log_printf("\n-- v8 dtb=%lx %s midr=%x cntfrq=%lx tick_ms=%u\n",
		   g_boot_dtb, g_boot_dtb_ok ? "ok" : "bad",
		   g_boot_midr, g_boot_cntfrq, g_tick_ms);
	log_printf("-- wdt_en=%x usb=%s emmc=%s fast=%x\n", g_boot_wdt_en,
		   g_configured ? (bus_highspeed() ? "hs" : "fs") : "off",
		   g_emmc_state == 0 ? "ok" : (g_emmc_state == 0xffff ?
					       "not probed" : "failed"),
		   g_emmc_fast);
}

/* run once, 20 s in, so the host can open the port and see the log live
 * even if a probe step hangs the CPU */
static void probe_tick(void)
{
	static u32 started, t0, spin;

	if (started)
		return;
	if (g_tick_ms) {
		if (!t0)
			t0 = ticks();
		if (elapsed_ms(t0) < 20000)
			return;
	} else {
		if (++spin < 0x8000000)
			return;
	}
	started = 1;
	probe_drain();
	emmc_probe();
}

/* ---------------- device tree dump ---------------- */

static u32 bswap32(u32 v)
{
	return (v >> 24) | ((v >> 8) & 0xff00) |
	       ((v << 8) & 0xff0000) | (v << 24);
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int str_prefix(const char *a, const char *b, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* bounded walk of the FDT LK hands over in x0; logs model, bootargs and
 * the patched /memory reg */
static void fdt_dump(u64 dtb)
{
	u32 *h = (u32 *)(u64)dtb;
	u32 magic = bswap32(h[0]);
	u32 total, off_struct, off_strings, version;
	const u8 *base = (const u8 *)(u64)dtb;
	const u8 *p, *end, *strs;
	const char *nodename = "";
	u32 nodename_len = 0;
	u32 depth = 0;

	log_printf("fdt: raw %x %x %x %x\n", h[0], h[1], h[2], h[3]);
	if (magic != 0xd00dfeedu) {
		log_printf("fdt: no fdt at %lx (magic %x)\n", dtb, magic);
		return;
	}
	total = bswap32(h[1]);
	off_struct = bswap32(h[2]);
	off_strings = bswap32(h[3]);
	version = bswap32(h[5]);
	log_printf("fdt: total=%u ver=%u struct=%x strings=%x\n",
		   total, version, off_struct, off_strings);
	if (total < 0x40 || total > 0x400000 ||
	    off_struct >= total || off_strings >= total)
		return;

	p = base + off_struct;
	end = base + total;
	strs = base + off_strings;

	while (p + 4 <= end) {
		u32 tag = bswap32(*(const u32 *)p);

		p += 4;
		if (tag == 1) {			/* FDT_BEGIN_NODE */
			nodename = (const char *)p;
			nodename_len = 0;
			while (p < end && *p) {
				p++;
				nodename_len++;
			}
			p++;
			p = (const u8 *)(((u64)p + 3) & ~3ull);
			depth++;
		} else if (tag == 2) {		/* FDT_END_NODE */
			if (depth)
				depth--;
		} else if (tag == 4) {		/* FDT_NOP */
			;
		} else if (tag == 9) {		/* FDT_END */
			break;
		} else if (tag == 3 && p + 8 <= end) {
			u32 len = bswap32(*(const u32 *)p);
			u32 nameoff = bswap32(*(const u32 *)(p + 4));
			const u8 *data = p + 8;
			const char *name;

			p = data + ((len + 3) & ~3u);
			if (p > end || off_strings + nameoff >= total)
				break;
			name = (const char *)(strs + nameoff);

			if (str_eq(name, "model")) {
				log_puts("fdt: model=");
				log_strn((const char *)data, len);
				log_putc('\n');
			} else if (str_eq(name, "bootargs")) {
				log_puts("fdt: bootargs=");
				log_strn((const char *)data, len);
				log_putc('\n');
			} else if (str_eq(name, "reg") && depth == 2 &&
				   nodename_len >= 6 &&
				   str_prefix(nodename, "memory", 6) &&
				   len == 16) {
				log_printf("fdt: memory %x+%x\n",
					   bswap32(*(const u32 *)(data + 4)),
					   bswap32(*(const u32 *)(data + 12)));
			}
		} else {
			break;
		}
	}
}

/* ---------------- main ---------------- */

void exc_report(u64 esr, u64 elr, u64 far)
{
	static volatile int busy;

	if (busy)
		for (;;)
			;
	busy = 1;
	log_printf("\n!!! exception esr=%lx elr=%lx far=%lx\n", esr, elr, far);
	if (g_configured) {
		u32 s = ticks();
		while (elapsed_ms(s) < 300) {
			usb_poll();
			usb_pump();
		}
	}
	for (;;)
		;
}

static void heartbeat(void)
{
	static u32 last, hb, spin;

	if (g_tick_ms) {
		if (elapsed_ms(last) < 1000)
			return;
		last = ticks();
	} else {
		if (++spin & 0xfffff)
			return;
	}
	log_printf("hb %u drops %u sts %x portsc %x\n", hb++,
		   g_ring_drops, rd(USB_BASE + USB_USBSTS),
		   rd(USB_BASE + USB_PORTSC));
}

static void status_tick(void)
{
	static u32 last, spin;

	if (!g_configured) {
		if (g_tick_ms) {
			if (elapsed_ms(last) < 5000)
				return;
			last = ticks();
		} else {
			if (++spin & 0x3fffff)
				return;
		}
		log_printf("usb: waiting sts=%x portsc=%x addr=%x\n",
			   rd(USB_BASE + USB_USBSTS),
			   rd(USB_BASE + USB_PORTSC),
			   rd(USB_BASE + USB_DEVICEADDR));
	}
}

void kmain(u64 dtb)
{
	u64 mpidr, midr, cntfrq;

	timer_init();

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));

	log_printf("\nios-sweet vivo Y21L stage0 v8: AArch64 payload\n");
	g_boot_dtb = dtb;
	g_boot_cntfrq = cntfrq;
	g_boot_midr = (u32)midr;
	g_boot_dtb_ok = 0;
	log_printf("dtb=%lx", dtb);
	if (dtb >= 0x80000000ull && dtb < 0xc0000000ull &&
	    *(volatile u32 *)(u64)dtb == 0xedfe0dd0u) {
		g_boot_dtb_ok = 1;
		log_puts(" magic ok\n");
	} else {
		log_puts(" magic bad\n");
	}
	log_printf("mpidr=%lx midr=%x cntfrq=%lx tick_ms=%u\n",
		   mpidr, (u32)midr, cntfrq, g_tick_ms);
	if (dtb >= 0x80000000ull && dtb < 0xc0000000ull)
		fdt_dump(dtb);

	build_string(g_str_mfr, &g_str_mfr_len, "ios-sweet");
	build_string(g_str_prod, &g_str_prod_len, "vivo Y21L stage0");
	build_string(g_str_ser, &g_str_ser_len, "Y21L-0001");

	wdt_disable_and_log();
	gic_disable_and_log();
	usb_clocks_init();
	usb_hw_init();
	usb_attach();

	log_printf("usb: waiting for host enumeration\n");

	for (;;) {
		usb_poll();
		usb_pump();
		probe_tick();
		heartbeat();
		status_tick();
	}
}

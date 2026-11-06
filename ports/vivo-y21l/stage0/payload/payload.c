/*
 * stage0 v5 AArch64 payload: polled ChipIdea USB device-mode console.
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
 * /dev/tty.usbmodemXXXX and `cat` shows the log.
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
			wr(USB_BASE + USB_ENDPTFLUSH, EPT_TX(3));
			wait_clear(USB_BASE + USB_ENDPTFLUSH, EPT_TX(3), 100);
			if (rd(USB_BASE + USB_ENDPTCTRL(3)) & CTRL_TXS)
				wr(USB_BASE + USB_ENDPTCTRL(3),
				   rd(USB_BASE + USB_ENDPTCTRL(3)));
			g_tx_busy = 0;
			log_printf("usb: tx3 timeout, flushed\n");
		}
		return;
	}
	n = ring_read(g_tx_buf, sizeof(g_tx_buf));
	if (!n)
		return;
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

	log_printf("\nios-sweet vivo Y21L stage0 v5: AArch64 payload\n");
	log_printf("dtb=%lx", dtb);
	if (dtb >= 0x80000000ull && dtb < 0xc0000000ull &&
	    *(volatile u32 *)(u64)dtb == 0xd00dfeedu)
		log_puts(" magic ok\n");
	else
		log_puts(" magic bad\n");
	log_printf("mpidr=%lx midr=%x cntfrq=%lx tick_ms=%u\n",
		   mpidr, (u32)midr, cntfrq, g_tick_ms);

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
		heartbeat();
		status_tick();
	}
}

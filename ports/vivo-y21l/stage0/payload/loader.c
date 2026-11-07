/*
 * SWEETXNU payload loader: the msm8916 counterpart of sweet's shim
 * (shim/loader.c). LK loads the boot.img ramdisk to 0x82000000, so the
 * payload mkpayload.py built sits there; this copies its segments to
 * phys_base + (vmaddr - virt_base), places the ADT and the arm64
 * boot_args behind the kernel, and jumps with MMU/caches off and
 * x0 = boot_args physical address.
 *
 * The test pipeline injects a stub "kernel" that writes qcom,pshold and
 * powers the phone off, so a power-off about a second after the loader
 * lines is the success signal.
 */
typedef unsigned int u32;
typedef unsigned long long u64;

#define RD_BASE		0x82000000ull
#define BOOT_LINE_LENGTH 608
#define PAGE_16K	0x4000ull

struct payload_seg {
	u64 vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	u32 version, nsegs;
	u64 virt_base, entry_va, adt_off, adt_size, kernel_end;
	u64 phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

struct boot_video {
	u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	unsigned short Revision, Version;
	u64 virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	u32 machineType;
	u64 deviceTreeP;
	u32 deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	u64 bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8,
	       "payload header layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

extern void log_puts(const char *s);
extern void log_printf(const char *fmt, ...);
extern void log_drain(u32 ms);

typedef void (*xnu_entry_t)(u64 boot_args_pa);

static u64 current_el(void)
{
	u64 v;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return v >> 2;
}

/* MMU off: everything is Device memory, so move aligned 8-byte words. */
static void copy64(u64 dst, u64 src, u64 n)
{
	volatile u64 *d = (volatile u64 *)dst;
	volatile const u64 *s = (volatile const u64 *)src;
	u64 i;

	for (i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(u64 dst, u64 n)
{
	volatile u64 *d = (volatile u64 *)dst;
	u64 i;

	for (i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	int i;

	for (i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

/* ---------------- previous xnu console (SHMCON) ---------------- */

static u32 rd32(u64 a) { return *(volatile u32 *)a; }
static u64 rd64(u64 a) { return *(volatile u64 *)a; }

#define SHMCON_MAGIC	0x53484d43u	/* 'SHMC' */
#define SHMCON_VERSION	2

/*
 * xnu's SHMCON console writes kprintf into the panic area at the top of
 * DRAM (the ADT "pram" node). With the window mapped as device memory the
 * bytes reach DRAM without cache maintenance, so a previous run's log can
 * be printed here before it gets overwritten. Offsets are the arm64 layout
 * of struct shmcon_header (sidx/midx are 64-byte aligned).
 */
static void dump_prev_shmcon(u64 pram_pa)
{
	u64 out_pa = rd64(pram_pa + 16);
	u32 out_len = rd32(pram_pa + 28);
	u32 head = rd32(pram_pa + 132);		/* midx[CBUF_OUT] */
	u32 tail = rd32(pram_pa + 68);		/* sidx[CBUF_OUT] */
	u32 n, i;

	if (rd32(pram_pa) != SHMCON_MAGIC ||
	    *(volatile unsigned char *)(pram_pa + 4) != SHMCON_VERSION) {
		log_printf("prev: no shmcon log (magic=%x)\n", rd32(pram_pa));
		return;
	}
	log_printf("prev: shmcon out=%lx len=%u head=%u tail=%u\n",
		   out_pa, out_len, head, tail);
	if (out_len < 1024 || out_len > 0x100000 || head >= out_len ||
	    tail >= out_len || out_pa < 0x80000000ull || out_pa >= 0xc0000000ull)
		return;
	n = (head + out_len - tail) % out_len;
	if (n > 8192) {
		/* keep the most recent bytes */
		tail = (head + out_len - 8192) % out_len;
		n = 8192;
	}
	log_puts("prev> ");
	for (i = 0; i < n; i++) {
		char c = *(volatile char *)(out_pa + (u64)((tail + i) % out_len));
		log_printf("%c", (c >= 0x20 && c <= 0x7e) || c == '\n' || c == '\t' ? c : '.');
	}
	if (n == 0 || *(volatile char *)(out_pa + (u64)((tail + n - 1) % out_len)) != '\n')
		log_puts("\n");
}

/*
 * The APCS watchdog's bite powers the PMIC down instead of resetting the
 * SoC (tested 2026-11-06), so it is not armed here; a hung xnu leaves the
 * phone off until the power key is held.
 */

/*
 * Can DRAM survive the reset that ends an xnu run? Write a magic just
 * below the panic area on every boot and report what the previous boot
 * left there. This is independent of whether xnu managed to log anything.
 */
#define DRAM_PROBE_PA		0xbfef0000ull
#define DRAM_PROBE_MAGIC	0x4d4152444b524150ull	/* 'PARKDRAM' */

static void
dram_probe_prev(void)
{
	u64 v = rd64(DRAM_PROBE_PA);
	u64 started = rd64(DRAM_PROBE_PA + 8);

	log_printf("prev: dram %s (%lx) start=%lx\n",
		   v == DRAM_PROBE_MAGIC ? "kept" : "lost", v, started);
	*(volatile u64 *)(DRAM_PROBE_PA + 8) = 0;
	*(volatile u64 *)DRAM_PROBE_PA = DRAM_PROBE_MAGIC;
}

const char *loader_boot(void)
{
	const struct payload *p = (const struct payload *)RD_BASE;
	u64 blob_len, pa_of_va_base, adt_pa, ba_pa, top, entry_pa;
	u32 i;

	if (!magic_ok(p->magic))
		return "no SWEETXNU payload";
	if (p->version != 1 || p->nsegs == 0 || p->nsegs > 64 ||
	    (p->adt_off & 7))
		return "bad payload header";
	log_printf("loader: payload v%u nsegs=%u virt=%lx phys=%lx size=%lx\n",
		   p->version, p->nsegs, p->virt_base, p->phys_base,
		   p->mem_size);

	dram_probe_prev();
	dump_prev_shmcon(p->phys_base + p->mem_size);

	blob_len = p->adt_off + p->adt_size;
	for (i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];

		if (s->file_off + s->file_size > blob_len)
			blob_len = s->file_off + s->file_size;
	}
	if (RD_BASE + blob_len > p->phys_base &&
	    RD_BASE < p->phys_base + p->mem_size)
		return "payload overlaps kernel memory";

	pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	for (i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		u64 dst = s->vmaddr + pa_of_va_base;
		u64 fsz = (s->file_size + 7) & ~7ull;

		copy64(dst, RD_BASE + s->file_off, s->file_size);
		if (s->vmsize > fsz)
			zero64(dst + fsz, s->vmsize - fsz);
	}
	log_printf("loader: segs done, end=%lx entry=%lx\n",
		   p->kernel_end + pa_of_va_base, p->entry_va + pa_of_va_base);

	/* ADT and boot_args behind the kernel; topOfKernelData is after them. */
	adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, RD_BASE + p->adt_off, p->adt_size);
	ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	{
		volatile struct boot_args *ba =
			(volatile struct boot_args *)ba_pa;

		ba->Revision = 2;
		ba->Version = 2;
		ba->virtBase = p->virt_base;
		ba->physBase = p->phys_base;
		ba->memSize = p->mem_size;
		ba->topOfKernelData = top;
		ba->deviceTreeP = adt_pa - pa_of_va_base;	/* kernel VA */
		ba->deviceTreeLength = (u32)p->adt_size;
		for (i = 0; i < BOOT_LINE_LENGTH; i++)
			ba->CommandLine[i] = p->cmdline[i];
		ba->memSizeActual = p->mem_size;
	}
	log_printf("loader: adt=%lx ba=%lx top=%lx cmd=\"%s\"\n",
		   adt_pa, ba_pa, top, p->cmdline);

	entry_pa = p->entry_va + pa_of_va_base;
	log_printf("loader: jumping to %lx el=%lx\n", entry_pa, current_el());
	log_drain(1500);

	/* MMU/caches off writes; drop stale I-cache lines like sweet's shim. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
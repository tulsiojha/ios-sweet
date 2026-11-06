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
	log_printf("loader: jumping to %lx\n", entry_pa);
	log_drain(400);

	/* MMU/caches off writes; drop stale I-cache lines like sweet's shim. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
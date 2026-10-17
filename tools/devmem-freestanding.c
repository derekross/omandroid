// SPDX-License-Identifier: MIT
// devmem for a first-light initramfs that has no musl toolchain.
// Usage matches tools/devmem.c: devmem <address> [value]
// A second argument is the 32-bit value to write, never a width.
typedef unsigned int u32;
typedef unsigned long u64;
typedef long s64;

static s64 sc(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f)
{
	register s64 x8 __asm__("x8") = n;
	register s64 x0 __asm__("x0") = a;
	register s64 x1 __asm__("x1") = b;
	register s64 x2 __asm__("x2") = c;
	register s64 x3 __asm__("x3") = d;
	register s64 x4 __asm__("x4") = e;
	register s64 x5 __asm__("x5") = f;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

static void __attribute__((noreturn)) quit(int code)
{
	sc(93, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static void wr(const char *s)
{
	u64 n = 0;
	while (s[n])
		n++;
	sc(64, 1, (s64)s, (s64)n, 0, 0, 0);
}

static void hex8(u32 v)
{
	char b[11];
	int i;
	b[0] = '0';
	b[1] = 'x';
	for (i = 9; i >= 2; i--) {
		u32 n = v & 0xf;
		b[i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
		v >>= 4;
	}
	b[10] = '\n';
	sc(64, 1, (s64)b, 11, 0, 0, 0);
}

static int parse_u32(const char *s, u32 *out)
{
	u64 v = 0;
	int hex = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		hex = 1;
		s += 2;
	}
	if (!*s)
		return -1;
	for (; *s; s++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (hex && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (hex && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -1;
		v = hex ? v * 16 + (u32)d : v * 10 + (u32)d;
		if (v > 0xffffffffu)
			return -1;
	}
	*out = (u32)v;
	return 0;
}

static u32 access_reg(u32 addr, int do_write, u32 value, int *ok)
{
	u64 page = addr & ~0xfffu;
	s64 fd = sc(56, -100, (s64)"/dev/mem", 04010002, 0, 0, 0);
	s64 map;
	u32 v;
	*ok = 0;
	if (fd < 0)
		return 0;
	map = sc(222, 0, 0x1000, 3, 1, fd, (s64)page);
	if (map < 0) {
		sc(57, fd, 0, 0, 0, 0, 0);
		return 0;
	}
	if (do_write)
		*(volatile u32 *)(map + (addr - page)) = value;
	v = *(volatile u32 *)(map + (addr - page));
	sc(215, map, 0x1000, 0, 0, 0, 0);
	sc(57, fd, 0, 0, 0, 0, 0);
	*ok = 1;
	return v;
}

void devmem_main(unsigned long *sp)
{
	unsigned long argc = sp[0];
	char **argv = (char **)(sp + 1);
	u32 addr, value = 0, got;
	int ok, do_write = 0;
	if (argc < 2 || argc > 3 || parse_u32(argv[1], &addr)) {
		wr("usage: devmem <address> [value]\n");
		quit(1);
	}
	if (argc == 3) {
		if (parse_u32(argv[2], &value)) {
			wr("bad value\n");
			quit(1);
		}
		do_write = 1;
	}
	got = access_reg(addr, do_write, value, &ok);
	if (!ok) {
		wr("devmem failed\n");
		quit(1);
	}
	hex8(got);
	quit(0);
}

__attribute__((naked)) void _start(void)
{
	__asm__ volatile("mov x0, sp\n\tb devmem_main\n");
}

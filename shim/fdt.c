/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location, ABL's bootargs). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

/* The value and length of property prop of /chosen; 0 if absent. */
static int chosen_prop(uintptr_t fdt, const char *prop, uintptr_t *val, uint32_t *len)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			uintptr_t v = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				*val = v;
				*len = l;
				return 1;
			}
			p = align4(v + l);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	uintptr_t val;
	uint32_t len;
	if (!chosen_prop(fdt, prop, &val, &len))
		return 0;
	if (len == 4)
		*out = be32(val);
	else if (len == 8)
		*out = (uint64_t)be32(val) << 32 | be32(val + 4);
	else
		return 0;
	return 1;
}

int fdt_chosen_has(uintptr_t fdt, const char *prop, const char *word)
{
	uintptr_t val;
	uint32_t len;
	if (!chosen_prop(fdt, prop, &val, &len))
		return 0;
	/* a word: at the start or after a space, followed by a space or the end */
	for (uint32_t i = 0; i < len; i++) {
		volatile const char *s = (volatile const char *)(val + i);
		if (i && s[-1] != ' ')
			continue;
		uint32_t k = 0;
		while (word[k] && i + k < len && s[k] == word[k])
			k++;
		if (!word[k] && (i + k == len || s[k] == ' ' || s[k] == 0))
			return 1;
	}
	return 0;
}

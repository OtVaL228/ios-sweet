/*
 * exports: the exported symbols of a loaded dylib (from its LC_DYLD_EXPORTS_TRIE / dyld info in memory), and optionally
 * the code of some of them, for reading a shared-cache library without extracting the cache.
 *   exports PATH                     "addr name" per export
 *   exports PATH NAME [BYTES] > f    the first BYTES (default 512) of NAME's code, raw
 *   exports PATH -text > f           the whole __TEXT segment, raw (offset 0 = the Mach-O header)
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t *base, *trie;
static uint64_t trie_size;
static const char *want;
static uint64_t want_off;

static uint64_t
uleb(const uint8_t **p)
{
	uint64_t v = 0;
	int s = 0;
	uint8_t b;
	do {
		b = *(*p)++;
		v |= (uint64_t)(b & 0x7f) << s;
		s += 7;
	} while (b & 0x80);
	return v;
}

static void
walk(const uint8_t *node, char *name, int len)
{
	const uint8_t *p = node;
	uint64_t info = uleb(&p);
	if (info) {
		const uint8_t *q = p;
		uint64_t flags = uleb(&q);
		if (!(flags & 0x08)) {           /* not a re-export */
			uint64_t off = uleb(&q);
			name[len] = 0;
			if (want) {
				if (!strcmp(name, want))
					want_off = off;
			} else
				printf("%llx %s\n", (unsigned long long)off, name);
		}
	}
	p += info;
	uint8_t n = *p++;
	for (uint8_t i = 0; i < n; i++) {
		const char *edge = (const char *)p;
		size_t el = strlen(edge);
		p += el + 1;
		uint64_t child = uleb(&p);
		if (len + el < 1023) {
			memcpy(name + len, edge, el);
			walk(trie + child, name, len + (int)el);
		}
	}
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	for (uint32_t i = 0; i < _dyld_image_count(); i++)
		if (!strcmp(_dyld_get_image_name(i), argv[1]))
			base = (const uint8_t *)_dyld_get_image_header(i);
	if (!base)
		return 1;
	const struct mach_header_64 *mh = (const void *)base;
	const uint8_t *lc = base + sizeof(*mh);
	uint64_t text_vm = 0, text_size = 0, le_vm = 0, le_off = 0;
	uint32_t toff = 0, tsize = 0;
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *c = (const void *)lc;
		if (c->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *s = (const void *)lc;
			if (!strcmp(s->segname, "__TEXT")) {
				text_vm = s->vmaddr;
				text_size = s->vmsize;
			}
			if (!strcmp(s->segname, "__LINKEDIT")) {
				le_vm = s->vmaddr;
				le_off = s->fileoff;
			}
		} else if (c->cmd == LC_DYLD_EXPORTS_TRIE) {
			const struct linkedit_data_command *d = (const void *)lc;
			toff = d->dataoff;
			tsize = d->datasize;
		} else if (c->cmd == LC_DYLD_INFO_ONLY || c->cmd == LC_DYLD_INFO) {
			const struct dyld_info_command *d = (const void *)lc;
			if (!toff) {
				toff = d->export_off;
				tsize = d->export_size;
			}
		}
		lc += c->cmdsize;
	}
	if (!toff)
		return 1;
	intptr_t slide = (intptr_t)base - (intptr_t)text_vm;
	trie = (const uint8_t *)(le_vm + slide + (toff - le_off));
	trie_size = tsize;
	char name[1024];
	if (argc > 2 && !strcmp(argv[2], "-text")) {
		write(1, base, text_size);
		return 0;
	}
	if (argc > 2) {
		want = argv[2];
		walk(trie, name, 0);
		if (!want_off)
			return 1;
		size_t n = argc > 3 ? (size_t)atol(argv[3]) : 512;
		fprintf(stderr, "%s at +%llx\n", want, (unsigned long long)want_off);
		write(1, base + want_off, n);
		return 0;
	}
	walk(trie, name, 0);
	return 0;
}

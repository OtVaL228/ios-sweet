/*
 * libsweet-bbd.dylib: our code inside backboardd (the window server and
 * CoreAnimation's render server), put there by /usr/libexec/sweet-bbd.
 * Step 1 of G4: only says it is there. backboardd's stdout/stderr go to
 * /dev/null, which the kernel copies to the console log for it (sweetnull,
 * xnu 0048), so the line shows as "|backboardd| SWEET BBD: ...".
 */
#include <stdio.h>
#include <unistd.h>

__attribute__((constructor)) static void
sweet_bbd_loaded(void)
{
	fprintf(stderr, "SWEET BBD: library loaded into pid %d (uid %d)\n", getpid(), getuid());
}

/*
 * libsweet-insert-test.dylib: proves that dyld honours DYLD_INSERT_LIBRARIES
 * (needs xnu 0121: our AMFI answers dyld's policy query). Its constructor
 * says which process it was loaded into; the line reaches the log through
 * sweet-autorun, which relays the program's stderr.
 */
#include <mach-o/dyld.h>
#include <stdio.h>
#include <unistd.h>

__attribute__((constructor)) static void
sweet_insert_test(void)
{
	char path[1024];
	uint32_t size = sizeof(path);
	if (_NSGetExecutablePath(path, &size) != 0) {
		path[0] = '\0';
	}
	fprintf(stderr, "SWEET INSERT: loaded into pid %d (%s)\n", getpid(), path);
}

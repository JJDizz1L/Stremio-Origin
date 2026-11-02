#include "platform/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <unistd.h>

int plat_mem_free_mb(void) {
  struct sysinfo si;
  if (sysinfo(&si) != 0) return -1;
  /* si.freeram is free pages; MemAvailable would be better but sysinfo lacks it */
  unsigned long freeKb = si.freeram * si.mem_unit / 1024;
  return (int)(freeKb / 1024);
}
void plat_mem_self(int *rssMb, int *swapMb) {
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) { *rssMb = -1; *swapMb = -1; return; }
  long pages = 0, dummy;
  if (fscanf(f, "%ld %ld", &dummy, &pages) != 2) pages = 0;
  fclose(f);
  long pageSize = sysconf(_SC_PAGESIZE);
  *rssMb = (int)((pages * pageSize) >> 20);
  *swapMb = -1;
}
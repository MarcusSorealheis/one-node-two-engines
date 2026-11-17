// psiwatch.c: count how often PSI triggers fire.
//
//   psiwatch <seconds> <psi file> "<trigger>" [<psi file> "<trigger>" ...]
//
// A trigger is "<some|full> <stall us> <window us>", written to a file under
// /proc/pressure/ (or a cgroup v2 *.pressure file) and then poll()ed for
// POLLPRI, as described in the kernel's Documentation/accounting/psi.rst.
// The kernel delivers at most one event per window, so with a 1 s window the
// count is "seconds in which the threshold was crossed". Prints one count per
// trigger. Windows that are not a multiple of 2 s need CAP_SYS_RESOURCE.
//
// Build: gcc -O2 -o psiwatch psiwatch.c
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_TRIGGERS 8

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
  int n = (argc - 2) / 2;
  if (argc < 4 || argc % 2 || n > MAX_TRIGGERS) {
    fprintf(stderr, "usage: psiwatch <seconds> <psi file> \"<trigger>\" ...\n");
    return 2;
  }
  struct pollfd fds[MAX_TRIGGERS];
  long fired[MAX_TRIGGERS] = {0};
  for (int i = 0; i < n; i++) {
    const char *file = argv[2 + 2 * i], *trig = argv[3 + 2 * i];
    fds[i].fd = open(file, O_RDWR | O_NONBLOCK); /* one fd per trigger */
    if (fds[i].fd < 0 || write(fds[i].fd, trig, strlen(trig) + 1) < 0) {
      perror(file);
      return 1;
    }
    fds[i].events = POLLPRI;
  }
  double end = now() + atof(argv[1]);
  for (double left; (left = end - now()) > 0;) {
    if (poll(fds, n, (int)(left * 1000) + 1) < 0) {
      perror("poll");
      return 1;
    }
    for (int i = 0; i < n; i++) {
      if (fds[i].revents & POLLERR) return 1; /* psi file went away */
      if (fds[i].revents & POLLPRI) fired[i]++;
    }
  }
  for (int i = 0; i < n; i++) printf("%s%ld", i ? " " : "", fired[i]);
  printf("\n");
  return 0;
}

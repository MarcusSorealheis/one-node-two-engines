// contend.c: two storage engines' I/O models on one Linux kernel.
//
//   search  A Lucene-style engine reduced to its I/O model:
//           - the index is one big mmap()ed file; a query is a burst of random
//             page reads (postings blocks, doc values, HNSW neighbours, vectors)
//           - an indexer writes every incoming document AGAIN as an immutable
//             32 MB segment, fsyncs it, and merges 4 segments of a tier into
//             one at a throttled 32 MB/s; a new top-tier segment supersedes
//             the previous one, so the live segment set stays bounded (an
//             update workload)
//   db      A transactional engine reduced to its I/O model:
//           - a private cache of anonymous memory (not page cache)
//           - txn = a few 8 KB point reads that missed that cache (pread, so
//             they are served by the page cache or the disk) + WAL append +
//             fdatasync
//           - the same bytes applied in place to the collection/B-tree file
//           - a checkpointer that trickles dirty pages out every second
//             (sync_file_range) and fdatasyncs the collection every 10 s
//
// Nothing here is mongod, mongot or Postgres. It is the smallest program that
// makes the kernel treat two processes the way it treats those engines.
//
// Build: gcc -O2 -pthread -o contend contend.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096UL
#define MB (1UL << 20)
#define FLUSH_BYTES (32 * MB) /* indexer RAM buffer -> one tier-0 segment   */
#define MERGE_FACTOR 4        /* segments per tier before a merge          */
#define TOP_TIER 2            /* 32 MB -> 128 MB -> 512 MB, then supersede */
#define MERGE_MBPS 32.0       /* merges are rate-limited, as real ones are */
#define WAL_BYTES (256 * MB)  /* circular, fully pre-written               */
#define CKPT_SECS 10
#define BUCKET_SECS 15        /* optional time series (CONTEND_TS=file)    */

static void die(const char *m) { perror(m); exit(1); }

static double mono(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static double wall(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static void sleep_until(double when) { /* CLOCK_MONOTONIC, absolute */
  struct timespec t = {(time_t)when, (long)((when - (time_t)when) * 1e9)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}
static void wait_for_wall(double start_epoch) { /* line both processes up */
  double d = start_epoch - wall();
  if (d > 0) sleep_until(mono() + d);
}
static inline uint64_t rnd(uint64_t *s) { /* xorshift64* */
  uint64_t x = *s;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1DULL;
}
static int cmpf(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}
/* optional per-bucket time series, in arrival order, before the final sort */
static void series(const char *tag, const float *lat, long n, double rate) {
  const char *path = getenv("CONTEND_TS");
  if (!path) return;
  FILE *f = fopen(path, "a");
  long per = (long)(BUCKET_SECS * rate);
  float *tmp = malloc(per * sizeof(float));
  for (long b = 0; (b + 1) * per <= n; b++) {
    memcpy(tmp, lat + b * per, per * sizeof(float));
    qsort(tmp, per, sizeof(float), cmpf);
    fprintf(f, "%s t=%3ld-%3lds p50_us=%.0f p99_us=%.0f max_us=%.0f\n", tag, b * BUCKET_SECS,
            (b + 1) * BUCKET_SECS, tmp[per / 2], tmp[(long)(per * 0.99)], tmp[per - 1]);
  }
  fclose(f);
  free(tmp);
}
static void pcts(const char *tag, float *lat, long n) {
  qsort(lat, n, sizeof(float), cmpf);
  printf("%s n=%ld p50_us=%.0f p99_us=%.0f p999_us=%.0f max_us=%.0f", tag, n,
         lat[n / 2], lat[(long)(n * 0.99)], lat[(long)(n * 0.999)], lat[n - 1]);
}
static volatile int stop_flag, measuring;

/* ---------------------------------------------------------------- mkfile */
static void mkfile(const char *path, size_t bytes) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) die("open");
  char *b = malloc(MB);
  memset(b, 0x5a, MB);
  for (size_t off = 0; off < bytes; off += MB)
    if (write(fd, b, MB) != (ssize_t)MB) die("write");
  if (fsync(fd)) die("fsync");
  close(fd);
}

/* ---------------------------------------------------------------- search */
static uint8_t *idx;
static size_t idx_len;
static double resident_min = 100.0, resident_end = 100.0;

static double resident_pct(void) { /* how much of the index is in RAM right now */
  size_t pages = idx_len / PAGE, in = 0;
  unsigned char *vec = malloc(pages);
  if (mincore(idx, idx_len, vec)) die("mincore");
  for (size_t i = 0; i < pages; i++) in += vec[i] & 1;
  free(vec);
  return 100.0 * in / pages;
}
static void *sampler(void *arg) {
  (void)arg;
  const char *path = getenv("CONTEND_TS");
  double t0 = mono();
  while (!stop_flag) {
    double r = resident_pct();
    if (measuring && r < resident_min) resident_min = r;
    resident_end = r;
    if (path) {
      FILE *f = fopen(path, "a");
      fprintf(f, "resident t=%.0fs pct=%.1f\n", mono() - t0, r);
      fclose(f);
    }
    for (int i = 0; i < 50 && !stop_flag; i++) usleep(100000);
  }
  return NULL;
}

struct indexer {
  const char *dir;
  double bytes_per_s, start_epoch;
  long flushed_mb, merged_mb, merges;
};
static void seg_name(char *out, const char *dir, int tier, long id) {
  sprintf(out, "%s/seg_t%d_%ld", dir, tier, id);
}
static void *indexer(void *arg) {
  struct indexer *ix = arg;
  static long ids[TOP_TIER + 1][MERGE_FACTOR], cnt[TOP_TIER + 1];
  static char doc[8192];
  char *buf = malloc(FLUSH_BYTES), *io = malloc(MB), name[512], in[512];
  long next_id = 0, top_prev = -1;
  size_t fill = 0;
  memset(doc, 0x33, sizeof doc);
  wait_for_wall(ix->start_epoch);
  double t0 = mono(), ingested = 0;
  while (!stop_flag) {
    double want = (mono() - t0) * ix->bytes_per_s; /* the same write stream the db sees */
    while (!stop_flag && ingested + sizeof doc <= want) {
      memcpy(buf + fill, doc, sizeof doc);
      fill += sizeof doc;
      ingested += sizeof doc;
      if (fill < FLUSH_BYTES) continue;
      /* flush: the document bytes reach the disk a second time */
      long id = next_id++;
      seg_name(name, ix->dir, 0, id);
      int fd = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd < 0 || write(fd, buf, fill) != (ssize_t)fill || fsync(fd)) die("flush");
      close(fd);
      ix->flushed_mb += fill / MB;
      fill = 0;
      ids[0][cnt[0]++] = id;
      /* merge: read MERGE_FACTOR segments, write one, fsync, unlink the inputs */
      for (int t = 0; t < TOP_TIER && cnt[t] == MERGE_FACTOR; t++) {
        long out_id = next_id++;
        seg_name(name, ix->dir, t + 1, out_id);
        int out = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out < 0) die("merge open");
        double m0 = mono(), copied = 0;
        for (int k = 0; k < MERGE_FACTOR; k++) {
          seg_name(in, ix->dir, t, ids[t][k]);
          int src = open(in, O_RDONLY);
          if (src < 0) die("merge input");
          ssize_t r;
          while ((r = read(src, io, MB)) > 0) {
            if (write(out, io, r) != r) die("merge write");
            ix->merged_mb += r / MB;
            copied += r;
            sleep_until(m0 + copied / (MERGE_MBPS * MB)); /* throttle */
          }
          close(src);
        }
        if (fsync(out)) die("merge fsync");
        close(out);
        for (int k = 0; k < MERGE_FACTOR; k++) {
          seg_name(in, ix->dir, t, ids[t][k]);
          unlink(in);
        }
        cnt[t] = 0;
        ix->merges++;
        if (t + 1 < TOP_TIER) {
          ids[t + 1][cnt[t + 1]++] = out_id;
        } else { /* updates: the new top-tier segment supersedes the old one */
          if (top_prev >= 0) {
            seg_name(in, ix->dir, TOP_TIER, top_prev);
            unlink(in);
          }
          top_prev = out_id;
        }
      }
    }
    usleep(20000);
  }
  return NULL;
}

static void search(const char *path, double ramp, double secs, double qps, int pages_per_q,
                   const char *segdir, double idx_bytes_per_s, double start_epoch) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st)) die("index");
  idx_len = st.st_size;
  idx = mmap(NULL, idx_len, PROT_READ, MAP_SHARED, fd, 0);
  if (idx == MAP_FAILED) die("mmap");
  /* preload, as a search node does after start-up, then declare random access */
  char *b = malloc(MB);
  while (read(fd, b, MB) > 0) {}
  free(b);
  madvise(idx, idx_len, MADV_RANDOM);

  pthread_t ts, ti;
  struct indexer ix = {segdir, idx_bytes_per_s, start_epoch, 0, 0, 0};
  pthread_create(&ts, NULL, sampler, NULL);
  if (idx_bytes_per_s > 0) pthread_create(&ti, NULL, indexer, &ix);

  long n = (long)((ramp + secs) * qps), k0 = (long)(ramp * qps);
  float *lat = malloc(n * sizeof(float));
  uint64_t seed = 0x9E3779B97F4A7C15ULL, sum = 0;
  size_t npages = idx_len / PAGE;
  struct rusage r0, r1;
  wait_for_wall(start_epoch);
  double t0 = mono();
  for (long k = 0; k < n; k++) {
    if (k == k0) { /* ramp over: start measuring */
      getrusage(RUSAGE_THREAD, &r0);
      measuring = 1;
    }
    double due = t0 + k / qps; /* open loop: arrivals do not wait for us */
    if (mono() < due) sleep_until(due);
    for (int i = 0; i < pages_per_q; i++) {
      uint64_t r = rnd(&seed);
      sum += idx[(r % npages) * PAGE + ((r >> 40) & (PAGE - 1))];
    }
    lat[k] = (float)((mono() - due) * 1e6); /* includes time spent queued */
  }
  getrusage(RUSAGE_THREAD, &r1);
  stop_flag = 1;
  pthread_join(ts, NULL);
  if (idx_bytes_per_s > 0) pthread_join(ti, NULL);
  series("search", lat, n, qps);
  pcts("search", lat + k0, n - k0);
  printf(" majflt=%ld resident_min_pct=%.1f resident_end_pct=%.1f flushed_mb=%ld merged_mb=%ld merges=%ld chk=%lu\n",
         r1.ru_majflt - r0.ru_majflt, resident_min, resident_end, ix.flushed_mb,
         ix.merged_mb, ix.merges, (unsigned long)(sum & 0xff));
}

/* -------------------------------------------------------------------- db */
static int coll_fd;
static void *checkpointer(void *arg) {
  (void)arg;
  for (int tick = 1; !stop_flag; tick++) {
    for (int i = 0; i < 10 && !stop_flag; i++) usleep(100000);
    sync_file_range(coll_fd, 0, 0, SYNC_FILE_RANGE_WRITE); /* trickle dirty pages out */
    if (tick % CKPT_SECS == 0) fdatasync(coll_fd);         /* checkpoint              */
  }
  return NULL;
}
static void db(const char *dir, double ramp, double secs, size_t cache_bytes, double txns_per_s,
               size_t commit_bytes, int reads_per_txn, double start_epoch) {
  char p[512];
  /* the engine's own cache: anonymous memory the kernel cannot drop */
  char *cache = mmap(NULL, cache_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (cache == MAP_FAILED) die("cache");
  memset(cache, 1, cache_bytes);
  char *doc = malloc(commit_bytes > MB ? commit_bytes : MB), *rbuf = malloc(8192);
  memset(doc, 0x77, commit_bytes > MB ? commit_bytes : MB);
  sprintf(p, "%s/wal", dir);
  int wal = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (wal < 0) die("wal");
  for (size_t off = 0; off < WAL_BYTES; off += MB) /* pre-write: commits never allocate */
    if (write(wal, doc, MB) != (ssize_t)MB) die("wal prefill");
  if (fsync(wal)) die("wal fsync");
  sprintf(p, "%s/collection", dir);
  coll_fd = open(p, O_RDWR);
  struct stat st;
  if (coll_fd < 0 || fstat(coll_fd, &st)) die("collection");
  size_t coll_pages = st.st_size / 8192, cache_slots = cache_bytes / 8192;

  pthread_t tc;
  pthread_create(&tc, NULL, checkpointer, NULL);
  long n = (long)((ramp + secs) * txns_per_s), k0 = (long)(ramp * txns_per_s);
  float *lat = malloc(n * sizeof(float));
  uint64_t seed = 0xD1B54A32D192ED03ULL;
  off_t woff = 0;
  wait_for_wall(start_epoch);
  double t0 = mono();
  for (long k = 0; k < n; k++) {
    double due = t0 + k / txns_per_s;
    if (mono() < due) sleep_until(due);
    for (int i = 0; i < reads_per_txn; i++) /* point reads that missed the engine cache */
      if (pread(coll_fd, rbuf, 8192, (off_t)(rnd(&seed) % coll_pages) * 8192) != 8192) die("read");
    if (pwrite(wal, doc, commit_bytes, woff) != (ssize_t)commit_bytes) die("wal write");
    if (fdatasync(wal)) die("fdatasync"); /* durability point */
    woff = (woff + commit_bytes) % (WAL_BYTES - commit_bytes);
    lat[k] = (float)((mono() - due) * 1e6); /* transaction latency */
    for (size_t o = 0; o < commit_bytes; o += 8192) { /* apply to cache + B-tree file */
      memcpy(cache + (rnd(&seed) % cache_slots) * 8192, doc, 8192);
      if (pwrite(coll_fd, doc, 8192, (off_t)(rnd(&seed) % coll_pages) * 8192) != 8192) die("collection write");
    }
  }
  stop_flag = 1;
  pthread_join(tc, NULL);
  series("db", lat, n, txns_per_s);
  pcts("db", lat + k0, n - k0);
  printf(" written_mb=%ld\n", (long)(n * commit_bytes / MB));
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc == 4 && !strcmp(argv[1], "mkfile")) {
    mkfile(argv[2], strtoull(argv[3], 0, 10) * MB);
    return 0;
  }
  if (argc == 10 && !strcmp(argv[1], "search")) {
    search(argv[2], atof(argv[3]), atof(argv[4]), atof(argv[5]), atoi(argv[6]), argv[7],
           atof(argv[8]) * MB, atof(argv[9]));
    return 0;
  }
  if (argc == 10 && !strcmp(argv[1], "db")) {
    db(argv[2], atof(argv[3]), atof(argv[4]), strtoull(argv[5], 0, 10) * MB, atof(argv[6]),
       strtoull(argv[7], 0, 10) * 1024, atoi(argv[8]), atof(argv[9]));
    return 0;
  }
  fprintf(stderr,
          "usage:\n"
          "  contend mkfile <path> <MB>\n"
          "  contend search <index> <ramp s> <secs> <qps> <pages/query> <segdir> <index MB/s> <start epoch>\n"
          "  contend db <dir> <ramp s> <secs> <cache MB> <txns/s> <commit KB> <reads/txn> <start epoch>\n");
  return 2;
}

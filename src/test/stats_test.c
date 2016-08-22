#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include "cm_stats_api.h"

#define Tassert assert

int64_t global_42 = 42;
char *foo = NULL;
stats_handle_t *hist = NULL;
stats_handle_t *mtadd = NULL;

void cbtest(stats_handle_t *h, void *vptr, void *c) {
  static int64_t incr;
  if(stats_handle_type(h) != STATS_TYPE_INT64) return;
  incr++;
  **(int64_t **)vptr = incr;
}

void register_globals(stats_ns_t *ns) {
  stats_handle_t *h;
  h = stats_register(ns, "global_42", STATS_TYPE_INT64);
  Tassert(h != NULL);
  Tassert(NULL == stats_register(ns, "global_42", STATS_TYPE_UINT64));
  stats_observe(h, STATS_TYPE_INT64, &global_42);

  h = stats_register(ns, "foo", STATS_TYPE_STRING);
  stats_observe(h, STATS_TYPE_STRING, &foo);

  h = stats_register(ns, "incr", STATS_TYPE_INT64);
  stats_invoke(h, cbtest, NULL);

  mtadd = stats_register(ns, "incratomic", STATS_TYPE_COUNTER);
  stats_add32(mtadd, 1);
}

ssize_t
writefd_ref(void *fdptr, const char *buf, size_t len) {
  return write(*(int *)fdptr, buf, len);
}
void *latency_m(void *cl) {
  int i = (int)cl;
  stats_add32(mtadd, 1);
  while(true) {
    stats_set_hist_intscale(hist, lrand48()%10 + 10, -2+i, 1);
  }
  return NULL;
}
void start_thread() {
  pthread_t tid;
  pthread_create(&tid, NULL, latency_m, (void *)0);
  pthread_create(&tid, NULL, latency_m, (void *)1);
}

int main() {
  bool simple = false;
  bool since_last = false;
  stats_recorder_t *rec;
  stats_ns_t *global, *ns1;
  rec = stats_recorder_alloc();
  global = stats_recorder_global_ns(rec);
  ns1 = stats_register_ns(rec, global, "ns1");
  Tassert(ns1 != NULL);
  Tassert(ns1 == stats_register_ns(rec, NULL, "ns1"));
  register_globals(ns1);

  hist = stats_register(global, "latency", STATS_TYPE_HISTOGRAM);

  int fd = 1;
  stats_recorder_output_json(rec, true, simple, writefd_ref, &fd);
  foo = "huzza wuzza";
  int32_t val = 2;
  stats_set(hist, STATS_TYPE_INT32, &val);
  stats_set_hist(hist, 1.2, 1);
  stats_set_hist_intscale(hist, 1200, -6, 1);

  start_thread();

  int cnt = 5;
  while(cnt-- > 0) {
    printf("\n\n");
    if(cnt == 0) simple = true;
    stats_recorder_output_json(rec, since_last, simple, writefd_ref, &fd);
    since_last = !since_last;
    sleep(1);
  }

  exit(0);
}

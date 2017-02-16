#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ck_hs.h>
#include <ck_pr.h>
#include <ck_spinlock.h>
#include <pthread.h>
#include <circllhist.h>
#include <assert.h>
#include <math.h>
#include <inttypes.h>

#include "cm_stats_api.h"
#include "stats_hash_f.h"

#define MAX_FANOUT 128
#define DEFAULT_FANOUT 8
#ifndef unlikely
#define unlikely(x)    __builtin_expect(!!(x), 0)
#endif

#if defined(linux) || defined(__linux) || defined(__linux__)
#include <sched.h>
#elif defined(__sun__) || defined(__sun) || defined(sun)
#include <sys/processor.h>
#endif

static __thread int circmetrics_tid;
static inline int __get_fanout(int fanout) {
  if(unlikely(circmetrics_tid == 0)) {
#if defined(linux) || defined(__linux) || defined(__linux__)
    unsigned cpu = sched_getcpu();
    circmetrics_tid = (int)cpu;
#elif defined(__sun__) || defined(__sun) || defined(sun)
    circmetrics_tid = (int)getcpuid();
#else
    circmetrics_tid = (int)(intptr_t)pthread_self();
    if(circmetrics_tid > 0x100) {
      int f = circmetrics_tid;
      circmetrics_tid = 0;
      while(f) {
        circmetrics_tid = circmetrics_tid | (f & 0x7f);
        f >>= 7;
      }
    }
#endif
  }
  return circmetrics_tid % fanout;
}

struct stats_recorder_t {
  struct stats_ns_t *global;
};
struct stats_ns_freshnode {
  stats_ns_update_func_t f;
  void *closure;
  struct stats_ns_freshnode *next;
};
struct stats_ns_t {
  stats_recorder_t          *rec;
  pthread_mutex_t            lock;
  ck_hs_t                    map;
  struct stats_ns_freshnode *freshen;
};
struct stats_handle_t {
  stats_ns_t              *ns;
  stats_type_t             type;

  stats_invocation_func_t  cb;
  void                    *cb_closure;

  void                    *valueptr;

  union {
    char                     pad[CK_MD_CACHELINE];
    struct {
      histogram_t             *hist;
      uint64_t                 incr;
      /* need progress guarantees, standard spinlock is FAS which is not fair */
      ck_spinlock_ticket_t     spinlock;
    }                        cpu;
  }                       *fan;
  int                      fanout;
  histogram_t             *hist_aggr;
  int                      last_size;

  union {
    int32_t                  i32;
    uint32_t                 u32;
    int64_t                  i64;
    uint64_t                 u64;
    double                   d;
  }                        store;
  struct {
    char *                   value;
    int                      len;
  }                        str;
  char                   **strref;
  /* need progress guarantees, standard spinlock is FAS which is not fair */
  ck_spinlock_ticket_t   spinlock;
};

// The one true container for all things
// The hashes hold these...
typedef struct stats_container_t {
  stats_ns_t     *ns;
  stats_handle_t *handle;
  const char     *key;
  int             len;
} stats_container_t;

static void * hs_malloc(size_t r) { return malloc(r); }
static void hs_free(void *p, size_t b, bool r) { (void)b; (void)r; free(p); return; }
static struct ck_malloc hs_allocator = {
  .malloc = hs_malloc, .free = hs_free
};
static unsigned long hs_hash(const void *object, unsigned long seed)
{
  const stats_container_t *c = object;
  unsigned long h;

  h = (unsigned long)__hash(c->key, c->len, seed);
  return h;
}
static bool hs_compare(const void *previous, const void *compare) {
  const stats_container_t *prev = previous;
  const stats_container_t *cur = compare;

  if (prev->len == cur->len) {
    return memcmp(prev->key, cur->key, prev->len) == 0;
  }
  /* We know they're not equal if they have different lengths */
  return false;
}

static void
stats_ns_free(stats_ns_t *ns) {
  if(ns == NULL) return;
  pthread_mutex_destroy(&ns->lock);
  free(ns);
}
static stats_ns_t *
stats_ns_alloc(stats_recorder_t *rec) {
  stats_ns_t *ns = calloc(1, sizeof(*ns));
  if(ck_hs_init(&ns->map, CK_HS_MODE_OBJECT|CK_HS_MODE_SPMC,
                hs_hash, hs_compare, &hs_allocator, 128, lrand48()) == 0) {
    free(ns);
    return NULL;
  }
  pthread_mutex_init(&ns->lock, NULL);
  ns->rec = rec;
  return ns;
}

const char *
stats_type_name(stats_type_t t) {
  switch(t) {
  case STATS_TYPE_STRING: return "string";
  case STATS_TYPE_INT32: return "int32";
  case STATS_TYPE_UINT32: return "uint32";
  case STATS_TYPE_INT64: return "int64";
  case STATS_TYPE_UINT64: return "uint64";
  case STATS_TYPE_COUNTER: return "counter";
  case STATS_TYPE_DOUBLE: return "double";
  case STATS_TYPE_HISTOGRAM: return "histogram";
  case STATS_TYPE_HISTOGRAM_FAST: return "histogram_fast";
  }
  return "unknown";
}

stats_recorder_t *
stats_recorder_alloc(void) {
  stats_recorder_t *rec = calloc(1, sizeof(*rec));
  rec->global = stats_ns_alloc(rec);
  return rec;
}

stats_ns_t *
stats_recorder_global_ns(stats_recorder_t *rec) {
  return rec ? rec->global : NULL;
}

static stats_container_t *
stats_ns_add_container(stats_ns_t *ns, const char *name) {
  long hashv;
  stats_container_t nc, *prev, *toadd = NULL;

  if(strchr(name, '"')) return NULL;

  nc.key = name;
  nc.len = strlen(name);
  // hashv won't change
  hashv = CK_HS_HASH(&ns->map, hs_hash, &nc);
  do {
    prev = ck_hs_get(&ns->map, hashv, &nc);
    if(!prev) {
      toadd = calloc(1, sizeof(*toadd));
      toadd->key = strdup(name);
      toadd->len = strlen(toadd->key);
    
      pthread_mutex_lock(&ns->lock);
      if(ck_hs_put(&ns->map, hashv, toadd)) {
        prev = toadd;
        toadd = NULL;
      }
      pthread_mutex_unlock(&ns->lock);
      if(toadd) {
        free((void *)toadd->key);
        free(toadd);
      }
    }
  } while(prev == NULL);
  return prev;
}

stats_ns_t *
stats_register_ns(stats_recorder_t *rec, stats_ns_t *ns, const char *name) {
  stats_container_t *c;
  stats_ns_t *new_ns;
  if(rec == NULL && ns) rec = ns->rec;
  if(ns == NULL && rec) ns = rec->global;
  if(ns == NULL || rec == NULL) return NULL;
  if(ns->rec != rec) return NULL;
  c = stats_ns_add_container(ns, name);
  if(!c) return NULL;
  if(c->ns) return c->ns;
  new_ns = stats_ns_alloc(rec);
  pthread_mutex_lock(&ns->lock);
  if(c->ns == NULL) {
    c->ns = new_ns;
    new_ns = NULL;
  }
  pthread_mutex_unlock(&ns->lock);
  if(new_ns != NULL) stats_ns_free(new_ns);
  return c->ns;
}

bool
stats_ns_invoke(stats_ns_t *ns, stats_ns_update_func_t f, void *closure) {
  struct stats_ns_freshnode *node = calloc(1, sizeof(*node));
  node->f = f;
  node->closure = closure;
  pthread_mutex_lock(&ns->lock);
  node->next = ns->freshen;
  ns->freshen = node;
  pthread_mutex_unlock(&ns->lock);
  return true;
}

void
stats_ns_update(stats_ns_t *ns) {
  struct stats_ns_freshnode *node;
  if(!ns) return;
  for(node = ns->freshen; node; node = node->next) {
    node->f(ns, node->closure);
  }
}

static stats_handle_t *
stats_handle_alloc(stats_ns_t *ns, stats_type_t type, int fanout) {
  stats_handle_t *h = calloc(1, sizeof(*h));
  h->ns = ns;
  h->type = type;
  h->strref = &h->str.value;
  if(type == STATS_TYPE_HISTOGRAM ||
     type == STATS_TYPE_HISTOGRAM_FAST ||
     type == STATS_TYPE_COUNTER) {
    h->fanout = fanout;
    if(h->fanout < 1) h->fanout = DEFAULT_FANOUT;
    if(h->fanout > MAX_FANOUT) h->fanout = MAX_FANOUT;
    h->fan = calloc(h->fanout, sizeof(*h->fan));
  }
  if(type == STATS_TYPE_STRING) {
    h->valueptr = NULL;
  }
  else if(type == STATS_TYPE_HISTOGRAM_FAST) {
    int i;
    for(i=0;i<h->fanout;i++) {
      h->fan[i].cpu.hist = hist_fast_alloc();
      ck_spinlock_init(&h->fan[i].cpu.spinlock);
    }
    h->hist_aggr = hist_fast_alloc();
    h->valueptr = h->hist_aggr;
  }
  else if(type == STATS_TYPE_HISTOGRAM) {
    int i;
    for(i=0;i<h->fanout;i++) {
      h->fan[i].cpu.hist = hist_alloc();
      ck_spinlock_ticket_init(&h->fan[i].cpu.spinlock);
    }
    h->hist_aggr = hist_alloc();
    h->valueptr = h->hist_aggr;
  }
  else {
    stats_observe(h, type, &h->store);
  }
  ck_spinlock_ticket_init(&h->spinlock);
  return h;
}

static void
stats_handle_free(stats_handle_t *h) {
  int i;
  if(h == NULL) return;
  for(i=0;i<h->fanout;i++) {
    if(h->fan[i].cpu.hist) hist_free(h->fan[i].cpu.hist);
  }
  free(h->fan);
  if(h->hist_aggr) hist_free(h->hist_aggr);
  free(h);
}

stats_handle_t *
stats_register_fanout(stats_ns_t *ns, const char *name, stats_type_t type, int fanout) {
  stats_container_t *c;
  if(fanout && (type != STATS_TYPE_COUNTER && type != STATS_TYPE_HISTOGRAM &&
                type != STATS_TYPE_HISTOGRAM_FAST))
    return NULL;
  if(ns == NULL) return NULL;
  c = stats_ns_add_container(ns, name);
  if(!c) return NULL;
  if(!c->handle) {
    stats_handle_t *h = stats_handle_alloc(ns, type, fanout);
    pthread_mutex_lock(&ns->lock);
    if(!c->handle) {
      c->handle = h;
      h = NULL;
    }
    pthread_mutex_unlock(&ns->lock);
    stats_handle_free(h);
  }
  if(c->handle->type == type) return c->handle;
  return NULL;
}

stats_handle_t *
stats_register(stats_ns_t *ns, const char *name, stats_type_t type) {
  return stats_register_fanout(ns, name, type, 0);
}

bool
stats_handle_clear(stats_handle_t *h) {
  int i;
  /* We only support clearing histograms and counters */
  switch(h->type) {
  case STATS_TYPE_HISTOGRAM_FAST:
  case STATS_TYPE_HISTOGRAM:
    for(i=0;i<h->fanout;i++)
      hist_clear(h->fan[i].cpu.hist);
    hist_clear(h->hist_aggr);
    return true;
  case STATS_TYPE_COUNTER:
    for(i=0;i<h->fanout;i++)
      h->fan[i].cpu.incr = 0;
    return true;
  default:
    break;
  }
  return false;
}

stats_type_t
stats_handle_type(stats_handle_t *h) {
  return h->type;
}

bool
stats_observe(stats_handle_t *h, stats_type_t type, void *memory) {
  if(h == NULL) return false;
  // Can't observe a histogram as they aren't thread safe
  if(h->type == STATS_TYPE_HISTOGRAM) return false;
  if(h->type == STATS_TYPE_HISTOGRAM_FAST) return false;
  if(h->type != type) return false;
  h->valueptr = memory;
  return true;
}

bool
stats_invoke(stats_handle_t *h, stats_invocation_func_t cb, void *closure) {
  if(h == NULL) return false;
  h->cb = cb;
  h->cb_closure = closure;
  return true;
}

bool
stats_set_hist(stats_handle_t *h, double d, uint64_t cnt) {
  if(h == NULL || (h->type != STATS_TYPE_HISTOGRAM &&
                   h->type != STATS_TYPE_HISTOGRAM_FAST)) return false;
  int cpu = __get_fanout(h->fanout);
  ck_spinlock_ticket_lock(&h->fan[cpu].cpu.spinlock);
  hist_insert(h->fan[cpu].cpu.hist, d, cnt);
  ck_spinlock_ticket_unlock(&h->fan[cpu].cpu.spinlock);
  return true;
}
bool
stats_set_hist_intscale(stats_handle_t *h, int64_t val, int scale, uint64_t cnt) {
  if(h == NULL || (h->type != STATS_TYPE_HISTOGRAM &&
                   h->type != STATS_TYPE_HISTOGRAM_FAST)) return false;
  int cpu = __get_fanout(h->fanout);
  ck_spinlock_ticket_lock(&h->fan[cpu].cpu.spinlock);
  hist_insert_intscale(h->fan[cpu].cpu.hist, val, scale, cnt);
  ck_spinlock_ticket_unlock(&h->fan[cpu].cpu.spinlock);
  return true;
}

bool stats_add32(stats_handle_t *h, int32_t cnt) {
  if(h == NULL) return false;
  if(h->type == STATS_TYPE_INT64 || h->type == STATS_TYPE_UINT64 ||
     h->type == STATS_TYPE_COUNTER)
    return stats_add64(h, (int64_t)cnt);
  if(h->type != STATS_TYPE_INT32 && h->type != STATS_TYPE_UINT32)
    return false;
  ck_pr_add_32(&h->store.u32, cnt);
  return true;
}

bool stats_add64(stats_handle_t *h, int64_t cnt) {
  if(h == NULL) return false;
  if(h->type == STATS_TYPE_COUNTER) {
    int cpu = __get_fanout(h->fanout);
    ck_pr_add_64(&h->fan[cpu].cpu.incr, cnt);
    return true;
  }
  if(h->type != STATS_TYPE_INT64 && h->type != STATS_TYPE_UINT64)
    return false;
  ck_pr_add_64(&h->store.u64, cnt);
  return true;
}

bool
stats_set(stats_handle_t *h, stats_type_t type, void *ptr) {
  int len, i;
  if(h == NULL) return false;
  if(h->type == STATS_TYPE_HISTOGRAM ||
     h->type == STATS_TYPE_HISTOGRAM_FAST) {
    const histogram_t * const * hptr = (const histogram_t * const *)&ptr;
    int cpu = __get_fanout(h->fanout);
    double d;
    bool rv = true;
    if(ptr == NULL) {
      for(i=0;i<h->fanout;i++) {
        hist_clear(h->fan[i].cpu.hist);
      }
      hist_clear(h->hist_aggr);
      return true;
    }
    // For histogram types, we can actually allow setting from other types
    ck_spinlock_ticket_lock(&h->fan[cpu].cpu.spinlock);
    switch(type) {
    case STATS_TYPE_COUNTER:
    case STATS_TYPE_STRING:
      rv = false; break; // but not these types
    case STATS_TYPE_HISTOGRAM:
    case STATS_TYPE_HISTOGRAM_FAST:
      hist_accumulate(h->fan[cpu].cpu.hist, hptr, 1);
    case STATS_TYPE_INT32:
      hist_insert_intscale(h->fan[cpu].cpu.hist, *((int32_t *)ptr), 0, 1);
      break;
    case STATS_TYPE_UINT32:
      hist_insert_intscale(h->fan[cpu].cpu.hist, *((uint32_t *)ptr), 0, 1);
      break;
    case STATS_TYPE_INT64:
      hist_insert_intscale(h->fan[cpu].cpu.hist, *((int64_t *)ptr), 0, 1);
      break;
    case STATS_TYPE_UINT64:
      hist_insert(h->fan[cpu].cpu.hist, (double)*((uint64_t *)ptr), 1);
      break;
    case STATS_TYPE_DOUBLE:
      hist_insert(h->fan[cpu].cpu.hist, *((double *)ptr), 1);
      break;
    }
    ck_spinlock_ticket_unlock(&h->fan[cpu].cpu.spinlock);
    return rv;
  }
  if(h->type != type) return false;
  switch(type) {
  // we necessarily handled the histogram case already
  case STATS_TYPE_COUNTER:
    if(ptr == NULL) {
      for(i=0;i<h->fanout;i++) h->fan[i].cpu.incr = 0;
      return true;
    }
    return false;
  case STATS_TYPE_HISTOGRAM: assert(type != STATS_TYPE_HISTOGRAM);
  case STATS_TYPE_HISTOGRAM_FAST: assert(type != STATS_TYPE_HISTOGRAM_FAST);
  case STATS_TYPE_STRING:
    if(ptr == NULL) {
      h->valueptr = NULL;
      return true;
    }
    len = strlen((char *)ptr) + 1;
    char *tofree = NULL;
    if(h->str.len < len) {
      char *replace = malloc(len);
      ck_spinlock_ticket_lock(&h->spinlock);
      tofree = h->str.value;
      h->str.value = replace;
      h->str.len = len;
    } else {
      ck_spinlock_ticket_lock(&h->spinlock);
    }
    memcpy(h->str.value, (char *)ptr, len);
    h->valueptr = h->strref;
    ck_spinlock_ticket_unlock(&h->spinlock);
    free(tofree);
    break;
  case STATS_TYPE_INT32:
  case STATS_TYPE_UINT32:
    h->valueptr = &h->store;
    memcpy(h->valueptr, ptr, sizeof(int32_t));
    break;
  case STATS_TYPE_INT64:
  case STATS_TYPE_UINT64:
  case STATS_TYPE_DOUBLE:
    h->valueptr = &h->store;
    memcpy(h->valueptr, ptr, sizeof(int64_t));
    break;
  }
  return true;
}

static int
stats_ns_clear(stats_ns_t *ns, stats_type_t type) {
  int cleared = 0;
  void *vc;
  ck_hs_iterator_t iterator = CK_HS_ITERATOR_INITIALIZER;
  while(ck_hs_next(&ns->map, &iterator, &vc)) {
    stats_container_t *c = vc;
    if(c->ns) cleared += stats_ns_clear(c->ns, type);
    if(c->handle && c->handle->type == type) {
      if(stats_handle_clear(c->handle)) cleared++;
    }
  }
  return cleared;
}

int
stats_recorder_clear(stats_recorder_t *rec, stats_type_t type) {
  return stats_ns_clear(rec->global, type);
}

#define OUTF(cl,k,l,a) do { \
  int rv = outf((cl), (k), (l)); \
  if(rv != (l)) return -1; \
  (a) += rv; \
} while(0)

#define OUTB(cl,k,l,a,label) do { \
  int rv = outf((cl), (k), (l)); \
  if(rv != (l)) goto label; \
  (a) += rv; \
} while(0)

/* yajl_string_encode is borrowed and hacked from libyajl
 *
 * Copyright (c) 2007-2014, Lloyd Hilaiel <me@lloyd.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
static ssize_t
yajl_string_encode(ssize_t (*outf)(void *, const char *, size_t),
                   void *cl,
                   const char * sstr,
                   size_t len) {
  const unsigned char * str = (const unsigned char *)sstr;
  ssize_t written = 0;
  static const char * hexchar = "0123456789ABCDEF";
  size_t beg = 0;
  size_t end = 0;
  char hexbuf[7] = { '\\', 'u', '0', '0', '0', '0', 0 };

  while (end < len) {
    const char * escaped = NULL;
    switch (str[end]) {
      case '\r': escaped = "\\r"; break;
      case '\n': escaped = "\\n"; break;
      case '\\': escaped = "\\\\"; break;
      case '"': escaped = "\\\""; break;
      case '\f': escaped = "\\f"; break;
      case '\b': escaped = "\\b"; break;
      case '\t': escaped = "\\t"; break;
      default:
        if ((unsigned char) str[end] < 32) {
          hexbuf[4] = hexchar[(uint8_t)str[end] >> 4];
          hexbuf[5] = hexchar[(uint8_t)str[end] & 0xf];
          escaped = hexbuf;
        }
        break;
    }
    if (escaped != NULL) {
      OUTF(cl, (const char *) (str + beg), end - beg, written);
      OUTF(cl, escaped, (unsigned int)strlen(escaped), written);
      beg = ++end;
    } else {
      ++end;
    }
  }
  OUTF(cl, (const char *) (str + beg), end - beg, written);
  return written;
}

static ssize_t
stats_val_output_json(stats_handle_t *h, bool hist_since_last,
                      ssize_t (*outf)(void *, const char *, size_t), void *cl) {
  ssize_t written = 0, rv, len;
  int fpclass;
  char buff[64];
  char string_copy[4096];

  if(h->cb) {
    h->cb(h, &h->valueptr, h->cb_closure);
  }

  if(h->valueptr == NULL ||
     (h->type == STATS_TYPE_STRING && *((char **)h->valueptr) == NULL)) {
    OUTF(cl, "null", 4, written);
    return written;
  }
  switch(h->type) {
  case STATS_TYPE_STRING:
    OUTF(cl,"\"",1,written);
    ck_spinlock_ticket_lock(&h->spinlock);
    len = strlen(*(char **)h->valueptr);
    if(len >= sizeof(string_copy)) len = sizeof(string_copy)-1;
    memcpy(string_copy, *(char **)h->valueptr, len);
    ck_spinlock_ticket_unlock(&h->spinlock);
    string_copy[len] = '\0';
    rv = yajl_string_encode(outf, cl, string_copy, len);
    if(rv < 0) return -1;
    written += rv;
    OUTF(cl,"\"",1,written);
    break;
  case STATS_TYPE_INT32:
    len = snprintf(buff, sizeof(buff), "%d", *(int32_t *)h->valueptr);
    OUTF(cl,buff,len,written);
    break;
  case STATS_TYPE_UINT32:
    len = snprintf(buff, sizeof(buff), "%u", *(uint32_t *)h->valueptr);
    OUTF(cl,buff,len,written);
    break;
  case STATS_TYPE_INT64:
    len = snprintf(buff, sizeof(buff), "%" PRId64, *(int64_t *)h->valueptr);
    OUTF(cl,buff,len,written);
    break;
  case STATS_TYPE_COUNTER:
  {
    int i;
    uint64_t sum = 0;
    for(i=0;i<h->fanout;i++)
      sum += ck_pr_load_64(&h->fan[i].cpu.incr);
    len = snprintf(buff, sizeof(buff), "%" PRIu64, sum);
    OUTF(cl,buff,len,written);
    break;
  }
  case STATS_TYPE_UINT64:
    len = snprintf(buff, sizeof(buff), "%" PRIu64, *(uint64_t *)h->valueptr);
    OUTF(cl,buff,len,written);
    break;
  case STATS_TYPE_DOUBLE:
    fpclass = fpclassify(*(double *)h->valueptr);
    if(fpclass == FP_INFINITE || fpclass == FP_NAN) {
      OUTF(cl, "null", 4, written);
    } else {
      len = snprintf(buff, sizeof(buff), "%g", *(double *)h->valueptr);
      OUTF(cl,buff,len,written);
    }
    break;
  case STATS_TYPE_HISTOGRAM_FAST:
  case STATS_TYPE_HISTOGRAM:
    {
      int i;
      bool needs_comma = false;
      histogram_t *interest = h->hist_aggr;
      histogram_t *copy = hist_alloc_nbins(h->last_size * h->fanout); // upper bound
      for(i=0;i<h->fanout;i++) {
        const histogram_t * const * hptr = (const histogram_t * const *)&h->fan[i].cpu.hist;
        ck_spinlock_ticket_lock(&h->fan[i].cpu.spinlock);
        hist_accumulate(copy, hptr, 1);
        if(hist_since_last) {
          hist_clear(h->fan[i].cpu.hist);
        }
        ck_spinlock_ticket_unlock(&h->fan[i].cpu.spinlock);
      }
      if(hist_since_last) hist_accumulate(h->hist_aggr, (const histogram_t *const *)&copy, 1);
      else hist_accumulate(copy, (const histogram_t *const *)&h->hist_aggr, 1);
      OUTB(cl, "[", 1, written, bail);
      h->last_size = hist_bucket_count(copy);
      for(i=0;i<hist_bucket_count(copy);i++) {
        uint64_t cnt;
        double val;
        if(hist_bucket_idx(copy, i, &val, &cnt)) {
          len = snprintf(buff, sizeof(buff), "%s\"H[%0.2g]=%" PRIu64 "\"",
                         needs_comma ? "," : "", val, cnt);
          needs_comma = true;
          OUTB(cl,buff,len,written,bail);
        }
      }
      OUTB(cl, "]", 1, written,bail);
    bail:
      hist_free(copy);
    }
    break;
  }
  return written;
}

static ssize_t
stats_con_output_json(stats_ns_t *ns, stats_handle_t *h, bool hist_since_last,
                      bool simple,
                      ssize_t (*outf)(void *, const char *, size_t), void *cl) {
  void *vc;
  ssize_t written = 0, ns_written = 0;
  ck_hs_iterator_t iterator = CK_HS_ITERATOR_INITIALIZER;
  stats_ns_update(ns);
  if(!simple) OUTF(cl, "{", 1, written);
  if(ns) {
    if(simple) OUTF(cl, "{", 1, written);
    while(ck_hs_next(&ns->map, &iterator, &vc)) {
      stats_container_t *c = vc;
      if(!simple || c->ns != NULL ||
         (c->handle->type != STATS_TYPE_HISTOGRAM &&
          c->handle->type != STATS_TYPE_HISTOGRAM_FAST)) {
        if(ns_written) OUTF(cl, ",", 1, written);
        OUTF(cl, "\"", 1, written);
        OUTF(cl, c->key, c->len, written);
        OUTF(cl, "\":", 2, written);
        ns_written = stats_con_output_json(c->ns, c->handle, hist_since_last, simple, outf, cl);
        if(ns_written < 0) return -1;
        written += ns_written;
      }
    }
    if(simple) OUTF(cl, "}", 1, written);
  }
  if(h && (!ns || !simple)) {
    if(!simple) {
      if(ns_written) OUTF(cl, ",", 1, written);
      OUTF(cl, "\"_type\":\"", 9, written);
      switch(h->type) {
        case STATS_TYPE_STRING: OUTF(cl, "s", 1, written); break;
        case STATS_TYPE_INT32: OUTF(cl, "i", 1, written); break;
        case STATS_TYPE_UINT32: OUTF(cl, "I", 1, written); break;
        case STATS_TYPE_INT64: OUTF(cl, "l", 1, written); break;
        case STATS_TYPE_COUNTER:
        case STATS_TYPE_UINT64: OUTF(cl, "L", 1, written); break;
        case STATS_TYPE_DOUBLE:
        case STATS_TYPE_HISTOGRAM_FAST:
        case STATS_TYPE_HISTOGRAM: OUTF(cl, "n", 1, written); break;
      }
      OUTF(cl, "\",\"_value\":", 11, written);
    }
    if(!simple || (h->type != STATS_TYPE_HISTOGRAM &&
                   h->type != STATS_TYPE_HISTOGRAM_FAST)) {
      ssize_t rv = stats_val_output_json(h, hist_since_last, outf, cl);
      if(rv < 0) return -1;
      written += rv;
    }
  }
  if(!simple) OUTF(cl, "}", 1, written);
  return written;
}
ssize_t
stats_recorder_output_json(stats_recorder_t *rec,
                           bool hist_since_last, bool simple,
                           ssize_t (*outf)(void *, const char *, size_t), void *cl) {
  return stats_con_output_json(rec->global, NULL, hist_since_last, simple, outf, cl);
}

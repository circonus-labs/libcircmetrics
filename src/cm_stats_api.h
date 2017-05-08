/*
 * Copyright (c) 2016, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonus, Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CM_STATS_API_H
#define CM_STATS_API_H

#include <stdbool.h>
#include <stdint.h>

typedef struct stats_recorder_t stats_recorder_t;
typedef struct stats_ns_t stats_ns_t;
typedef struct stats_handle_t stats_handle_t;

typedef enum stats_type_t {
  STATS_TYPE_STRING,
  STATS_TYPE_INT32,
  STATS_TYPE_UINT32,
  STATS_TYPE_INT64,
  STATS_TYPE_UINT64,
  STATS_TYPE_COUNTER,
  STATS_TYPE_DOUBLE,
  STATS_TYPE_HISTOGRAM,
  STATS_TYPE_HISTOGRAM_FAST
} stats_type_t;

/* Allocate a recorder object */
stats_recorder_t *
  stats_recorder_alloc(void);

/* Attempt to clear all handles within a recorder.
 * Returns the numner of handles cleared.
 */
int
  stats_recorder_clear(stats_recorder_t *rec, stats_type_t);

/* Get the global namespace for the recorder */
stats_ns_t *
  stats_recorder_global_ns(stats_recorder_t *);

/* Register a name space, optionally within an existing namespace.
 * If the key is taken by something other than a namespace, NULL is returned.
 */
stats_ns_t *
  stats_register_ns(stats_recorder_t *, stats_ns_t *, const char *);

/* Deregister and free a namespace.
 * Results are undefined if ns is still being used during or after this call
 * if the return value is true.
 * This function deregisters ns from parent within rec. If successful, ns is
 * freed.
 */
bool
  stats_deregister_ns(stats_recorder_t *rec, stats_ns_t *parent, stats_ns_t *ns);

typedef void (*stats_ns_update_func_t)(stats_ns_t *, void *closure);

/* Functions registered in this way will fire before a namespace is walked for
 * dumping.  This is particularly useful if you have a set of variables within
 * a namespace being observed that need refreshing. e.g. exposing metrics
 * for each of a large number of struct members that are updated by a single
 * call.
 */
bool
  stats_ns_invoke(stats_ns_t *, stats_ns_update_func_t, void *closure);

/* Register a "metric" under the namespace,
 * creating if nothing exists under that key
 * returning an existing handle if one exists and has the same type
 * returning NULL otherwise
 * The fanout variant controls the number of concurrency slots to fan across.
 */
stats_handle_t *
  stats_register(stats_ns_t *, const char *name, stats_type_t);

stats_handle_t *
  stats_register_fanout(stats_ns_t *, const char *name, stats_type_t,
                               int fanout);

/* If possible clear the handle to an initial state.
 * If you looking at some bit of memory for your handle,
 * this will fail as it would be dangerous for the library
 * to intrusively set it.  In that case you own the memory
 * and you can reset it yourself.
 */
bool
  stats_handle_clear(stats_handle_t *);

/* Returns the registered type of the handle */
stats_type_t
  stats_handle_type(stats_handle_t *);

/* Tells the handle to observe the memory at the specified location
 * it will cast the memory based on the stats_type_t of the handle.
 * It should be the address of the type such as `int32_t *` or `char **`
 */
bool
  stats_observe(stats_handle_t *, stats_type_t, void *memory);

#define stats_rob_i32(ns,name,vptr) stats_observe(stats_register(ns,name,STATS_TYPE_INT32), STATS_TYPE_INT32, vptr)
#define stats_rob_u32(ns,name,vptr) stats_observe(stats_register(ns,name,STATS_TYPE_UINT32), STATS_TYPE_UINT32, vptr)
#define stats_rob_i64(ns,name,vptr) stats_observe(stats_register(ns,name,STATS_TYPE_INT64), STATS_TYPE_INT64, vptr)
#define stats_rob_u64(ns,name,vptr) stats_observe(stats_register(ns,name,STATS_TYPE_UINT64), STATS_TYPE_UINT64, vptr)
#define stats_rob_d(ns,name,vptr) stats_observe(stats_register(ns,name,STATS_TYPE_DOUBLE), STATS_TYPE_DOUBLE, vptr)
#define stats_rob_str(ns,name,vptr) stats_observe(stats_register(ns,name,STATS_TYPE_STRING), STATS_TYPE_STRING, vptr)

/* The invocation function is called when statistics are requested from
 * the system. Closure is passed through from invoke registration.
 *
 * `h` is the handle, it is recommended that you use a _set_ function
 * on it to change the underlying value.
 * `ptr` is a point to the underlying value pointer which is, itself,
 * a pointer to the datatype; modify with care.  If the type is a simple
 * scaler (not string, not histogram), then you can change the underlying
 * value directly; e.g for `STATS_TYPE_INT64` one could directly set it
 * via `**(int64_t **)ptr = 42`.
 */
typedef void (*stats_invocation_func_t)
  (stats_handle_t *h, void *ptr, void *closure);

/* Tells the handle to invoke the specified function to determine a value */
bool
  stats_invoke(stats_handle_t *, stats_invocation_func_t, void *closure);

/* Set the handle to a specific value. Minimal effort is made to convert
 * between numeric types and compose into histograms.
 * Setting to NULL will clear the value and, in the case of histograms,
 * will clear the histogram entirely.
 */
bool
  stats_set(stats_handle_t *, stats_type_t, void *);

/* Add (possibly negative) amount to an underlying handle value.
 * Returns false if types mismatch such that an addition can't happen.
 */
bool
  stats_add32(stats_handle_t *, int32_t);
bool
  stats_add64(stats_handle_t *, int64_t);

#define stats_set_i32(h, v) do { \
  int32_t vp = (int32_t)v; \
  stats_set(h, STATS_TYPE_INT32, &vp); \
} while(0)

#define stats_set_u32(h, v) do { \
  uint32_t vp = (uint32_t)v; \
  stats_set(h, STATS_TYPE_UINT32, &vp); \
} while(0)
  
#define stats_set_i64(h, v) do { \
  int64_t vp = (int64_t)v; \
  stats_set(h, STATS_TYPE_INT64, &vp); \
} while(0)
  
#define stats_set_u64(h, v) do { \
  uint64_t vp = (uint64_t)v; \
  stats_set(h, STATS_TYPE_UINT64, &vp); \
} while(0)

#define stats_set_d(h, v) do { \
  double vp = (double)v; \
  stats_set(h, STATS_TYPE_DOUBLE, &vp); \
} while(0)

#define stats_set_str(h, v) do { \
  char *vp = (char *)v; \
  stats_set(h, STATS_TYPE_STRING, vp); \
} while(0)

/* Specific to histograms, set with a count.
 * Setting a double value requires FP work
 */
bool
  stats_set_hist(stats_handle_t *h, double d, uint64_t cnt);
/* Setting via integer scaling is faster.
 * (..., 1500, -3, 1) will add one sample to bin 1.5 
 */
bool
  stats_set_hist_intscale(stats_handle_t *h, int64_t val, int scale, uint64_t cnt);

/* Prints a simple name for a statistics type */
const char *
  stats_type_name(stats_type_t);

/* Prints json via the outf function
 * hist_since_last as true will only show the histogram counts since last
 * invocation, false will show over all of time.
 * simple dictates simpl key value pairs without type information. It also
 * precludes having values are branches of the JSON tree.
 */
ssize_t
  stats_recorder_output_json(stats_recorder_t *rec,
                             bool hist_since_last, bool simple,
                             ssize_t (*outf)(void *, const char *, size_t),
                             void *cl);

#endif

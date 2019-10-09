# libcircmetrics

libcircmetrics provides a simple and user-friendly statistics tracking API
for applications.  It is also focuses on supporting state-of-the-art
datatypes, and being high-performance, contention-avoiding, and thread-safe.

## High Level

The library supports exposing name values where those values are textual,
numeric (various native C integer types and doubles), and log-linear histograms
via the [circllhist](https://github.com/circonus-labs/libcircllhist)
implementation.

Metric names are exposed both hierarchically, as is common in legacy systems
(e.g. `acme.users.login.count`), and "Metrics 2.0"-style tagged metrics (.e.g.,
`count{app=acme,subsystem=users,operation=login}`).  Tags are exposed in
Circonus' preferred format:
`count|ST[app:acme,operation:login,subsystem:users]`.

It is highly recommended that programmers attach units in a standard way via a
`units` tag using the library's [standard set of units](units.md).

> Circonus supports multi-value tags, so a metric can be tagged with `team:a`
> and `team:b` simultaneously.  If your monitoring system is incapable of
> handling this type of tagging, be mindful.

Aside from nomenclature and metric name formatting, there is no reliance on the
Circonus platform whatsoever.  The library is standalone and makes it easy to
track metrics and expose them over JSON.

## Usage

It all starts with a "stats recorder."  An application can have any number
of these, but most often a single recorder is sufficient.

```c
stats_recorder_t *rec;
rec = stats_recorder_alloc(); 
```

Next we create a namespace.  A namespace is part of the metric's heirarchical
name.

```c
stats_ns_t *root, *appns, *apins;
root = stats_recorder_global_ns(rec);
appns = stats_register_ns(rec, root, "mycoolapp");
stats_ns_add_tag(appns, "app", "mycoolapp");
apins = stats_register_ns(rec, app, "api");
stats_ns_add_tag(apins, "subsystem", "api");
```

This has created a hierarchy of `mycoolapp.api`. The `stats_ns_add_tag` call
will attach a tag `app:mycoolapp` to that branch (and thus all children) of the
metrics tree. Likewise, any metrics registered under the `api` namespace will
have both the `app:mycoolapp` tag as well as the `subsystem:api` tag.

Now we will put three metrics under this:
 1. A version for our application under the `mycoolapp` namespace.
 1. A count of API calls under the `api` namespace.
 1. A histogram of latencies.

Additionally we will inform the library what the tagged variant of each of
these metrics is.

```c
const char *version_string = "v1.9.2";
stats_handle_t *version_handle = stats_register(appns, "version", STATS_TYPE_STRING);
stats_observe(version_handle, STATS_TYPE_STRING, &version_string);
```
This will create a metric `mycoolapp.version` that observes the
`version_string` symbol and reports its value (even if it changes).  The fully
qualified, tagged variant of this metric is `version|ST[app:mycoolapp]`.

```c
stats_handle_t *api_req_counter;

/* initialize */
api_req_counter = stats_register(apins, "calls", STATS_TYPE_COUNTER);
stats_handle_add_tag(api_req_counter, "units", STATS_UNITS_REQUESTS);

/* in the API service function */
stats_add64(api_req_counter, 1);
```

This will expose an `api_req_counter` handle to the application on which it can
"count" things using the `stats_add32` or `stats_add64` functions.  These
functions are designed to reduce multi-threaded contention and are lock-free.
This metric is called `mycoolapp.api.calls` or in tagged form
`calls|ST[app:mycoolapp,subsystem:api,units:requests]`.

```c
stats_handle_t *api_latency;

/* initialize */
api_latency = stats_register(apins, "latency", STATS_TYPE_HISTOGRAM_FAST);
stats_handle_add_tag(api_latency, "units", STATS_UNITS_SECONDS);

/* in the API service function */
uint64_t start_ns = get_nanos();
/* do work */
stats_set_hist_intscale(api_latency, get_nanos() - start_ns, -9, 1);
```

This will track the latency of every API call into a histogram called
`mycoolapp.api.latency` or in tagged form,
`latency|ST[app:mycoolapp,subsystem:api,units:seconds]`.  The "intscale"
variant of histogram insertion allows us to insert nanoseconds into a metric
tracking seconds by specifying that it should be scaled by 10<sup>-9</sup>.

## Extraction

As a standalone library, libcircmetrics provides a functional writer mechanism
to produce a JSON document with all the data in it.

The "simple" format is `{ "key": value }` in JSON.  However, due to JSON's
horribly poor specification around numbers, it is often useful to know what
type the measurement is, so the "not simple" format is: `{ "key": { "_type":
"T", "_value": "V" }}` where T is `s` for strings, `i`, `I`, `l`, `L`, or `n`
for `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, or `double`, respectively, or
`H` for histograms.

```c
static ssize_t write_to_fd(void *closure, const char *buf, size_t len) {
  int *fd = (int *)closure;
  return write(fd, buf, len);
}


int fd = STDOUT_FILENO;
/* print a tagged JSON document to stdout */
stats_recorder_output_json_tagged(rec, false, write_to_fd, &fd);

/* print an untagged (hierarchical) JSON document to stdout */
bool simple = false;
stats_recorder_output_json(rec, false, simple, write_to_fd, &fd);
```

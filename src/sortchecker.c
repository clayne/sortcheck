#include <checksum.h>
#include <proc_info.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>

#include <dlfcn.h>
#include <syslog.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

enum CheckFlags {
  CHECK_BASIC        = 1 << 0,
  CHECK_REFLEXIVITY  = 1 << 1,
  CHECK_SYMMETRY     = 1 << 2,
  CHECK_TRANSITIVITY = 1 << 3,
  CHECK_SORTED       = 1 << 4,
  CHECK_GOOD_BSEARCH = 1 << 5,
  CHECK_DEFAULT      = CHECK_BASIC | CHECK_SYMMETRY
                       | CHECK_TRANSITIVITY | CHECK_SORTED,
  CHECK_ALL          = 0xffffffff,
};

// Runtime options
static int debug = 0;
static int max_errors = 10;
static int print_to_syslog = 0;
static unsigned check_flags = CHECK_DEFAULT;

// Other pieces of state
static int init_done = 0;
static ProcMap *maps = 0;
static char *proc_name = 0;
static char *proc_cmdline = 0;
static size_t nmaps = 0;
static int num_errors = 0;
static long proc_pid = -1;
static int do_report_error = 1;

static void fini(void) {
  if(maps)
    free(maps);
  if(proc_cmdline)
    free(proc_cmdline);
  if(proc_name)
    free(proc_name);
}

static void init(void) {
  const char *opts;
  if((opts = getenv("SORTCHECK_OPTIONS"))) {
    char *opts_ = strdup(opts);

    char *cur;
    for(cur = opts_; *cur; ) {
      // SORTCHECK_OPTIONS is a colon-separated list of assignments

      const char *name = cur;

      char *assign = strchr(cur, '=');
      if(!assign) {
        fprintf(stderr, "sortcheck: missing '=' in '%s'\n", cur);
        exit(1);
      }
      *assign = 0;

      char *value = assign + 1;

      char *end = strchr(value, ':');
      if(end)
        *end = 0;
      cur = end ? end + 1 : value + strlen(value);

      if(0 == strcmp(name, "debug")) {
        debug = atoi(value);
      } else if(0 == strcmp(name, "print_to_syslog")) {
        print_to_syslog = atoi(value);;
        openlog("", 0, LOG_USER);
      } else if(0 == strcmp(name, "report_error")) {
        do_report_error = atoi(value);
      } else if(0 == strcmp(name, "max_errors")) {
        max_errors = atoi(value);
      } else if(0 == strcmp(name, "check")) {
	unsigned flags = 0;
	do {
          char *next = strchr(value, ',');
	  if(next) {
	    *next = 0;
	    ++next;
	  }
	  if(0 == strcmp("basic", value)) {
	    flags |= CHECK_BASIC;
	  } else if(0 == strcmp("reflexivity", value)) {
	    flags |= CHECK_REFLEXIVITY;
	  } else if(0 == strcmp("symmetry", value)) {
	    flags |= CHECK_SYMMETRY;
	  } else if(0 == strcmp("transitivity", value)) {
	    flags |= CHECK_SYMMETRY;
	  } else if(0 == strcmp("sort", value)) {
	    flags |= CHECK_SORTED;
	  } else if(0 == strcmp("good_bsearch", value)) {
            flags |= CHECK_GOOD_BSEARCH;
	  } else if(0 == strcmp("default", value)) {
            flags |= CHECK_DEFAULT;
	  } else {
	    fprintf(stderr, "sortcheck: unknown check '%s'\n", value);
	    exit(1);
	  }
	  value = next;
	} while(value);
	check_flags = flags;
      } else {
        fprintf(stderr, "sortcheck: unknown option '%s'\n", name);
        exit(1);
      }
    }
    free(opts_);
  }

  // Get mappings
  maps = get_proc_maps(&nmaps);
  if(debug) {
    fputs("Process map:\n", stderr);
    size_t i;
    for(i = 0; i < nmaps; ++i)
      fprintf(stderr, "  %50s: %p-%p\n", &maps[i].name[0], maps[i].begin_addr, maps[i].end_addr);
  }

  get_proc_cmdline(&proc_name, &proc_cmdline);

  proc_pid = (long)getpid();

  atexit(fini);

  init_done = 1;
}

typedef struct {
  const char *func;
  const void *cmp_addr;
  const char *cmp_module;
  size_t cmp_offset;
  const void *ret_addr;
  const char *caller_module;
  size_t caller_offset;
} ErrorContext;

static void report_error(ErrorContext *ctx, const char *fmt, ...) {
  if(!do_report_error)
    return;

  va_list ap;
  va_start(ap, fmt);

  if(++num_errors > max_errors)  // Racy but ok
    return;

  if(!ctx->cmp_module) {
    // Lazily compute modules (no race!)

    const ProcMap *map_for_cmp = find_proc_map_for_addr(maps, nmaps, ctx->cmp_addr);
    ctx->cmp_module = map_for_cmp ? &map_for_cmp->name[0] : "<unknown>";
    ctx->cmp_offset = map_for_cmp ? ((const char *)ctx->cmp_addr - (const char *)map_for_cmp->begin_addr) : 0;

    const ProcMap *map_for_caller = find_proc_map_for_addr(maps, nmaps, ctx->ret_addr);
    ctx->caller_module = map_for_caller ? &map_for_caller->name[0] : "<unknown>";
    ctx->caller_offset = map_for_caller ? ((const char *)ctx->ret_addr - (const char *)map_for_caller->begin_addr) : 0;
  }

  char body[128];
  vsnprintf(body, sizeof(body), fmt, ap);

  char buf[256];

  char *full_msg = buf;
  size_t full_msg_size = sizeof(buf);
  size_t i;
  for(i = 0; i < 2; ++i) {
    // TODO: some parts of the message may be precomputed
    size_t need = snprintf(full_msg, full_msg_size, "%s[%ld]: %s: %s (comparison function %p (%s+%zx), called from %p (%s+%zx), cmdline is \"%s\")\n", proc_name, proc_pid, ctx->func, body, ctx->cmp_addr, ctx->cmp_module, ctx->cmp_offset, ctx->ret_addr, ctx->caller_module, ctx->caller_offset, proc_cmdline);
    if(i == 0 && need < sizeof(body))  // Did it fit to local buf?
      break;
    if(i == 0 && need >= sizeof(body)) {  // It didn't - go ahead and malloc
      full_msg_size = need + 1;
      full_msg = malloc(full_msg_size);
    }
  }

  if(print_to_syslog)
    syslog(LOG_WARNING, "%s", full_msg);
  else
    fputs(full_msg, stderr);

  if(full_msg != buf)
    free(full_msg);
}

typedef int (*cmp_fun_t)(const void *, const void *);

static inline int sign(int x) {
  return x < 0 ? -1 : x > 0 ? 1 : 0;
}

// Check that comparator is stable and does not modify arguments
static void check_basic(ErrorContext *ctx, cmp_fun_t cmp, const char *key, const void *data, size_t n, size_t sz) {
  if(!(check_flags & CHECK_BASIC))
    return;

  const char *some = key ? key : data;
  size_t i;

  // Check for modifying comparison functions
  for(i = 0; i < n; ++i) {
    const void *elt = (const char *)data + i * sz;
    unsigned cs = checksum(elt, sz);
    cmp(some, elt);
    if(cs != checksum(elt, sz)) {
      report_error(ctx, "comparison function modifies data");
      break;
    }
    if(!key || (check_flags & CHECK_GOOD_BSEARCH)) {
      cmp(elt, elt);
      if(cs != checksum(elt, sz)) {
        report_error(ctx, "comparison function modifies data");
        break;
      }
    }
  }

  // Check for non-constant return value
  for(i = 0; i < n; ++i) {
    const void *elt = (const char *)data + i * sz;
    if(cmp(some, elt) != cmp(some, elt)) {
      report_error(ctx, "comparison function returns unstable results");
      break;
    }
  }
}

// Check that comparator is reflexive i.e. respects self-application
static void check_reflex(ErrorContext *ctx, cmp_fun_t cmp, const char *key, const void *data, size_t n, size_t sz) {
  if(!(check_flags & CHECK_REFLEXIVITY))
    return;

  // Can check only good bsearch callbacks
  if(key && !(check_flags & CHECK_GOOD_BSEARCH))
    return;

  size_t i;
  for(i = 0; i < n; ++i) {
    const void *elt = (const char *)data + i * sz;
    // TODO: it may make sense to compare different but equal elements?
    if(0 != cmp(elt, elt)) {
      report_error(ctx, "comparison function returns non-zero for equal elements");
      break;
    }
  }
}

// Check that array is sorted
static void check_sorted(ErrorContext *ctx, cmp_fun_t cmp, const char *key, const void *data, size_t n, size_t sz) {
  if(!(check_flags & CHECK_SORTED))
    return;

  if(key) {
    int order = 1;
    size_t i;
    for(i = 0; i < n; ++i) {
      const void *elt = (const char *)data + i * sz;
      int new_order = sign(cmp(key, elt));
      if(new_order > order) {
        report_error(ctx, "processed array is not sorted at index %zd", i);
        return;  // Return to stop further error reporting
      }
      order = new_order;
    }
  }

  if(!key || (check_flags & CHECK_GOOD_BSEARCH)) {
    size_t i;
    for(i = 1; i < n; ++i) {
      const void *elt = (const char *)data + i * sz;
      const void *prev = (const char *)elt - sz;
      if(cmp(prev, elt) > 0) {
        report_error(ctx, "processed array is not sorted at index %zd", i);
        break;
      }
    }
  }
}

// Check that ordering is total
static void check_total(ErrorContext *ctx, cmp_fun_t cmp, const char *key, const void *data, size_t n, size_t sz) {
  // Can check only good bsearch callbacks
  if(key && !(check_flags & CHECK_GOOD_BSEARCH))
    return;

  // TODO: 2 bits enough for status
  int8_t cmp_[32][32];
  n = n > 32 ? 32 : n;
  memset(cmp_, 0, sizeof(cmp_));

  size_t i, j, k;
  for(i = 0; i < n; ++i)
  for(j = 0; j < n; ++j) {
    const void *a = (const char *)data + i * sz;
    const void *b = (const char *)data + j * sz;
    cmp_[i][j] = sign(cmp(a, b));
  }

  // Following axioms from http://mathworld.wolfram.com/StrictOrder.html

  // Totality by construction

  // Irreflexivity + Asymmetry
  if(check_flags & CHECK_TRANSITIVITY) {
    for(i = 0; i < n; ++i)
    for(j = 0; j < n; ++j) {
      if(!(check_flags & CHECK_REFLEXIVITY) && i == j)
	continue;

      if(cmp_[i][j] != -cmp_[j][i]) {
        report_error(ctx, "comparison function is not symmetric");
        goto sym_check_done;
      }
    }
  }
sym_check_done:

  // Transitivity
  if(check_flags & CHECK_TRANSITIVITY) {
    // FIXME: slow slow...
    for(i = 0; i < n; ++i)
    for(j = 0; j < n; ++j)
    for(k = 0; k < n; ++k) {
      if(cmp_[i][j] == cmp_[j][k] && cmp_[i][j] != cmp_[i][k]) {
        report_error(ctx, "comparison function is not transitive");
        goto trans_check_done;
      }
    }
  }
trans_check_done:

  return;
}

#define GET_REAL(sym)                                        \
  static typeof(sym) *_real;                                 \
  if(!_real) {                                               \
    _real = (typeof(sym) *)dlsym(RTLD_NEXT, #sym);           \
    if(debug)                                                \
      fprintf(stderr, "Hello from %s interceptor!\n", #sym); \
  }

#define MAYBE_INIT do {  \
  if(!init_done) init(); \
} while(0)

EXPORT void *bsearch(const void *key, const void *data, size_t n, size_t sz, cmp_fun_t cmp) {
  MAYBE_INIT;
  GET_REAL(bsearch);
  if(num_errors < max_errors) {
    ErrorContext ctx = { __func__, cmp, 0, 0, __builtin_return_address(0), 0, 0 };
    check_basic(&ctx, cmp, key, data, n, sz);
    check_reflex(&ctx, cmp, key, data, n, sz);
    check_total(&ctx, cmp, key, data, n, sz);  // manpage does not require this but still
    check_sorted(&ctx, cmp, key, data, n, sz);
  }
  return _real(key, data, n, sz, cmp);
}

EXPORT void qsort(void *data, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
  MAYBE_INIT;
  GET_REAL(qsort);
  if(num_errors < max_errors) {
    ErrorContext ctx = { __func__, cmp, 0, 0, __builtin_return_address(0), 0, 0 };
    check_basic(&ctx, cmp, 0, data, n, sz);
    check_reflex(&ctx, cmp, 0, data, n, sz);
    check_total(&ctx, cmp, 0, data, n, sz);
  }
  _real(data, n, sz, cmp);
}

// TODO: qsort_r


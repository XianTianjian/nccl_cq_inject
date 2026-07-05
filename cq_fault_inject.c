#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IBVERBS_VERSION "IBVERBS_1.1"

typedef struct ibv_context* (*ibv_open_device_fn)(struct ibv_device* device);
typedef int (*poll_cq_fn)(struct ibv_cq* cq, int num_entries, struct ibv_wc* wc);

struct ibv_context* ibv_open_device(struct ibv_device* device);
#if defined(__linux__)
typedef void* (*dlvsym_fn)(void* handle, const char* symbol, const char* version);
void* dlvsym(void* handle, const char* symbol, const char* version);
static dlvsym_fn g_real_dlvsym;
static __thread int g_resolving_dlvsym;

static dlvsym_fn get_real_dlvsym(void) {
  if (g_real_dlvsym != NULL) return g_real_dlvsym;
  if (g_resolving_dlvsym) return NULL;

  g_resolving_dlvsym = 1;
  void* real = dlsym(RTLD_NEXT, "dlvsym");
  g_resolving_dlvsym = 0;

  if (real != NULL && real != (void*)dlvsym) {
    g_real_dlvsym = (dlvsym_fn)real;
  }
  return g_real_dlvsym;
}
#endif

struct poll_entry {
  struct ibv_context* ctx;
  poll_cq_fn old_poll_cq;
  struct poll_entry* next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct poll_entry* g_poll_entries;
static ibv_open_device_fn g_real_ibv_open_device;

static pthread_once_t g_config_once = PTHREAD_ONCE_INIT;
static int g_enabled = 1;
static int g_verbose = 1;
static int g_status = IBV_WC_RETRY_EXC_ERR;
static uint32_t g_vendor_err = 0x71;
static unsigned long g_on_nth = 0;
static atomic_ulong g_seen = 0;

static void init_config(void);

static void cqfi_log(const char* fmt, ...) {
  pthread_once(&g_config_once, init_config);
  if (!g_verbose) return;

  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[inject] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static long parse_long_env(const char* name, long fallback) {
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0') return fallback;

  char* end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0') return fallback;
  return parsed;
}

static int parse_bool_env(const char* name, int fallback) {
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0') return fallback;
  if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
      strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
    return 0;
  }
  return 1;
}

static int parse_status_env(void) {
  const char* value = getenv("NCCL_CQFI_STATUS");
  if (value == NULL || value[0] == '\0' || strcasecmp(value, "retry") == 0 ||
      strcasecmp(value, "retry_exc") == 0 || strcasecmp(value, "retry_exc_err") == 0) {
    return IBV_WC_RETRY_EXC_ERR;
  }
  if (strcasecmp(value, "flush") == 0 || strcasecmp(value, "wr_flush") == 0 ||
      strcasecmp(value, "wr_flush_err") == 0) {
    return IBV_WC_WR_FLUSH_ERR;
  }
  if (strcasecmp(value, "general") == 0 || strcasecmp(value, "general_err") == 0) {
    return IBV_WC_GENERAL_ERR;
  }

  char* end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 0);
  if (errno == 0 && end != value && *end == '\0') return (int)parsed;

  return IBV_WC_RETRY_EXC_ERR;
}

static void init_config(void) {
  g_enabled = parse_bool_env("NCCL_CQFI_ENABLE", 1);
  g_verbose = parse_bool_env("NCCL_CQFI_VERBOSE", 1);
  g_status = parse_status_env();
  g_vendor_err = (uint32_t)parse_long_env("NCCL_CQFI_VENDOR_ERR", 0x71);
  g_on_nth = (unsigned long)parse_long_env("NCCL_CQFI_ON", 0);

  if (g_verbose) {
    fprintf(stderr,
            "[inject] config enabled=%d status=%d vendor_err=0x%x on_nth=%lu\n",
            g_enabled, g_status, g_vendor_err, g_on_nth);
  }
}

static poll_cq_fn find_old_poll_cq_locked(struct ibv_context* ctx) {
  for (struct poll_entry* entry = g_poll_entries; entry != NULL; entry = entry->next) {
    if (entry->ctx == ctx) return entry->old_poll_cq;
  }
  return NULL;
}

static poll_cq_fn find_old_poll_cq(struct ibv_context* ctx) {
  pthread_mutex_lock(&g_lock);
  poll_cq_fn old_poll = find_old_poll_cq_locked(ctx);
  pthread_mutex_unlock(&g_lock);
  return old_poll;
}

static int should_inject_one(void) {
  if (g_on_nth == 0) return 0;
  unsigned long seen = atomic_fetch_add_explicit(&g_seen, 1, memory_order_relaxed) + 1;
  return seen == g_on_nth;
}

static int my_poll_cq_inject(struct ibv_cq* cq, int num_entries, struct ibv_wc* wc) {
  struct ibv_context* ctx = cq ? cq->context : NULL;
  poll_cq_fn old_poll = find_old_poll_cq(ctx);
  if (old_poll == NULL) {
    cqfi_log("missing original poll_cq for ctx=%p cq=%p", (void*)ctx, (void*)cq);
    return -1;
  }

  int ret = old_poll(cq, num_entries, wc);
  if (!g_enabled || ret <= 0 || wc == NULL) return ret;

  for (int i = 0; i < ret; i++) {
    if (wc[i].status != IBV_WC_SUCCESS) continue;
    if (!should_inject_one()) continue;

    enum ibv_wc_status old_status = wc[i].status;
    wc[i].status = (enum ibv_wc_status)g_status;
    wc[i].vendor_err = g_vendor_err;
    cqfi_log("injected wc[%d] ctx=%p cq=%p wr_id=%llu old_status=%d new_status=%d vendor_err=0x%x",
             i, (void*)ctx, (void*)cq, (unsigned long long)wc[i].wr_id, old_status, wc[i].status,
             wc[i].vendor_err);
  }

  return ret;
}

static int patch_context(struct ibv_context* ctx) {
  pthread_once(&g_config_once, init_config);
  if (!g_enabled || ctx == NULL) return 0;

  pthread_mutex_lock(&g_lock);

  poll_cq_fn current = ctx->ops.poll_cq;
  poll_cq_fn old = find_old_poll_cq_locked(ctx);
  if (old != NULL) {
    if (current != my_poll_cq_inject) {
      cqfi_log("ctx=%p already tracked, re-applying wrapper old_poll_cq=%p current=%p",
               (void*)ctx, (void*)old, (void*)current);
      ctx->ops.poll_cq = my_poll_cq_inject;
    } else {
      cqfi_log("ctx=%p already patched old_poll_cq=%p", (void*)ctx, (void*)old);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
  }

  if (current == NULL) {
    pthread_mutex_unlock(&g_lock);
    cqfi_log("ctx=%p has null poll_cq; skipping patch", (void*)ctx);
    return -1;
  }
  if (current == my_poll_cq_inject) {
    pthread_mutex_unlock(&g_lock);
    cqfi_log("ctx=%p already points at wrapper but no original poll_cq is recorded", (void*)ctx);
    return -1;
  }

  struct poll_entry* entry = (struct poll_entry*)calloc(1, sizeof(*entry));
  if (entry == NULL) {
    pthread_mutex_unlock(&g_lock);
    cqfi_log("failed to allocate poll entry for ctx=%p", (void*)ctx);
    return -1;
  }

  entry->ctx = ctx;
  entry->old_poll_cq = current;
  entry->next = g_poll_entries;
  g_poll_entries = entry;
  ctx->ops.poll_cq = my_poll_cq_inject;

  pthread_mutex_unlock(&g_lock);
  cqfi_log("patched ctx=%p old_poll_cq=%p new_poll_cq=%p", (void*)ctx, (void*)current,
           (void*)my_poll_cq_inject);
  return 0;
}

static void remember_real_ibv_open_device(void* real) {
  if (real == NULL || real == (void*)ibv_open_device) return;

  pthread_mutex_lock(&g_lock);
  g_real_ibv_open_device = (ibv_open_device_fn)real;
  pthread_mutex_unlock(&g_lock);
}

static ibv_open_device_fn get_real_ibv_open_device(void) {
  pthread_mutex_lock(&g_lock);
  ibv_open_device_fn fn = g_real_ibv_open_device;
  pthread_mutex_unlock(&g_lock);
  if (fn != NULL) return fn;

  void* real = NULL;
#if defined(__linux__)
  dlvsym_fn real_dlvsym = get_real_dlvsym();
  if (real_dlvsym != NULL) {
    real = real_dlvsym(RTLD_NEXT, "ibv_open_device", IBVERBS_VERSION);
  }
#endif
  if (real == NULL) real = dlsym(RTLD_NEXT, "ibv_open_device");
  if (real == NULL || real == (void*)ibv_open_device) return NULL;

  remember_real_ibv_open_device(real);
  return (ibv_open_device_fn)real;
}

static int is_libibverbs_symbol(void* symbol) {
  Dl_info info;
  if (symbol == NULL || symbol == (void*)ibv_open_device) return 0;
  if (dladdr(symbol, &info) == 0 || info.dli_fname == NULL) return 0;
  return strstr(info.dli_fname, "libibverbs.so") != NULL;
}

__attribute__((visibility("default")))
struct ibv_context* ibv_open_device(struct ibv_device* device) {
  pthread_once(&g_config_once, init_config);

  ibv_open_device_fn real_open = get_real_ibv_open_device();
  if (real_open == NULL) {
    cqfi_log("failed to resolve real ibv_open_device");
    return NULL;
  }

  struct ibv_context* ctx = real_open(device);
  cqfi_log("ibv_open_device wrapper called device=%p ctx=%p", (void*)device, (void*)ctx);
  if (ctx != NULL) patch_context(ctx);
  return ctx;
}

#if defined(__linux__)
__attribute__((visibility("default")))
void* dlvsym(void* handle, const char* symbol, const char* version) {
  dlvsym_fn real_dlvsym = get_real_dlvsym();
  if (real_dlvsym == NULL) {
    cqfi_log("failed to resolve real dlvsym for symbol=%s", symbol);
    return NULL;
  }

  void* real = real_dlvsym(handle, symbol, version);
  volatile uintptr_t symbol_addr = (uintptr_t)symbol;
  if (symbol_addr != 0 && strcmp(symbol, "ibv_open_device") == 0) {
    if (is_libibverbs_symbol(real)) {
      remember_real_ibv_open_device(real);
      cqfi_log("dlvsym intercepted ibv_open_device version=%s real=%p wrapper=%p",
               version, real, (void*)ibv_open_device);
      return (void*)ibv_open_device;
    }
    cqfi_log("dlvsym left ibv_open_device unchanged version=%s real=%p", version, real);
  }

  return real;
}
#endif

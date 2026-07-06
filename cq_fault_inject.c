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
#include <limits.h>

#define IBVERBS_VERSION "IBVERBS_1.1"

typedef struct ibv_context* (*ibv_open_device_fn)(struct ibv_device* device);
typedef struct ibv_cq* (*ibv_create_cq_fn)(struct ibv_context* context, int cqe, void* cq_context,
                                           struct ibv_comp_channel* channel, int comp_vector);
typedef int (*ibv_destroy_cq_fn)(struct ibv_cq* cq);
typedef struct ibv_qp* (*ibv_create_qp_fn)(struct ibv_pd* pd, struct ibv_qp_init_attr* qp_init_attr);
typedef int (*ibv_destroy_qp_fn)(struct ibv_qp* qp);
typedef int (*poll_cq_fn)(struct ibv_cq* cq, int num_entries, struct ibv_wc* wc);

struct ibv_context* ibv_open_device(struct ibv_device* device);
struct ibv_cq* ibv_create_cq(struct ibv_context* context, int cqe, void* cq_context,
                             struct ibv_comp_channel* channel, int comp_vector);
int ibv_destroy_cq(struct ibv_cq* cq);
struct ibv_qp* ibv_create_qp(struct ibv_pd* pd, struct ibv_qp_init_attr* qp_init_attr);
int ibv_destroy_qp(struct ibv_qp* qp);
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

struct cq_entry {
  struct ibv_cq* cq;
  int cqe;
  struct cq_entry* next;
};

struct qp_entry {
  struct ibv_qp* qp;
  uint32_t qp_num;
  struct ibv_cq* send_cq;
  struct ibv_cq* recv_cq;
  int qp_type;
  uint32_t max_send_wr;
  uint32_t max_recv_wr;
  char dev_name[64];
  struct qp_entry* next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct poll_entry* g_poll_entries;
static struct cq_entry* g_cq_entries;
static struct qp_entry* g_qp_entries;
static ibv_open_device_fn g_real_ibv_open_device;
static ibv_create_cq_fn g_real_ibv_create_cq;
static ibv_destroy_cq_fn g_real_ibv_destroy_cq;
static ibv_create_qp_fn g_real_ibv_create_qp;
static ibv_destroy_qp_fn g_real_ibv_destroy_qp;

static pthread_once_t g_config_once = PTHREAD_ONCE_INIT;
static int g_enabled = 1;
static int g_verbose = 1;
static int g_status = IBV_WC_RETRY_EXC_ERR;
static uint32_t g_vendor_err = 0x71;
static unsigned long g_on_nth = 0;
static unsigned long g_every = 1;
static long g_max = 1;
static int g_skip_recovery_cq = 1;
static int g_skip_dummy_wr = 1;
static long g_cq_min_cqe = -1;
static long g_cq_max_cqe = -1;
static int g_opcode = -1;
static int g_has_qp_num = 0;
static uint32_t g_qp_num = 0;
static int g_has_wr_id_min = 0;
static int g_has_wr_id_max = 0;
static uint64_t g_wr_id_min = 0;
static uint64_t g_wr_id_max = UINT64_MAX;
static char g_dev_name[64] = "";
static atomic_ulong g_seen = 0;
static atomic_long g_injected = 0;

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

static int parse_u64_env(const char* name, uint64_t* out) {
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0') return 0;

  char* end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0') return 0;
  *out = (uint64_t)parsed;
  return 1;
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

static int parse_opcode_env(void) {
  const char* value = getenv("NCCL_CQFI_OPCODE");
  if (value == NULL || value[0] == '\0' || strcasecmp(value, "any") == 0) return -1;
  if (strcasecmp(value, "send") == 0) return IBV_WC_SEND;
  if (strcasecmp(value, "rdma_write") == 0 || strcasecmp(value, "write") == 0) return IBV_WC_RDMA_WRITE;
  if (strcasecmp(value, "rdma_read") == 0 || strcasecmp(value, "read") == 0) return IBV_WC_RDMA_READ;
  if (strcasecmp(value, "recv") == 0) return IBV_WC_RECV;
  if (strcasecmp(value, "recv_rdma_with_imm") == 0 || strcasecmp(value, "recv_imm") == 0) {
    return IBV_WC_RECV_RDMA_WITH_IMM;
  }

  char* end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 0);
  if (errno == 0 && end != value && *end == '\0') return (int)parsed;
  return -1;
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
  g_every = (unsigned long)parse_long_env("NCCL_CQFI_EVERY", 1);
  if (g_every == 0) g_every = 1;
  g_max = parse_long_env("NCCL_CQFI_MAX", 1);
  g_skip_recovery_cq = parse_bool_env("NCCL_CQFI_SKIP_RECOVERY_CQ", 1);
  g_skip_dummy_wr = parse_bool_env("NCCL_CQFI_SKIP_DUMMY_WR", 1);
  g_cq_min_cqe = parse_long_env("NCCL_CQFI_CQ_MIN_CQE", -1);
  g_cq_max_cqe = parse_long_env("NCCL_CQFI_CQ_MAX_CQE", -1);
  g_opcode = parse_opcode_env();
  long qp_num = parse_long_env("NCCL_CQFI_QP_NUM", -1);
  if (qp_num >= 0) {
    g_has_qp_num = 1;
    g_qp_num = (uint32_t)qp_num;
  }
  g_has_wr_id_min = parse_u64_env("NCCL_CQFI_WR_ID_MIN", &g_wr_id_min);
  g_has_wr_id_max = parse_u64_env("NCCL_CQFI_WR_ID_MAX", &g_wr_id_max);
  const char* dev_name = getenv("NCCL_CQFI_DEV_NAME");
  if (dev_name != NULL && dev_name[0] != '\0') {
    snprintf(g_dev_name, sizeof(g_dev_name), "%s", dev_name);
  }

  if (g_verbose) {
    fprintf(stderr,
            "[inject] config enabled=%d status=%d vendor_err=0x%x on_nth=%lu every=%lu max=%ld "
            "skip_recovery_cq=%d skip_dummy_wr=%d cq_min_cqe=%ld cq_max_cqe=%ld opcode=%d "
            "qp_num=%s%u wr_id_min=%s%llu wr_id_max=%s%llu dev_name=%s%s\n",
            g_enabled, g_status, g_vendor_err, g_on_nth, g_every, g_max, g_skip_recovery_cq, g_skip_dummy_wr,
            g_cq_min_cqe, g_cq_max_cqe, g_opcode, g_has_qp_num ? "" : "off:", g_qp_num,
            g_has_wr_id_min ? "" : "off:", (unsigned long long)g_wr_id_min,
            g_has_wr_id_max ? "" : "off:", (unsigned long long)g_wr_id_max,
            g_dev_name[0] ? "" : "off:", g_dev_name);
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

static void record_cq(struct ibv_cq* cq, int cqe) {
  if (cq == NULL) return;
  struct cq_entry* entry = (struct cq_entry*)calloc(1, sizeof(*entry));
  if (entry == NULL) {
    cqfi_log("failed to allocate cq entry for cq=%p", (void*)cq);
    return;
  }

  entry->cq = cq;
  entry->cqe = cqe;
  pthread_mutex_lock(&g_lock);
  entry->next = g_cq_entries;
  g_cq_entries = entry;
  pthread_mutex_unlock(&g_lock);
  cqfi_log("created cq=%p cqe=%d", (void*)cq, cqe);
}

static int forget_cq(struct ibv_cq* cq) {
  int cqe = -1;
  pthread_mutex_lock(&g_lock);
  struct cq_entry** prev = &g_cq_entries;
  struct cq_entry* entry = g_cq_entries;
  while (entry != NULL) {
    if (entry->cq == cq) {
      *prev = entry->next;
      cqe = entry->cqe;
      free(entry);
      break;
    }
    prev = &entry->next;
    entry = entry->next;
  }
  pthread_mutex_unlock(&g_lock);
  return cqe;
}

static int lookup_cqe(struct ibv_cq* cq) {
  int cqe = -1;
  pthread_mutex_lock(&g_lock);
  for (struct cq_entry* entry = g_cq_entries; entry != NULL; entry = entry->next) {
    if (entry->cq == cq) {
      cqe = entry->cqe;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return cqe;
}

static void record_qp(struct ibv_qp* qp, struct ibv_pd* pd, struct ibv_qp_init_attr* attr) {
  if (qp == NULL || attr == NULL) return;
  struct qp_entry* entry = (struct qp_entry*)calloc(1, sizeof(*entry));
  if (entry == NULL) {
    cqfi_log("failed to allocate qp entry for qp=%p qp_num=%u", (void*)qp, qp->qp_num);
    return;
  }

  entry->qp = qp;
  entry->qp_num = qp->qp_num;
  entry->send_cq = attr->send_cq;
  entry->recv_cq = attr->recv_cq;
  entry->qp_type = attr->qp_type;
  entry->max_send_wr = attr->cap.max_send_wr;
  entry->max_recv_wr = attr->cap.max_recv_wr;
  const char* dev_name = NULL;
  if (pd != NULL && pd->context != NULL && pd->context->device != NULL) {
    dev_name = pd->context->device->name;
  }
  if (dev_name != NULL) snprintf(entry->dev_name, sizeof(entry->dev_name), "%s", dev_name);

  pthread_mutex_lock(&g_lock);
  entry->next = g_qp_entries;
  g_qp_entries = entry;
  pthread_mutex_unlock(&g_lock);

  cqfi_log("created qp=%p qp_num=%u dev=%s type=%d send_cq=%p recv_cq=%p max_send_wr=%u max_recv_wr=%u",
           (void*)qp, entry->qp_num, entry->dev_name, entry->qp_type, (void*)entry->send_cq,
           (void*)entry->recv_cq, entry->max_send_wr, entry->max_recv_wr);
}

static int forget_qp(struct ibv_qp* qp, struct qp_entry* out) {
  int found = 0;
  pthread_mutex_lock(&g_lock);
  struct qp_entry** prev = &g_qp_entries;
  struct qp_entry* entry = g_qp_entries;
  while (entry != NULL) {
    if (entry->qp == qp) {
      *prev = entry->next;
      if (out != NULL) *out = *entry;
      free(entry);
      found = 1;
      break;
    }
    prev = &entry->next;
    entry = entry->next;
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

static int lookup_qp(uint32_t qp_num, struct qp_entry* out) {
  int found = 0;
  pthread_mutex_lock(&g_lock);
  for (struct qp_entry* entry = g_qp_entries; entry != NULL; entry = entry->next) {
    if (entry->qp_num == qp_num) {
      if (out != NULL) *out = *entry;
      found = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

static int completion_matches_filters(struct ibv_cq* cq, const struct ibv_wc* wc) {
  if (wc->status != IBV_WC_SUCCESS) return 0;

  int cqe = lookup_cqe(cq);
  struct qp_entry qp_info;
  int has_qp_info = lookup_qp(wc->qp_num, &qp_info);
  if (g_skip_recovery_cq && cqe == 20) return 0;
  if (g_cq_min_cqe >= 0 && cqe >= 0 && cqe < g_cq_min_cqe) return 0;
  if (g_cq_max_cqe >= 0 && cqe >= 0 && cqe > g_cq_max_cqe) return 0;
  if (g_skip_dummy_wr && wc->wr_id == UINT64_MAX) return 0;
  if (g_opcode >= 0 && wc->opcode != (enum ibv_wc_opcode)g_opcode) return 0;
  if (g_has_qp_num && wc->qp_num != g_qp_num) return 0;
  if (g_has_wr_id_min && wc->wr_id < g_wr_id_min) return 0;
  if (g_has_wr_id_max && wc->wr_id > g_wr_id_max) return 0;
  if (g_dev_name[0] != '\0' && (!has_qp_info || strcmp(qp_info.dev_name, g_dev_name) != 0)) return 0;
  return 1;
}

static int should_inject_one(void) {
  if (g_on_nth == 0) return 0;
  unsigned long seen = atomic_fetch_add_explicit(&g_seen, 1, memory_order_relaxed) + 1;
  if (seen < g_on_nth) return 0;
  if (((seen - g_on_nth) % g_every) != 0) return 0;

  if (g_max >= 0) {
    long injected = atomic_load_explicit(&g_injected, memory_order_relaxed);
    while (injected < g_max) {
      if (atomic_compare_exchange_weak_explicit(&g_injected, &injected, injected + 1,
                                                memory_order_relaxed, memory_order_relaxed)) {
        return 1;
      }
    }
    return 0;
  }

  atomic_fetch_add_explicit(&g_injected, 1, memory_order_relaxed);
  return 1;
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
    if (!completion_matches_filters(cq, &wc[i])) continue;
    if (!should_inject_one()) continue;

    enum ibv_wc_status old_status = wc[i].status;
    struct qp_entry qp_info;
    int has_qp_info = lookup_qp(wc[i].qp_num, &qp_info);
    wc[i].status = (enum ibv_wc_status)g_status;
    wc[i].vendor_err = g_vendor_err;
    cqfi_log("injected wc[%d] ctx=%p cq=%p cqe=%d wr_id=%llu opcode=%d qp_num=%u dev=%s "
             "old_status=%d new_status=%d vendor_err=0x%x seen=%lu injected=%ld",
             i, (void*)ctx, (void*)cq, lookup_cqe(cq), (unsigned long long)wc[i].wr_id,
             wc[i].opcode, wc[i].qp_num, has_qp_info ? qp_info.dev_name : "unknown",
             old_status, wc[i].status, wc[i].vendor_err,
             atomic_load_explicit(&g_seen, memory_order_relaxed),
             atomic_load_explicit(&g_injected, memory_order_relaxed));
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

static ibv_create_cq_fn get_real_ibv_create_cq(void) {
  pthread_mutex_lock(&g_lock);
  ibv_create_cq_fn fn = g_real_ibv_create_cq;
  pthread_mutex_unlock(&g_lock);
  if (fn != NULL) return fn;

  void* real = dlsym(RTLD_NEXT, "ibv_create_cq");
  if (real == NULL || real == (void*)ibv_create_cq) return NULL;

  pthread_mutex_lock(&g_lock);
  if (g_real_ibv_create_cq == NULL) g_real_ibv_create_cq = (ibv_create_cq_fn)real;
  fn = g_real_ibv_create_cq;
  pthread_mutex_unlock(&g_lock);
  return fn;
}

static ibv_destroy_cq_fn get_real_ibv_destroy_cq(void) {
  pthread_mutex_lock(&g_lock);
  ibv_destroy_cq_fn fn = g_real_ibv_destroy_cq;
  pthread_mutex_unlock(&g_lock);
  if (fn != NULL) return fn;

  void* real = dlsym(RTLD_NEXT, "ibv_destroy_cq");
  if (real == NULL || real == (void*)ibv_destroy_cq) return NULL;

  pthread_mutex_lock(&g_lock);
  if (g_real_ibv_destroy_cq == NULL) g_real_ibv_destroy_cq = (ibv_destroy_cq_fn)real;
  fn = g_real_ibv_destroy_cq;
  pthread_mutex_unlock(&g_lock);
  return fn;
}

static ibv_create_qp_fn get_real_ibv_create_qp(void) {
  pthread_mutex_lock(&g_lock);
  ibv_create_qp_fn fn = g_real_ibv_create_qp;
  pthread_mutex_unlock(&g_lock);
  if (fn != NULL) return fn;

  void* real = dlsym(RTLD_NEXT, "ibv_create_qp");
  if (real == NULL || real == (void*)ibv_create_qp) return NULL;

  pthread_mutex_lock(&g_lock);
  if (g_real_ibv_create_qp == NULL) g_real_ibv_create_qp = (ibv_create_qp_fn)real;
  fn = g_real_ibv_create_qp;
  pthread_mutex_unlock(&g_lock);
  return fn;
}

static ibv_destroy_qp_fn get_real_ibv_destroy_qp(void) {
  pthread_mutex_lock(&g_lock);
  ibv_destroy_qp_fn fn = g_real_ibv_destroy_qp;
  pthread_mutex_unlock(&g_lock);
  if (fn != NULL) return fn;

  void* real = dlsym(RTLD_NEXT, "ibv_destroy_qp");
  if (real == NULL || real == (void*)ibv_destroy_qp) return NULL;

  pthread_mutex_lock(&g_lock);
  if (g_real_ibv_destroy_qp == NULL) g_real_ibv_destroy_qp = (ibv_destroy_qp_fn)real;
  fn = g_real_ibv_destroy_qp;
  pthread_mutex_unlock(&g_lock);
  return fn;
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

__attribute__((visibility("default")))
struct ibv_cq* ibv_create_cq(struct ibv_context* context, int cqe, void* cq_context,
                             struct ibv_comp_channel* channel, int comp_vector) {
  pthread_once(&g_config_once, init_config);

  ibv_create_cq_fn real_create = get_real_ibv_create_cq();
  if (real_create == NULL) {
    cqfi_log("failed to resolve real ibv_create_cq");
    return NULL;
  }

  struct ibv_cq* cq = real_create(context, cqe, cq_context, channel, comp_vector);
  if (cq != NULL) record_cq(cq, cqe);
  return cq;
}

__attribute__((visibility("default")))
int ibv_destroy_cq(struct ibv_cq* cq) {
  pthread_once(&g_config_once, init_config);

  ibv_destroy_cq_fn real_destroy = get_real_ibv_destroy_cq();
  if (real_destroy == NULL) {
    cqfi_log("failed to resolve real ibv_destroy_cq");
    return -1;
  }

  int cqe = forget_cq(cq);
  cqfi_log("destroying cq=%p cqe=%d", (void*)cq, cqe);
  return real_destroy(cq);
}

__attribute__((visibility("default")))
struct ibv_qp* ibv_create_qp(struct ibv_pd* pd, struct ibv_qp_init_attr* qp_init_attr) {
  pthread_once(&g_config_once, init_config);

  ibv_create_qp_fn real_create = get_real_ibv_create_qp();
  if (real_create == NULL) {
    cqfi_log("failed to resolve real ibv_create_qp");
    return NULL;
  }

  struct ibv_qp* qp = real_create(pd, qp_init_attr);
  if (qp != NULL) record_qp(qp, pd, qp_init_attr);
  return qp;
}

__attribute__((visibility("default")))
int ibv_destroy_qp(struct ibv_qp* qp) {
  pthread_once(&g_config_once, init_config);

  ibv_destroy_qp_fn real_destroy = get_real_ibv_destroy_qp();
  if (real_destroy == NULL) {
    cqfi_log("failed to resolve real ibv_destroy_qp");
    return -1;
  }

  struct qp_entry entry;
  if (forget_qp(qp, &entry)) {
    cqfi_log("destroying qp=%p qp_num=%u dev=%s", (void*)qp, entry.qp_num, entry.dev_name);
  } else {
    cqfi_log("destroying qp=%p qp_num=unknown", (void*)qp);
  }
  return real_destroy(qp);
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
  if (symbol_addr != 0 && strcmp(symbol, "ibv_create_cq") == 0 && is_libibverbs_symbol(real)) {
    pthread_mutex_lock(&g_lock);
    g_real_ibv_create_cq = (ibv_create_cq_fn)real;
    pthread_mutex_unlock(&g_lock);
    cqfi_log("dlvsym intercepted ibv_create_cq version=%s real=%p wrapper=%p",
             version, real, (void*)ibv_create_cq);
    return (void*)ibv_create_cq;
  }
  if (symbol_addr != 0 && strcmp(symbol, "ibv_destroy_cq") == 0 && is_libibverbs_symbol(real)) {
    pthread_mutex_lock(&g_lock);
    g_real_ibv_destroy_cq = (ibv_destroy_cq_fn)real;
    pthread_mutex_unlock(&g_lock);
    cqfi_log("dlvsym intercepted ibv_destroy_cq version=%s real=%p wrapper=%p",
             version, real, (void*)ibv_destroy_cq);
    return (void*)ibv_destroy_cq;
  }
  if (symbol_addr != 0 && strcmp(symbol, "ibv_create_qp") == 0 && is_libibverbs_symbol(real)) {
    pthread_mutex_lock(&g_lock);
    g_real_ibv_create_qp = (ibv_create_qp_fn)real;
    pthread_mutex_unlock(&g_lock);
    cqfi_log("dlvsym intercepted ibv_create_qp version=%s real=%p wrapper=%p",
             version, real, (void*)ibv_create_qp);
    return (void*)ibv_create_qp;
  }
  if (symbol_addr != 0 && strcmp(symbol, "ibv_destroy_qp") == 0 && is_libibverbs_symbol(real)) {
    pthread_mutex_lock(&g_lock);
    g_real_ibv_destroy_qp = (ibv_destroy_qp_fn)real;
    pthread_mutex_unlock(&g_lock);
    cqfi_log("dlvsym intercepted ibv_destroy_qp version=%s real=%p wrapper=%p",
             version, real, (void*)ibv_destroy_qp);
    return (void*)ibv_destroy_qp;
  }

  return real;
}
#endif

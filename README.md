# NCCL IB CQ fault injector

Standalone LD_PRELOAD library for real NCCL / ibverbs runs. This directory does
not modify NCCL sources or the NCCL build.

The mechanism is:

1. Intercept `ibv_open_device` in direct-link / normal verbs users.
2. On Linux, intercept `dlvsym`; when NCCL asks for `ibv_open_device`, save the
   real function pointer and return this library's wrapper.
3. After the real `ibv_open_device` returns an `ibv_context`, patch
   `ctx->ops.poll_cq`.
4. The patched `poll_cq` calls the original provider function first, then changes
   selected successful `ibv_wc` entries into configured error completions.

## Build

```sh
cd tools/ib_cq_fault_inject
make
```

For NCCL validation, build this directory on each Linux/IB node that will run a
rank, then preload the resulting `build/libnccl_cq_fault_inject.so` into the NCCL
workload.

## Runtime knobs

- `NCCL_CQFI_ENABLE=0|1`: enable injection, default `1`.
- `NCCL_CQFI_VERBOSE=0|1`: print injector logs, default `1`.
- `NCCL_CQFI_STATUS=retry|flush|general|<number>`: injected `wc.status`,
  default `retry`.
- `NCCL_CQFI_VENDOR_ERR=<number>`: injected `wc.vendor_err`, default `0x71`.
- `NCCL_CQFI_AFTER=<n>`: skip the first `n` successful completions.
- `NCCL_CQFI_EVERY=<n>`: inject every `n`th eligible successful completion.
- `NCCL_CQFI_MAX=<n>`: maximum injections. Use `-1` for unlimited. Default `1`.

## Example

```sh
export LD_PRELOAD=/path/to/ib_cq_fault_inject/build/libnccl_cq_fault_inject.so
export NCCL_CQFI_STATUS=retry
export NCCL_CQFI_VENDOR_ERR=0x113
export NCCL_CQFI_MAX=1
export NCCL_CQFI_VERBOSE=1

# Launch the real NCCL workload with the same environment on all ranks.
```

Expected injector logs include:

```text
[inject] dlvsym intercepted ibv_open_device version=IBVERBS_1.1 ...
[inject] ibv_open_device wrapper called ctx=...
[inject] patched ctx=... old_poll_cq=... new_poll_cq=...
[inject] injected wc.status=... vendor_err=...
```

On NCCL 2.30.x this flows into `wrap_ibv_poll_cq()` because NCCL calls
`cq->context->ops.poll_cq(...)`.

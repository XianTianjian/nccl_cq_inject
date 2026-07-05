# NCCL IB CQ 故障注入器

一个独立的 LD_PRELOAD 共享库，用于在真实 NCCL / ibverbs 运行中注入 InfiniBand CQ（Completion Queue）错误。本工具不修改 NCCL 源码，也不影响 NCCL 构建。

## 工作原理

1. 拦截 `ibv_open_device`（直连 / 普通 verbs 用户）。
2. Linux 下拦截 `dlvsym`；当 NCCL 请求 `ibv_open_device` 时，保存真实函数指针并返回本库的包装函数。
3. 真实 `ibv_open_device` 返回 `ibv_context` 后，对 `ctx->ops.poll_cq` 进行 patch。
4. 被 patch 的 `poll_cq` 先调用原始函数获取完成事件，再将选中的成功 `ibv_wc` 条目修改为配置的错误完成。

## 构建

```sh
make
```

对于 NCCL 验证，在每个运行 rank 的 Linux/IB 节点上构建此目录，然后将生成的 `build/libnccl_cq_fault_inject.so` 通过 LD_PRELOAD 注入 NCCL 工作负载。

## 运行时参数

| 环境变量 | 取值 | 默认值 | 说明 |
|---|---|---|---|
| `NCCL_CQFI_ENABLE` | `0`/`1` | `1` | 启用/禁用注入 |
| `NCCL_CQFI_VERBOSE` | `0`/`1` | `1` | 打印注入日志 |
| `NCCL_CQFI_STATUS` | `retry` / `flush` / `general` / `<数字>` | `retry` | 注入的 `wc.status` |
| `NCCL_CQFI_VENDOR_ERR` | `<数字>` | `0x71` | 注入的 `wc.vendor_err` |
| `NCCL_CQFI_AFTER` | `<n>` | `0` | 跳过前 n 个成功完成 |
| `NCCL_CQFI_EVERY` | `<n>` | `1` | 每 n 个成功完成注入一次 |
| `NCCL_CQFI_MAX` | `<n>` / `-1` | `1` | 最大注入次数，`-1` 表示无限制 |

## 使用示例

```sh
export LD_PRELOAD=/path/to/nccl_cq_inject/build/libnccl_cq_fault_inject.so
export NCCL_CQFI_STATUS=retry
export NCCL_CQFI_VENDOR_ERR=0x113
export NCCL_CQFI_MAX=1
export NCCL_CQFI_VERBOSE=1

# 在所有 rank 上使用相同环境变量启动真实 NCCL 工作负载
```

预期注入日志示例：

```text
[inject] dlvsym intercepted ibv_open_device version=IBVERBS_1.1 ...
[inject] ibv_open_device wrapper called ctx=...
[inject] patched ctx=... old_poll_cq=... new_poll_cq=...
[inject] injected wc.status=... vendor_err=...
```

在 NCCL 2.30.x 上，错误会流入 `wrap_ibv_poll_cq()`，因为 NCCL 通过 `cq->context->ops.poll_cq(...)` 调用 poll。

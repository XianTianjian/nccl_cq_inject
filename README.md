# NCCL IB CQ 故障注入器

一个独立的 LD_PRELOAD 共享库，在真实 NCCL / ibverbs 运行中向 InfiniBand Completion Queue 注入错误，用于验证 NCCL 的 RDMA 错误处理路径（failover、重传、flush 等）。本工具不修改 NCCL 源码，不影响 NCCL 构建。

## 工作原理

1. 拦截 `ibv_open_device` — 无论 NCCL 直接链接 libibverbs 还是通过 `dlvsym` 动态获取符号，都能截获。
2. Linux 下额外拦截 `dlvsym`：当 NCCL 请求 `ibv_open_device` 时，保存真实函数指针并返回本库的包装函数。
3. 在真实 `ibv_open_device` 返回的 `ibv_context` 上 patch `ctx->ops.poll_cq` 函数指针。
4. 被替换的 `poll_cq` 先调用原始函数拿到真实的完成事件（`ibv_wc`），再将其中的**成功完成**（`status == IBV_WC_SUCCESS`）按配置规则改写为错误完成。

在 NCCL 2.30.x 上，错误最终流入 `wrap_ibv_poll_cq()`，触发 NCCL 的 IB transport 错误处理（重传 / QP 重建 / failover）。

## 构建

```sh
make
```

产物为 `build/libnccl_cq_fault_inject.so`。在**每个**运行 NCCL rank 的 Linux / IB 节点上都需要构建（或分发同一份 `.so`）。

## 运行时参数

### `NCCL_CQFI_ENABLE`

- **类型**：`0` / `1`
- **默认**：`1`

总开关。设为 `0` 后即使 LD_PRELOAD 加载了 `.so`，也不会对 `poll_cq` 做任何注入，相当于完全旁路。用于对比测试：同一套环境变量只需改这一个值即可跑 baseline vs 注入。

### `NCCL_CQFI_VERBOSE`

- **类型**：`0` / `1`
- **默认**：`1`

控制是否向 stderr 打印注入日志。日志格式：`[inject] <事件>`。建议测试时始终开启，方便确认注入是否在期望的位置触发。

### `NCCL_CQFI_ON`

- **类型**：正整数 / `0`
- **默认**：`0`

**核心参数**。指定在第几个**成功的 CQ 完成**上注入错误。计数器在库加载后从 1 开始递增，跨所有 IB 设备上下文共享（即同一进程内所有 `ibv_poll_cq` 调用共用一个计数器）。

- `0`：不注入（禁用）。
- `1`：在第一个成功完成上注入，通常对应连接建立阶段。
- `N`（N > 1）：前 N-1 个成功完成正常放行，第 N 个被改写为错误。

注意：计数器统计的是成功完成（`IBV_WC_SUCCESS`）次数，已经失败 / 有 error 的完成不会计数。因此如果你需要"跳过连接建立阶段的 CQ、在业务数据传输阶段注入"，需要先跑一次 baseline 看连接阶段产生多少次成功完成，再设定合适的 `ON` 值。

### `NCCL_CQFI_STATUS`

- **类型**：字符串 / 数字
- **默认**：`retry`

注入的错误类型，即改写后的 `ibv_wc.status` 值：

| 值 | 对应 `ibv_wc_status` | 典型 NCCL 行为 |
|---|---|---|
| `retry`（或 `retry_exc`、`retry_exc_err`） | `IBV_WC_RETRY_EXC_ERR` (12) | 触发 NCCL transport 重传 / QP 重建 |
| `flush`（或 `wr_flush`、`wr_flush_err`） | `IBV_WC_WR_FLUSH_ERR` (5) | 触发 QP flush 处理 |
| `general`（或 `general_err`） | `IBV_WC_GENERAL_ERR` (14) | 通用硬件错误 |
| 数字（如 `4`） | 直接使用该数字作为 `ibv_wc_status` | 自定义错误码 |

### `NCCL_CQFI_VENDOR_ERR`

- **类型**：整数
- **默认**：`0x71`

注入的 `ibv_wc.vendor_err` 字段值。这个字段是 RDMA 硬件/驱动返回的厂商特定错误码（syndrome），不同设备对同一值有不同语义。常用值：

- `0x71`：典型的 retry count exceeded
- `0x113`：某些 Mellanox 设备的 transport retry exceeded
- 参考 `drivers/infiniband/hw/mlx5/*.h` 或设备文档查找特定错误码

## 使用示例

### 基础用法

```sh
# 在所有 rank 节点上设置相同的环境变量
export LD_PRELOAD=/path/to/nccl_cq_inject/build/libnccl_cq_fault_inject.so
export NCCL_CQFI_STATUS=retry
export NCCL_CQFI_VENDOR_ERR=0x113
export NCCL_CQFI_ON=100     # 前 99 个成功完成正常，第 100 个注入错误
export NCCL_CQFI_VERBOSE=1

# 通过 mpirun 将环境变量传递给所有 rank
mpirun -np 2 -host node1:1,node2:1 \
  -x LD_PRELOAD -x NCCL_CQFI_ON -x NCCL_CQFI_STATUS \
  -x NCCL_CQFI_VENDOR_ERR -x NCCL_CQFI_VERBOSE \
  all_reduce_perf -b 8M -e 8M -g 1
```

### 跑 baseline（无注入）

```sh
# 所有参数不变，仅关掉注入
export NCCL_CQFI_ON=0
```

### 常见测试场景

| 场景 | `NCCL_CQFI_ON` | 目的 |
|---|---|---|
| 连接阶段注入 | `1` | 验证 NCCL 能否在初始化阶段处理 IB 错误 |
| 稳态传输注入 | `500` ~ `1000` | 跳过连接/握手，在数据传输中途注入 |
| 首包注入 | 根据 baseline 确定连接 CQ 数 + `1` | 在第一个数据传输完成时注入 |

## 预期日志

```text
[inject] config enabled=1 status=12 vendor_err=0x71 on_nth=5
[inject] dlvsym intercepted ibv_open_device version=IBVERBS_1.1 real=0x... wrapper=0x...
[inject] ibv_open_device wrapper called device=0x... ctx=0x...
[inject] patched ctx=0x... old_poll_cq=0x... new_poll_cq=0x...
...（NCCL 初始化，多个 IB 设备上下文被 patch）...
[inject] injected wc[0] ctx=0x... cq=0x... wr_id=0 old_status=0 new_status=12 vendor_err=0x71
```

- `config` 行：库加载时打印一次，确认所有参数。
- `patched` 行：每个 IB 上下文被 hook 时打印。
- `injected` 行：每次注入时打印，包含被注入的 CQ、WR、新旧 status。

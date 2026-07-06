# NCCL IB CQ 故障注入器

一个独立的 LD_PRELOAD 共享库，在真实 NCCL / ibverbs 运行中向 InfiniBand Completion Queue 注入错误，用于验证 NCCL 的 RDMA 错误处理路径（failover、重传、flush 等）。本工具不修改 NCCL 源码，不影响 NCCL 构建。

## 工作原理

本库通过拦截以下 ibverbs 符号实现对 CQ 完成事件的注入：

1. **`ibv_open_device`** — 无论 NCCL 直接链接 libibverbs 还是通过 `dlvsym` 动态获取符号，都能截获。返回的 `ibv_context` 上 patch `ctx->ops.poll_cq` 函数指针。
2. **`dlvsym`**（Linux）— 当 NCCL 通过 `dlvsym(RTLD_NEXT, "ibv_open_device", "IBVERBS_1.1")` 请求符号时，保存真实函数指针并返回本库的包装函数。同时拦截 `ibv_create_cq` / `ibv_destroy_cq` / `ibv_create_qp` / `ibv_destroy_qp` 的 `dlvsym` 调用。
3. **`ibv_create_cq`** / **`ibv_destroy_cq`** — 记录每个 CQ 的 `cqe` 大小（用于区分 NCCL data/probing/recovery CQ），在销毁时清理记录。
4. **`ibv_create_qp`** / **`ibv_destroy_qp`** — 记录每个 QP 的 `qp_num`、设备名（从 `pd->context->device->name` 获取）、QP 类型等元数据，用于按设备名过滤注入目标，销毁时清理记录。
5. **`poll_cq`（patch）** — 被替换的 `poll_cq` 先调用原始函数拿到真实完成事件（`ibv_wc`），再将其中符合过滤条件的**成功完成**（`status == IBV_WC_SUCCESS`）按配置规则改写为错误完成。

在 NCCL 2.30.x 上，错误最终流入 `wrap_ibv_poll_cq()`，触发 NCCL 的 IB transport 错误处理（重传 / QP 重建 / failover）。

## 构建

```sh
make
```

产物为 `build/libnccl_cq_fault_inject.so`。在**每个**运行 NCCL rank 的 Linux / IB 节点上都需要构建（或分发同一份 `.so`）。

## 运行时参数

### 总开关

#### `NCCL_CQFI_ENABLE`

- **类型**：`0` / `1`
- **默认**：`1`

总开关。设为 `0` 后即使 LD_PRELOAD 加载了 `.so`，也不会对 `poll_cq` 做任何注入，相当于完全旁路。用于对比测试：同一套环境变量只需改这一个值即可跑 baseline vs 注入。

#### `NCCL_CQFI_VERBOSE`

- **类型**：`0` / `1`
- **默认**：`1`

控制是否向 stderr 打印注入日志。所有日志均以 `[inject]` 为前缀。建议测试时始终开启，方便确认注入是否在期望的位置触发。

---

### 注入策略

#### `NCCL_CQFI_ON`

- **类型**：正整数 / `0`
- **默认**：`0`

**核心参数**。指定从第几个**通过过滤条件的成功 CQ 完成**开始注入。计数器在库加载后从 1 开始递增，跨所有 IB 设备上下文共享（即同一进程内所有 `ibv_poll_cq` 调用共用一个全局计数器）。

- `0`：不注入（禁用）。
- `1`：在第一个通过过滤的成功完成上注入，通常对应连接建立阶段。
- `N`（N > 1）：前 N-1 个通过过滤的成功完成正常放行，第 N 个被改写为错误。

注意：只有通过全部过滤参数（见下文）的成功完成才会被计数。建议用 `NCCL_CQFI_CQ_MIN_CQE=21` 跳过 NCCL recovery/probing CQ 后，再用 `ON=1` 表示第一个 data CQE。

#### `NCCL_CQFI_EVERY`

- **类型**：正整数
- **默认**：`1`

从 `NCCL_CQFI_ON` 命中的完成开始，每隔 `EVERY` 个**通过过滤条件的完成**注入一次。`EVERY=1` 表示每次命中都注入（连续注入），`EVERY=2` 表示隔一次注入一次，以此类推。设 `0` 会被自动纠正为 `1`。

注入时机公式：第 `ON`、`ON+EVERY`、`ON+2*EVERY`、... 个通过过滤条件的完成将被注入。

#### `NCCL_CQFI_MAX`

- **类型**：整数（`-1` 表示不限制）
- **默认**：`1`

最多注入 `MAX` 次。达到上限后，后续所有通过过滤条件的完成都不再注入。

- `1`：只注入一次（单次故障）。
- `N`：最多注入 N 次。
- `-1`：不限制次数，持续注入。

---

### 过滤参数

过滤按以下顺序逐一检查，全部通过才算"命中"，才会推进注入计数器：

| 顺序 | 参数 | 默认 | 含义 |
|---:|---|---:|---|
| 1 | —（硬编码） | — | 只过滤 `status == IBV_WC_SUCCESS` 的完成。 |
| 2 | `NCCL_CQFI_SKIP_RECOVERY_CQ` | `1` | 跳过 `cqe == 20` 的 CQ。NCCL 2.30.x recovery CQ 固定 `cqe=20`。 |
| 3 | `NCCL_CQFI_CQ_MIN_CQE` | unset | 只注入 `cqe >= min` 的 CQ。常用 `21` 表示只看 data CQ。 |
| 4 | `NCCL_CQFI_CQ_MAX_CQE` | unset | 只注入 `cqe <= max` 的 CQ。 |
| 5 | `NCCL_CQFI_SKIP_DUMMY_WR` | `1` | 跳过 `wr_id == UINT64_MAX` 的 receiver dummy WR。 |
| 6 | `NCCL_CQFI_OPCODE` | `any` | 只注入指定 opcode：`rdma_write`（或 `write`）、`rdma_read`（或 `read`）、`recv_rdma_with_imm`（或 `recv_imm`）、`send`、`recv`，或直接传数字。 |
| 7 | `NCCL_CQFI_QP_NUM` | unset | 只注入指定 `wc.qp_num`。精确到单个 QP。 |
| 8 | `NCCL_CQFI_WR_ID_MIN` | unset | `wr_id` 下界（`>=`）。与 `WR_ID_MAX` 配合可精确到单个 WR。 |
| 9 | `NCCL_CQFI_WR_ID_MAX` | unset | `wr_id` 上界（`<=`）。 |
| 10 | `NCCL_CQFI_DEV_NAME` | unset | 只注入指定 verbs 设备名，如 `mlx5_0` 或 `mlx5_1`。依赖 `ibv_create_qp` 的 hook 记录 QP→设备映射，本工具已自动拦截，无需额外配置。 |

> **关于 `DEV_NAME`**：过滤时通过 `wc.qp_num` 反查创建 QP 时的设备名。如果对应 QP 不是通过被拦截的 `ibv_create_qp` 创建的（极少数边缘情况），则 `DEV_NAME` 过滤不会命中该 QP 上的完成。

---

### 注入内容

#### `NCCL_CQFI_STATUS`

- **类型**：字符串 / 数字
- **默认**：`retry`

注入的错误类型，即改写后的 `ibv_wc.status` 值：

| 值 | 对应 `ibv_wc_status` | 典型 NCCL 行为 |
|---|---|---|
| `retry`（或 `retry_exc`、`retry_exc_err`） | `IBV_WC_RETRY_EXC_ERR` (12) | 触发 NCCL transport 重传 / QP 重建 |
| `flush`（或 `wr_flush`、`wr_flush_err`） | `IBV_WC_WR_FLUSH_ERR` (5) | 触发 QP flush 处理 |
| `general`（或 `general_err`） | `IBV_WC_GENERAL_ERR` (14) | 通用硬件错误 |
| 数字（如 `4`） | 直接使用该数字作为 `ibv_wc_status` | 自定义错误码 |

#### `NCCL_CQFI_VENDOR_ERR`

- **类型**：整数
- **默认**：`0x71`

注入的 `ibv_wc.vendor_err` 字段值。这个字段是 RDMA 硬件/驱动返回的厂商特定错误码（syndrome），不同设备对同一值有不同语义。常用值：

- `0x71`：典型的 retry count exceeded
- `0x113`：某些 Mellanox 设备的 transport retry exceeded
- 参考 `drivers/infiniband/hw/mlx5/*.h` 或设备文档查找特定错误码

---

### 计数器说明

库内部维护两个全局原子计数器：

- **`g_seen`**：已见过的通过过滤条件的成功完成总数。驱动注入时机判定。
- **`g_injected`**：已实际执行的注入次数。受 `MAX` 上限约束。

两者均在库加载后从 0 开始，跨所有 IB 设备上下文共享。`MAX` 到达上限后，`g_seen` 继续递增但不再产生注入。

---

## 使用示例

### 基础用法

```sh
# 在所有 rank 节点上设置相同的环境变量
export LD_PRELOAD=/path/to/nccl_cq_inject/build/libnccl_cq_fault_inject.so
export NCCL_CQFI_STATUS=retry
export NCCL_CQFI_VENDOR_ERR=0x113
export NCCL_CQFI_CQ_MIN_CQE=21
export NCCL_CQFI_ON=1         # 第一个 data CQE 注入错误
export NCCL_CQFI_MAX=1        # 只注入一次
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

或者在运行时完全旁路：

```sh
export NCCL_CQFI_ENABLE=0
```

### 精细过滤示例

```sh
# 只在 mlx5_1 设备上的 RDMA_WRITE 操作的指定 QP 上注入，每隔 10 次注入一次，最多 3 次
export NCCL_CQFI_ON=1
export NCCL_CQFI_EVERY=10
export NCCL_CQFI_MAX=3
export NCCL_CQFI_DEV_NAME=mlx5_1
export NCCL_CQFI_OPCODE=rdma_write
export NCCL_CQFI_QP_NUM=1234

# 精确到单个 WR：在某 QP 上 wr_id 为 42 的完成上注入
export NCCL_CQFI_ON=1
export NCCL_CQFI_QP_NUM=5678
export NCCL_CQFI_WR_ID_MIN=42
export NCCL_CQFI_WR_ID_MAX=42
```

### 常见测试场景

| 场景 | `NCCL_CQFI_ON` | 辅助过滤 | 目的 |
|---|---|---|---|
| 连接阶段注入 | `1` | — | 验证 NCCL 能否在初始化阶段处理 IB 错误 |
| 首包注入 | `1` | `CQ_MIN_CQE=21` | 跳过 probing/recovery CQ，在第一个数据传输完成时注入 |
| 稳态传输注入 | `500` ~ `1000` | `CQ_MIN_CQE=21` | 跳过连接/握手，在数据传输中途注入 |
| 指定设备故障 | `1` | `DEV_NAME=mlx5_1` | 模拟单网卡故障 |
| 周期性故障 | `1` | `EVERY=100, MAX=-1` | 周期性注入，模拟间歇性链路抖动 |

## 预期日志

```text
[inject] config enabled=1 status=12 vendor_err=0x71 on_nth=5 every=1 max=1 skip_recovery_cq=1 skip_dummy_wr=1 cq_min_cqe=21 cq_max_cqe=-1 opcode=-1 qp_num=off:0 wr_id_min=off:0 wr_id_max=off:18446744073709551615 dev_name=off:
[inject] dlvsym intercepted ibv_open_device version=IBVERBS_1.1 real=0x... wrapper=0x...
[inject] ibv_open_device wrapper called device=0x... ctx=0x...
[inject] patched ctx=0x... old_poll_cq=0x... new_poll_cq=0x...
[inject] created cq=0x... cqe=128
[inject] created qp=0x... qp_num=1234 dev=mlx5_1 type=2 send_cq=0x... recv_cq=0x... max_send_wr=128 max_recv_wr=128
...（NCCL 初始化，多个 IB 设备上下文被 patch，CQ/QP 被记录）...
[inject] injected wc[0] ctx=0x... cq=0x... cqe=128 wr_id=0 opcode=0 qp_num=1234 dev=mlx5_1 old_status=0 new_status=12 vendor_err=0x71 seen=5 injected=1
```

- `config` 行：库加载时打印一次，完整列出所有参数及其生效值。`off:` 前缀表示该过滤参数未设置（不生效）。
- `patched` 行：每个 IB 上下文被 hook 时打印。
- `created cq/qp` 行：每个 CQ / QP 被创建时记录，用于后续过滤匹配。
- `destroying cq/qp` 行：CQ / QP 销毁时清理记录。
- `injected` 行：每次注入时打印，包含被注入的 CQ、WR、opcode、QP、设备名、新旧 status 及当前计数器值。

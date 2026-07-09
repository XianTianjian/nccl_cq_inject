# Project Context

NCCL IB/RoCE CQ 故障注入实验。验证 NCCL 在 IB 设备 CQE 错误注入下的 failover 与 recovery 行为。

## Agent 行为约束

修改代码前，Agent 必须：
1. 先读本文件。
2. 先复述当前目标与已知约束。
3. 先检查相关源码再提出修改方案。
4. 优先最小化 patch。
5. 禁止编造路径、主机名、设备名、环境变量或 NCCL 参数。
6. 涉及 NCCL 内部逻辑的结论，必须引用具体源文件和函数。
7. 涉及运行时行为的结论，必须引用具体日志行。
8. 不得假设仅靠 `ibv_poll_cq` 拦截就足够。
9. 不得假设所有 mlx5 设备都会被自动使用。
10. 不得假设跨 NUMA 融合有效，除非日志或源码证明。
11. 不得在 `-H` 中使用 IP 地址，除非明确在测试 MPI 主机名行为。
12. 不得使用 MPICH 专用参数——本环境使用 OpenMPI。
13. 不得 kill 非本实验的进程——清理残留时只 kill `nccl_ar_detail` 和属于当前用户的 `mpirun`/`orted`，不动 `/tmp/ompi.*` 等 MPI session 文件。如遇 "All nodes already filled" 错误，说明他人正在使用 MPI，等待而非强制清理。
14. 网卡（mlx5 IB/RoCE）是本实验的核心资源，GPU 空闲不代表网卡空闲。运行前必须检查 IB 设备状态和是否有他人 MPI 进程在占用。

## Current Status

CQE 故障注入路径已验证。观测结果：
- 注入的 CQE error 可触发 NCCL IB port failover ✓
- recovery / failback 可成功 ✓
- 恢复后的 device 可重新加入 active QP 池 ✓
- 每次迭代的延时 spike 可通过 cudaEvent 逐次计时精确观测 ✓

当前下一步目标：
- 量化对比 baseline / failover-only / recovery
- 测试更多 collective：all_gather、reduce_scatter、broadcast、reduce
- 生成图表与最终实验报告

## Goal

- baseline（无注入，验证通信正常）
- fault without recovery（注入错误，只观察 failover）
- failover（注入后流量从故障设备切换到健康设备）
- recovery / failback（故障设备恢复后重新加入 active QP 池）

## Environment

- GPUs: NVIDIA A100-PCIE-40GB, 2 GPUs per node
- CUDA: 13.3
- NCCL: 2.30.7（自编译，路径 `/home/xiajinyi25/nccl_inject/nccl/build`）
- OpenMPI: 4.1.9a1（路径 `/usr/mpi/gcc/openmpi-4.1.9a1`）
- Machines:
  - xfusion-2: 192.168.5.112（rank 0）
  - xfusion-3: 192.168.5.113（rank 1）
- Management: bond0 (192.168.5.0/24)
- RoCE: 4× mlx5_* 100Gbps
  - mlx5_0, mlx5_1 → NUMA0, PIX to GPU0
  - mlx5_2, mlx5_3 → NUMA1, PIX to GPU1
- 工作文件夹: `/home/xiajinyi25/nccl_inject/`，尽量不要修改其他文件夹内容
## Key Paths

| 内容 | 路径 |
|------|------|
| 本仓库（注入器 + 分析脚本） | 本地 `/Users/xiantianjian/nccl/tools/nccl_cq_inject` |
| 远程工作目录 | `/home/xiajinyi25/nccl_inject` |
| NCCL 库 | `$WORKDIR/nccl/build/lib/libnccl.so` |
| CQE 注入器 .so | `$WORKDIR/ib_cq_fault_inject/build/libnccl_cq_fault_inject.so` |
| 打 log 插件 | `$WORKDIR/nccl_ar_detail` |
| nccl-tests（官方） | `$WORKDIR/nccl-tests/build/` |
| NCCL 源码 | `$WORKDIR/nccl/` |

## 关键工具

### nccl_cq_inject（本仓库）

LD_PRELOAD 共享库。劫持链路：

1. **`ibv_open_device` wrapper** → patch `ctx->ops.poll_cq` 函数指针
2. **`dlvsym` interceptor** → 拦截 NCCL 的 `dlvsym(RTLD_NEXT, "ibv_xxx", "IBVERBS_1.1")` 调用，返回注入器 wrapper 而非真实函数
3. **`ibv_create_cq` / `ibv_destroy_cq`** → 记录每个 CQ 的 cqe 大小，用于区分 data/probing/recovery CQ
4. **`ibv_create_qp` / `ibv_destroy_qp`** → 记录 QP→设备名映射，支持 `DEV_NAME` 过滤
5. **`poll_cq` patch** → 真实 poll 拿到 wc 后，对符合过滤条件的 SUCCESS 改为 ERROR

NCCL 2.30.x 中各 CQ 的 cqe 大小：

| 类型 | cqe | 用途 |
|------|-----|------|
| data send | 1024 | 数据发送 |
| data recv | 2048 | 数据接收 |
| probing | 256 | failover 后探测 |
| recovery | 20 | recovery alive/ack |
| flush | 1 | QP 销毁 |

### nccl_ar_detail

位于 [XianTianjian/nccl-tests](https://github.com/XianTianjian/nccl-tests)，文件 `src/nccl_ar_detail.cu`。

逐次 CUDA event 计时 benchmark，输出 JSON `detail` 数组。支持 `-o` 切换 all_reduce/all_gather/reduce_scatter/broadcast/reduce。

与官方 `all_reduce_perf` 的核心区别：`cudaEventRecord` 在循环**内**，记录每次迭代的精确耗时，而非只算均值。

## NCCL Resiliency 参数（2.30.x）

```bash
# Failover（必须）
NCCL_IB_RESILIENCY_PORT_FAILOVER=1

# Recovery（必须）
NCCL_IB_RESILIENCY_PORT_RECOVERY=1

# Recovery 调优
NCCL_IB_RESILIENCY_PORT_RECOVERY_START_DELAY=10     # ms，故障后延迟
NCCL_IB_RESILIENCY_PORT_RECOVERY_ALIVE_MSG_TIMEOUT=1000  # ms
NCCL_IB_RESILIENCY_PORT_RECOVERY_ACK_TIMEOUT=1000        # ms
NCCL_IB_RESILIENCY_PORT_RECOVERY_ATTEMPTS_MAX=20         # 最多重试次数
```

注意：参数前缀是 `NCCL_IB_RESILIENCY_`，**不是** `NCCL_PORT_`。

## NCCL 设备融合

- `NCCL_IB_HCA=mlx5_0,mlx5_1` 限制 NCCL 只用指定设备
- `NCCL_NET_FORCE_MERGE=mlx5_0,mlx5_1` 强制融合指定 NIC 为虚拟设备
- 同 NUMA/PIX 设备默认自动融合（`ndevs=2`），跨 NUMA 拒绝融合
- `NCCL_NETDEVS_POLICY=ALL` 让所有设备对每个 GPU 可见

## MPI 注意事项

- 用主机名而非 IP：`-H xfusion-2:1,xfusion-3:1`（OpenMPI 4.1.9a1 用 IP 有时卡住）
- MPMD 模式：`: ` 分隔不同 rank 的命令
- `--oversubscribe` 允许单节点多进程
- `--mca btl_tcp_if_include bond0` 指定 TCP 通信走管理网口
- 用 `/usr/bin/env` 传环境变量，避免 mpirun `-x` 展开问题
- 113 的 `-J /dev/null` 表示不写 JSON（只有 rank 0 写）
- MPMD 模式下两边 `CQ_MIN_CQE` 不同（data send=1024, data recv=2048），不要随意改成相同值

## Standard Run Workflow

每次运行前，先检查环境是否空闲：

```bash
# 检查 GPU 占用
ssh 192.168.5.112 'nvidia-smi --query-compute-apps=pid,process_name --format=csv,noheader'
ssh 192.168.5.113 'nvidia-smi --query-compute-apps=pid,process_name --format=csv,noheader'

# 检查登录用户
ssh 192.168.5.112 who
ssh 192.168.5.113 who

# 检查 MPI session（他人任务不要动）
ssh 192.168.5.112 'ls /tmp/ompi.* 2>/dev/null'
ssh 192.168.5.113 'ls /tmp/ompi.* 2>/dev/null'

# 检查 IB 网卡占用（核心资源）
ssh 192.168.5.112 'rdma link show 2>/dev/null || ibv_devinfo 2>/dev/null | grep -E "hca_id|state"'
ssh 192.168.5.113 'rdma link show 2>/dev/null || ibv_devinfo 2>/dev/null | grep -E "hca_id|state"'
```

确认 GPU 空闲、无他人 MPI session、IB 设备 PORT_ACTIVE 后，运行实验。

### 单 size 独立实验（推荐）

脚本 `run_size_200.sh`，每个 size 独立跑 200 次迭代，各自拥有完整的 failover + recovery 时间线：

```bash
scp run_size_200.sh 192.168.5.112:/home/xiajinyi25/nccl_inject/
ssh 192.168.5.112 'bash /home/xiajinyi25/nccl_inject/run_size_200.sh'
```

产物：`/tmp/size_{8M,16M,32M,64M,128M,256M}_200.json`

### 多 size 连续实验（最简指令，已验证）

```bash
ssh 192.168.5.112

WORKDIR=/home/xiajinyi25/nccl_inject
BIN=$WORKDIR/nccl_ar_detail

ENVS="LD_PRELOAD=$WORKDIR/ib_cq_fault_inject/build/libnccl_cq_fault_inject.so \
  NCCL_CQFI_ENABLE=1 NCCL_CQFI_ON=1 NCCL_CQFI_DEV_NAME=mlx5_0 \
  NCCL_IB_HCA=mlx5_0,mlx5_1 \
  NCCL_IB_RESILIENCY_PORT_FAILOVER=1 NCCL_IB_RESILIENCY_PORT_RECOVERY=1"

mpirun --oversubscribe --mca btl_tcp_if_include bond0 \
  -np 1 -H xfusion-2:1 \
    /usr/bin/env $ENVS NCCL_CQFI_CQ_MIN_CQE=1024 NCCL_CQFI_CQ_MAX_CQE=1024 \
    $BIN -b 256M -e 256M -n 200 -w 10 -v -J /tmp/out.json \
  : -np 1 -H xfusion-3:1 \
    /usr/bin/env $ENVS NCCL_CQFI_CQ_MIN_CQE=2048 NCCL_CQFI_CQ_MAX_CQE=2048 \
    $BIN -b 256M -e 256M -n 200 -w 10 -v -J /dev/null
```

仅 6 个必须参数：`LD_PRELOAD`、`CQFI_ENABLE`、`CQFI_ON`、`CQFI_DEV_NAME`、`IB_HCA`、`IB_RESILIENCY_PORT_FAILOVER/RECOVERY`。如需看 recovery 日志加 `NCCL_DEBUG=TRACE NCCL_DEBUG_SUBSYS=NET`。

## Run Modes

### Baseline
```bash
NCCL_CQFI_ENABLE=0
NCCL_CQFI_ON=0
NCCL_IB_RESILIENCY_PORT_FAILOVER=0
NCCL_IB_RESILIENCY_PORT_RECOVERY=0
```

### Failover-only
```bash
NCCL_CQFI_ENABLE=1
NCCL_CQFI_ON=1
NCCL_IB_RESILIENCY_PORT_FAILOVER=1
NCCL_IB_RESILIENCY_PORT_RECOVERY=0
```

### Recovery / Failback
```bash
NCCL_CQFI_ENABLE=1
NCCL_CQFI_ON=1
NCCL_IB_RESILIENCY_PORT_FAILOVER=1
NCCL_IB_RESILIENCY_PORT_RECOVERY=1
```

## Known Issues

1. **注入器早期符号解析失败** — `dlsym(RTLD_NEXT, "ibv_xxx")` 在 NCCL `dlopen` libibverbs 之前调用会返回注入器自身。修复：`ibv_dlsym_fallback()` 加 `dlopen("libibverbs.so.1")` 显式查。

2. **113 JSON 不生成** — `nccl_ar_detail` 只在 rank 0 写 JSON（by design）。如果要 rank 1 也写，需改代码（当前已恢复为 rank-0-only）。

3. **僵尸进程** — timeout 杀 mpirun 时远程 nccl_ar_detail 残留，阻塞后续 MPI 初始化。每次跑前须 `pkill -9`。

4. **stdio 缓冲丢数据** — `fprintf` 写 JSON 时 stdio 全缓冲，进程被 timeout 杀时 `{` 都未刷盘。修复：`setbuf(json_fp, NULL)`。

5. **`-v` flag bug** — 早期版本 `case 'v':` 缺失导致 usage 退出。已修复。

## Verified Test Result

### 单 size 独立 failback（200 次/档，WARN 日志）

| Size | iters | oop_avg | min | max | spikes | BW | data_ok |
|------|-------|---------|-----|-----|--------|-----|---------|
| 8MB | 200 | 3,559us | 817 | 24,712 | 37 | 2.36 | ✅ |
| 16MB | 200 | 5,872us | 1,229 | 32,021 | 17 | 2.86 | ✅ |
| 32MB | 200 | 9,241us | 2,380 | 40,174 | 4 | 3.63 | ✅ |
| 64MB | 200 | 11,119us | 4,784 | 63,904 | 11 | 6.04 | ✅ |
| 128MB | 200 | 14,589us | 9,170 | 84,065 | 9 | 9.20 | ✅ |
| 256MB | 200 | 19,819us | 18,035 | 77,608 | 2 | 13.54 | ✅ |

小 size 尾部 spike 是 FLUSH_ERR CQE 排空行为，非恢复失败。恢复在 TRACE 日志中已确认（`Port recovery succeeded` + `Restoring QP` + `All resiliency operations completed`）。

### 多 size 连续 failback（200 次/档，TRACE 日志）

```
entries=1200  op=all_reduce  data_ok=True  sizes=6
oop avg=7122us  spikes=3
  iter 8   (8MB):  23739us (3.3x)  ← 第一轮 failover
  iter 268 (16MB): 26629us (3.7x)  ← 第二轮 failover
  iter 290 (16MB): 36017us (5.1x)  ← recovery

Port recovery succeeded for devIndex=0 (send comm)
Port recovery succeeded for devIndex=0 (recv comm)
Marking device 0 as recovered
Restoring QP on device 0
All resiliency operations are completed
```

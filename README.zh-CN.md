# DAZG-Orbit：可复现的双进程 Q16/HE 推理

[English](README.md)

DAZG-Orbit 是一个面向可复现实验的源码仓库，用于验证基于 CIFAR-100 的双进程定点推理与同态加密推理流程。

仓库包含维护后的 C++ 双进程运行时、冻结模型权重、确定性评估输入、逐 logits 精确参考结果、精简的 Python Q16 oracle、固定版本依赖，以及采用 fail-closed 原则设计的实验执行脚本。

## 当前公开版本范围

当前公开版本首先提供已经完成完整审计的 **CIFAR-100** 实验通道。

本仓库采用 **audit-first staged release（先审计、后发布）** 的渐进式发布策略。只有当一个实验通道的源码、模型身份、预处理流程、定点算术、参考输出、双进程执行、精度指标和构建说明全部通过统一的复现门禁后，该实验才会进入公开仓库。

CIFAR-100 被作为首个公开基准，是因为它在数据规模适中的同时具有 100 个类别，能够较完整地验证：

- 密文打包与布局；
- 密文线性计算；
- 激活函数的定点语义；
- 双进程算术一致性；
- 模型精度保持；
- 旋转与通信调度；
- clean-build 环境下的可复现性。

后续数据集、模型结构、更大样本规模和硬件配置，会在分别完成相同的清洁构建、数值一致性和文档审计后，以独立、版本化的实验通道持续加入本仓库。

## 已验证结果

| 实验通道 | 样本数 | 算术门禁 | 精度门禁 |
|---|---:|---:|---:|
| 冻结 P60 诊断通道 | 10 | strict exact 10/10 | Top-1 10/10；Top-5 10/10 |
| checkpoint 013 balanced 评估 | 100 | logits exact 100/100；mismatch 0；max delta 0 | Top-1 72/100；Top-5 91/100 |

当前实验不需要重新训练。公开 checkpoint 为仅包含模型权重的文件，共包含 77 个 tensor。

本版本暂不包含 N=1000。N=1000 会在固定输入集、Q16 oracle、执行身份、断点恢复和聚合指标全部完成独立审计后，作为单独实验通道发布。

> **安全边界。** 当前两个验证通道均使用仓库中的 `reveal` correctness backend，并声明 `security_claim=0`。它们验证的是算术一致性、双进程执行编排、密文线性操作和模型精度保持，不等同于完整的 no-reveal 端到端隐私推理部署。

## 仓库结构

```text
Datatype/ HE/ Layer/ Model/ OT/ Operator/ Utils/  DAZG-Orbit C++ 运行时
Extern/                                           固定版本的第三方源码
experiments/n10_p60/                              冻结 N=10 实验通道
experiments/n100_checkpoint013/                   checkpoint 013 N=100 通道
checkpoints/                                      仅权重 checkpoint
python/                                           Q16 模型与 oracle 导出程序
scripts/                                          构建、验证和发布脚本
results/                                          精简且经过复核的结果摘要
docs/                                             技术与复现文档
```

源码仓库不包含原始终端日志、PDF、旧发布包、历史失败包、构建产物、缓存和无关实验文件。

## 支持环境

参考环境：

- Ubuntu 22.04 或 Ubuntu 24.04；
- WSL2 Ubuntu；
- 支持 AVX2 的 x86-64 CPU；
- GCC/G++；
- CMake 与 Ninja；
- OpenSSL 开发包；
- GMP 开发包；
- Python 3；
- NumPy。

仓库中固定包含以下第三方依赖源码快照：

- Microsoft SEAL 4.1.2；
- Intel HEXL 1.2.6；
- emp-tool；
- emp-ot。

安装 Ubuntu 系统依赖：

```bash
bash ./reproduce.sh setup
```

该命令会调用 `apt`。在共享服务器或受管理环境中运行前，建议先查看 `scripts/setup_ubuntu.sh`。

## 从全新 clone 开始复现

```bash
git clone https://github.com/Lbrief/dazg-orbit-secure-inference-repro.git
cd dazg-orbit-secure-inference-repro

bash ./reproduce.sh verify
DAZG_JOBS=8 bash ./reproduce.sh build
bash ./reproduce.sh stage-s
bash ./reproduce.sh n10
bash ./reproduce.sh n100
```

系统依赖安装完成后，也可以执行完整流程：

```bash
DAZG_JOBS=8 bash ./reproduce.sh all
```

构建产物和运行证据会写入：

```text
build/
runs/
```

这两个目录均已被 `.gitignore` 排除，不会进入 GitHub 源码提交。

推荐统一使用：

```bash
bash ./reproduce.sh ...
```

而不是直接执行 `./reproduce.sh ...`。这样可以避免 Windows 挂载路径或某些压缩工具未保留 executable bit 时出现 `Permission denied`。

## 命令说明

| 命令 | 功能 |
|---|---|
| `bash ./reproduce.sh setup` | 安装 Ubuntu 系统依赖 |
| `bash ./reproduce.sh verify-manifest` | 校验发布压缩包中的全部文件 |
| `bash ./reproduce.sh verify` | 校验资产、指标、路径、品牌命名和脚本语法 |
| `DAZG_JOBS=8 bash ./reproduce.sh build` | 构建 HEXL、SEAL 和 N=10/N=100 执行器 |
| `bash ./reproduce.sh stage-s` | 运行 checkpoint 013 Stage-S 合约测试 |
| `bash ./reproduce.sh n10` | 复现冻结 P60 N=10 实验 |
| `bash ./reproduce.sh n100` | 复现 checkpoint 013 balanced N=100 实验 |
| `bash ./reproduce.sh oracle-n100` | 重新生成 N=100 Q16 oracle，需要 CPU 版 PyTorch |
| `bash ./reproduce.sh clean` | 删除本地 build 和 runs 目录 |
| `bash ./reproduce.sh release --output PATH` | 生成经过检查的源码发布包 |

## Fail-closed 验收门禁

### N=10

N=10 只有同时满足以下条件才会判定为通过：

- 10 个样本全部完成；
- 每个样本 server/client 返回码均为 0；
- strict arithmetic exactness 为 10/10；
- Top-1 为 10/10；
- Top-5 为 10/10。

### N=100

N=100 只有同时满足以下条件才会判定为通过：

- completed samples：100/100；
- server/client 返回码为 0：100/100；
- two-process logits exact：100/100；
- mismatched logit elements：0；
- maximum absolute logit delta：0；
- frozen reference Top-5 rows exact：100/100；
- Top-1：72/100；
- Top-5：91/100。

sample 0 和 balanced 前 10 个样本属于硬门禁。只有它们全部通过后，程序才会继续执行完整 N=100。

仅 Top-5 排名一致不足以通过。N=100 要求完整的 100 维 logits 与冻结 Q16 oracle 精确一致。

## 密文计算加速

DAZG-Orbit 在维护源码中直接说明了主要加速机制，包括：

- block-circulant cyclic-NTT 编码；
- 稀疏 baby-step/giant-step 调度；
- 重复密文旋转结果复用；
- 跳过不活跃 packed diagonal；
- K3/S2 phase packing 与 correctness-first fallback；
- 有界并行 share/ciphertext 转换；
- 对旋转、`mul_plain`、`add_inplace`、通信量和协议轮次的运行时计数。

这些优化的目标包括：

- 减少重复 Galois/key-switch 操作；
- 减少无效密文旋转；
- 复用已经生成的旋转密文；
- 避免不必要的 packed diagonal；
- 在优化路径不能证明 exact 时自动回退到保守正确路径。

详细说明见：

[docs/CIPHERTEXT_ACCELERATION.md](docs/CIPHERTEXT_ACCELERATION.md)

## Checkpoint 与评估资产

公开 checkpoint：

```text
checkpoints/dazg_orbit_checkpoint013_weights.pt
```

属性：

- tensor 数量：77；
- SHA-256：  
  `f7db0ff0bae94d806275c23ff46ded8fbdebc8a72cbf193004e915d293c28c3e`
- tensor 内容指纹：  
  `b9d36a2e92431eac464a0f5cb8d529e28fa095ab96038bf5f040ae639bb1362f`
- optimizer state：未包含；
- scheduler state：未包含；
- 是否需要重新训练：否。

仓库只包含复现 N=10 和 N=100 所需的固定 CIFAR-100 评估 tensor，不包含完整 CIFAR-100 数据集。

这样可以保证：

- 样本集合固定；
- 标签固定；
- 预处理固定；
- 输入身份可校验；
- Q16 oracle 可复核；
- 不需要下载无关数据和历史训练文件。

后续版本会继续加入：

- 更多 CIFAR-100 模型配置；
- 更多模型结构；
- 其他公开数据集；
- 更大的确定性样本集；
- 更完整的密文操作计数；
- 不同硬件与线程扩展实验；
- 独立验证的 no-reveal 实验通道；
- 版本化 N=1000 评估。

## 性能数据说明

运行时间会受到以下因素影响：

- CPU 型号；
- WSL 配置；
- 线程数；
- 编译器版本；
- 系统负载；
- 仓库所在文件系统；
- Windows Defender 或索引程序。

确定性的验收依据是：

- 固定输入身份；
- 权重与 oracle 身份；
- logits exact；
- mismatch 数量；
- max absolute delta；
- Top-1/Top-5 目标指标。

报告性能数据时，应同时说明硬件、线程数、编译器、backend policy 和执行身份。

## 安全与声明边界

当前公开通道使用：

```text
adapter_policy = reveal
security_claim = 0
```

当前版本可以支持以下结论：

- 定点算术一致性；
- 双进程执行编排正确；
- 密文线性计算可复现；
- packing 与 rotation scheduling 可复核；
- logits 与 Q16 oracle 一致；
- 模型精度未因执行器引入额外损失。

当前版本不能用于声明：

- 完整 no-reveal 端到端部署；
- 面向生产环境的恶意安全；
- 不含诊断暴露的完整隐私推理；
- 已完成公开 N=1000 通道验证。

后续安全执行配置会在其协议、泄露边界、精度和性能证据完成独立验证后单独发布。

## 后续更新计划

未来版本计划加入：

- 更多 CIFAR-100 模型配置；
- 更多网络架构；
- 其他公开数据集；
- 更大的确定性样本规模；
- 更完整的密文计算计数器；
- 更多硬件和线程扩展测试；
- 独立 no-reveal 协议实验；
- 版本化 N=1000 支持；
- 更精简的机器可读发布证据。

每个新实验通道都会提供独立的：

- 输入 manifest；
- 模型/checkpoint 身份；
- 参考 oracle；
- clean-build 指令；
- exactness gate；
- accuracy gate；
- 性能元数据；
- 安全边界声明。

## 文档导航

- [安装说明](docs/INSTALL.md)
- [系统架构](docs/ARCHITECTURE.md)
- [密文计算加速](docs/CIPHERTEXT_ACCELERATION.md)
- [复现协议](docs/REPRODUCIBILITY.md)
- [验证标准](docs/VALIDATION.md)
- [性能数据](docs/PERFORMANCE.md)
- [数据与权重](docs/DATA_AND_WEIGHTS.md)
- [安全边界](docs/SECURITY_BOUNDARY.md)
- [故障排查](docs/TROUBLESHOOTING.md)
- [GitHub 发布说明](docs/GITHUB_RELEASE.md)

## 署名与许可证

DAZG-Orbit 维护以下部分：

- 系统集成；
- 密文计算加速逻辑；
- 正确性门禁；
- 实验通道隔离；
- 复现与发布工具链。

上游来源记录保留在：

- [NOTICE_UPSTREAM.md](NOTICE_UPSTREAM.md)
- `THIRD_PARTY_LOCK.json`

第三方代码的版权和许可证声明不会被重写。

公开再分发前，应确认：

1. DAZG-Orbit 维护代码采用的正式许可证；
2. checkpoint 的公开再分发权限；
3. 固定 CIFAR-100 衍生 tensor 的公开再分发权限。

详细说明见：

[LICENSE_NOTICE.md](LICENSE_NOTICE.md)

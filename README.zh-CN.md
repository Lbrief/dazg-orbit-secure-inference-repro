# DAZG-Orbit：可复现的双进程 Q16/HE 推理

本仓库提供两个已经验证的 CIFAR-100 复现实验：冻结的 P60 N=10
诊断链，以及 checkpoint 013 的 balanced N=100 链。N=10 必须达到
strict exact 10/10、Top-1/Top-5 10/10；N=100 必须达到 logits exact
100/100、Top-1 72/100、Top-5 91/100。

## 快速复现

```bash
./reproduce.sh verify-manifest   # 仅发布压缩包需要
./reproduce.sh setup
./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
```

不需要重新训练。完整指令、依赖、目录说明和 GitHub 发布方法请参阅英文
`README.md` 与 `docs/`。

> 安全边界：当前验证使用 `reveal` correctness backend，
> `security_claim=0`。它证明算术一致性与精度保持，不等于 no-reveal
> 端到端隐私部署。


### WSL 路径说明

运行器支持将仓库放在 WSL 的 Windows 挂载路径。JSON 证据写入包含
重试、校验复制和临时报告恢复逻辑；不会因为 DrvFS 或索引程序短暂阻塞原子重命名，
把已经由 server/client 正常完成的样本误判为计算失败。

- [v4 WSL 运行证据写入修复](docs/V4_RELEASE_VALIDATION.md)

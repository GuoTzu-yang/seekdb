# AMD hipVS 赛题提交说明

- 基线：`zihaomu/seekdb` 分支 `amd-oceanbase-official-contest-base-v5`
- 冻结 commit：`634888d9b037153fa143d2c913ba4b25503e1d62`（本提交为其直接后代）
- 评测方式：云端统一编译与测试（ROCm 7.2.4 + AMD Radeon PRO W7900D），本地不参与计分

## 本次提交内容

1. 沿用 V5 冻结基线的 hipVS/cuVS CAGRA 构建与查询链路
   （`tools/hipvs_bridge/seekdb_cuvs_bridge.c`、`tools/hipvs_bridge/build.sh`）。
2. 验证 `lib=vsag` CPU 路径保持可用，`lib=cuvs` 走 GPU CAGRA 构建 + GPU 快照重载。
3. 编译方式与赛题要求一致：仓库根目录执行 `tools/hipvs_bridge/build.sh`，
   产物 `build_release/src/observer/seekdb` 与 bridge 动态库。

## 说明

本提交先确保「正确性与硬门槛」（20 分中的 4 分）通过：基线在 SIFT250K 与 Hidden250K
两个正式数据集上均满足全部 Recall 门槛；性能优化项将在此基础上迭代。

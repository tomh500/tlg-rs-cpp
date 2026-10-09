# 可复现的基准

本基准比较 TLG5 和 TLG6 在常数、渐变和固定种子随机像素上的编码时间、解码时间与输出大小。以下为 2026-10-10 完成解码分配检查修复后的本机复测结果。

## 工作负载和测量方法

- Windows x64，AMD Ryzen 7 9700X，8 核 / 16 线程；编解码串行运行。
- C++：MSVC 19.44.35228，Release `/O2 /std:c++latest`，x64 默认 SSE2 路径。
- Rust：`rustc 1.99.0`，`x86_64-pc-windows-msvc`，`--release --no-default-features`，
  原分支 `xuanrikka` 提交 `1f0d294e95b184bb9480d41e44d602d0e6a3dd7f`，库源码未修改。
- 每图 512×512 RGBA，原始像素恰好 1 MiB。`flat` 的 RGB 为 127、alpha 为 255；
  `gradient` 为 `[x mod 256, y mod 256, (x+y) mod 256, 255]`；
  `random` 是 xorshift32 高字节，种子 `0xc0ffee12`，包含随机 alpha。
- 每项先进行一次未计时的编码、解码和逐字节正确性检查，再分别计时 30 次。
  五次独立进程测量交替 C++/Rust 顺序；表中耗时取五个均值的中位数。
- 使用公开 API，包括返回缓冲分配与销毁，不包括像素生成、磁盘 I/O 和 CSV 输出。
  C++ 编码借用像素；Rust 原 API 接受拥有所有权的 `Vec`，每次编码为复用输入
  而克隆原始数据。比较衡量调用这些公开 API 的实际成本。

表中的两项数值均按 **C++ / Rust** 排列；倍率为 Rust 耗时 ÷ C++ 耗时，
大于 1 表示 C++ 更快。

| 格式 | 数据 | 输出字节 C++ / Rust | 编码 ms C++ / Rust | 编码倍率 | 解码 ms C++ / Rust | 解码倍率 |
|---|---|---:|---:|---:|---:|---:|
| TLG5 | flat | 15,900 / 16,922 | 1.713 / 11.735 | 6.85× | 0.487 / 1.896 | 3.89× |
| TLG5 | gradient | 16,409 / 17,196 | 2.556 / 12.729 | 4.98× | 0.738 / 2.050 | 2.78× |
| TLG5 | random | 1,051,672 / 1,051,672 | 4.623 / 5.465 | 1.18× | 0.418 / 1.157 | 2.77× |
| TLG6 | flat | 2,132 / 2,157 | 13.003 / 31.127 | 2.39× | 0.506 / 1.217 | 2.40× |
| TLG6 | gradient | 68,260 / 68,286 | 29.178 / 39.380 | 1.35× | 1.671 / 1.934 | 1.16× |
| TLG6 | random | 1,116,816 / 1,117,124 | 64.692 / 69.019 | 1.07× | 3.844 / 4.671 | 1.22× |

接近 1 的倍率容易受 CPU 调频、系统负载和编译器版本影响，应按接近的表现
解读。这里使用可复现的合成图片；真实图片的颜色相关性、透明度与纹理不同，
可能改变耗时及压缩率。TLG6 的双遍编码控制临时驻留内存，也有额外计算成本。

提前扫描块与 Slide token 会增加解码工作量；本次 TLG5 gradient 解码中位数为
0.738 ms，上一版测量为 0.608 ms。两次测量并非受控的单项差分实验，
其中也包含系统波动；这里保留当前带完整验证的实现结果。

完整逐样本数据在 [`2026-10-10-cpp.csv`](../bench/results/2026-10-10-cpp.csv)、
[`2026-10-10-rust.csv`](../bench/results/2026-10-10-rust.csv)，汇总在
[`2026-10-10-summary.csv`](../bench/results/2026-10-10-summary.csv)。
吞吐量以原始像素 MiB ÷ 耗时计算，CSV 中另有对应列。
CSV 保留完整浮点精度，展示表格才四舍五入；比较脚本拒绝零值和非有限耗时。

## 重跑

C++ 基准随 `msvc` preset 构建，可单独调整尺寸和次数：

```powershell
cmake --preset msvc
cmake --build --preset release
./build/msvc/Release/tlg_benchmark.exe 512 512 30
```

Rust oracle 的提取和构建见 [测试说明](../tests/README.md)。它不依赖可选 `image`
功能；驱动文件为 [`rust_oracle.rs`](../tests/interop/rust_oracle.rs)。完成构建后：

```powershell
./bench/compare.ps1 `
  -RustOracle build/oracle/rust-source/target/release/examples/interop.exe `
  -Width 512 -Height 512 -Rounds 30 -Samples 5
```

脚本输出各组中位数及倍率，并将独立进程数据保存在 `build/benchmark/{cpp,rust}.csv`。
对新的尺寸或数据分布，应重跑对应的正确性检查与测量再作比较。

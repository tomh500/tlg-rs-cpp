# 可复现的基准：2026-10-12

C++ 1.1.0 与未经修改的 Rust v1.0.0 提交 `10373b87fd0d24fa38311b1b59758c7727624194` 比较。
旧报告保留在 [2026-10-10 记录](benchmark-2026-10-10.md)，不用旧 Slide 解码器的成绩冒充最新版。

## 测量口径

- Windows x64，AMD Ryzen 7 9700X，8 核/16 线程；TLG 编解码串行。
- C++：MSVC 19.44.35228，Release `/O2 /std:c++latest`，默认 SSE2。
- Rust：rustc 1.99.0，MSVC target，`--release --no-default-features`，库源码不改。
- 默认 C++ 组合为 `greedy / adaptive / factored / bulk`。
- 每项先编码、解码并逐字节核对像素，再计时 30 轮；5 个独立进程样本，交替 C++/Rust 顺序，取进程均值的中位数。
- `-RustCodecOnly` 排除 Rust 的输入所有权克隆。Rust 编码返回缓冲的销毁也在计时外；C++ 编码包含返回缓冲创建和销毁。解码双方包含公开调用的读取、分配和结果销毁。磁盘 I/O、像素生成和 CSV 输出不计时。

合成输入为 512×512 RGBA，恰好 1 MiB。flat 为 RGB=127/A=255；gradient 为
`[x mod 256,y mod 256,(x+y) mod 256,255]`；random 用 xorshift32 高字节、种子 `0xc0ffee12`，包括随机 alpha。

照片是上游 `benches/test.jpg`，534×510。在计时外由 WIC 一次读取成 817,020 字节 RGB，双方使用相同像素，避免把 JPEG 解码差异混入 TLG 比较。
JPEG SHA256：`56fcf035f37e9e0d7bfd2b3fb5b818b3fc8de3d23b8ad2bd9969d41a0b5f22f8`。
RGB SHA256：`7ed31b797a56f4f7b9043d01ef1a01e7827c2c4a2f984d7c269f9add146f8413`。

## 默认优化路线与最新 Rust

双值按 **C++ / Rust**；倍率是 Rust 耗时 ÷ C++ 耗时，大于 1 表示 C++ 更快。

| 格式 | 输入 | 字节 C++ / Rust | 编码 ms C++ / Rust | 编码倍率 | 解码 ms C++ / Rust | 解码倍率 |
|---|---|---:|---:|---:|---:|---:|
| TLG5 | flat | 15,900 / 16,922 | 1.743 / 11.710 | 6.72× | 0.477 / 1.071 | 2.24× |
| TLG5 | gradient | 16,409 / 17,196 | 2.942 / 12.495 | 4.25× | 0.479 / 1.041 | 2.17× |
| TLG5 | random | 1,051,672 / 1,051,672 | 5.367 / 5.807 | 1.08× | 0.447 / 1.529 | 3.42× |
| TLG6 | flat | 2,132 / 2,157 | 7.445 / 27.497 | 3.69× | 0.586 / 1.177 | 2.01× |
| TLG6 | gradient | 68,260 / 68,286 | 12.128 / 36.575 | 3.02× | 1.675 / 1.885 | 1.12× |
| TLG6 | random | 1,116,816 / 1,117,124 | 29.681 / 79.955 | 2.69× | 3.832 / 5.215 | 1.36× |
| TLG5 | upstream photo | 346,574 / 347,223 | 29.245 / 36.601 | 1.25× | 1.637 / 2.070 | 1.26× |
| TLG6 | upstream photo | 224,353 / 224,373 | 31.964 / 61.057 | 1.91× | 4.724 / 5.261 | 1.11× |

八项中位数均更快，输出更小或相同。接近 1 的倍率应按接近表现理解：CPU 调频、后台负载、编译器和输入分布都能改变结果。TLG5 random 在前一轮曾略慢，不能凭本表声称所有可能图像、机器上都同时更快、更小。

原始进程数据与完整精度汇总：
[合成 C++](../bench/results/2026-10-12-synthetic-cpp.csv)、[合成 Rust](../bench/results/2026-10-12-synthetic-rust.csv)、[合成汇总](../bench/results/2026-10-12-synthetic-summary.csv)，
[照片 C++](../bench/results/2026-10-12-photo-cpp.csv)、[照片 Rust](../bench/results/2026-10-12-photo-rust.csv)、[照片汇总](../bench/results/2026-10-12-photo-summary.csv)。

## 可选版本的实际取舍

`reference` 组合为 `greedy / primary / reference / scalar`，是可运行的 C++ 对照路径。
它不是逐字节复制 Rust 的全部编码决策，也不保证比最新 Rust 快；例如其 TLG5 flat/gradient 标量解码比新版 Rust 慢。
另一组 3 样本×10 轮的独立测量见 [C++](../bench/results/2026-10-12-reference-cpp.csv)、[Rust](../bench/results/2026-10-12-reference-rust.csv)、[汇总](../bench/results/2026-10-12-reference-summary.csv)。轮数不同，不用来宣称受控的精确增益。

`optimal` 用最短路径保证同一起始字典的单个 Slide 块字节数最小。这次照片上的 TLG5 再减少 10,652 字节（约 3.07%），计算代价明显增加。
以下 optimal 数据单独运行、没有并发，3 样本×3 轮；greedy 列来自主表 5×30，因此展示取舍而非严格单变量差分。解码轮数较少，判断微小差异应重跑较多轮。

| 格式 | greedy / optimal 字节 | greedy / optimal 编码 ms | greedy / optimal 解码 ms |
|---|---:|---:|---:|
| TLG5 photo | 346,574 / 335,922 | 29.245 / 811.393 | 1.637 / 2.090 |
| TLG6 photo | 224,353 / 224,274 | 31.964 / 39.392 | 4.724 / 5.815 |

原始数据：[C++](../bench/results/2026-10-12-optimal-photo-cpp.csv)、[Rust](../bench/results/2026-10-12-optimal-photo-rust.csv)、[汇总](../bench/results/2026-10-12-optimal-photo-summary.csv)。
最优解析临时状态为 `20N+128 KiB`（64 位），默认预算 256 MiB，不含固定匹配索引及编码输出。TLG5 的 raw/compressed 分叉会改变后续字典，块级最优不等于整个文件全局最优。默认保留吞吐更好的 greedy。

## 重跑

按 [更新说明](upstream-v1.md) 提取和构建固定 Rust oracle，然后：

```powershell
cmake --preset msvc
cmake --build --preset release --parallel
./bench/compare.ps1 -RustOracle build/oracle/latest/target/release/examples/interop.exe -Width 512 -Height 512 -Rounds 30 -Samples 5 -RustCodecOnly
```

复现照片：

```powershell
./build/msvc/Release/tlg_tool.exe encode build/oracle/latest/benches/test.jpg build/oracle/latest/photo.tlg
./build/msvc/Release/tlg_tool.exe decode build/oracle/latest/photo.tlg build/oracle/latest/photo.raw
./bench/compare.ps1 -RustOracle build/oracle/latest/target/release/examples/interop.exe -Width 534 -Height 510 -Channels 3 -RawFile build/oracle/latest/photo.raw -Rounds 30 -Samples 5 -RustCodecOnly
```

脚本支持 `-SlideParse greedy|optimal`、`-SlideMatch adaptive|primary`、`-FilterSearch factored|reference`、`-SlideDecode bulk|scalar`，保留完整浮点数据，拒绝缺失样本、变化的输出大小及非有限/非正耗时。不同实验请指定独立 `-OutputDirectory`；默认目录为 `build/benchmark`。

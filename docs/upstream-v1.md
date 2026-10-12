# Rust v1.0.0 同步与 C++ 1.1.0

上游来源是 [XuanRikka/tlg-rs](https://github.com/XuanRikka/tlg-rs)，固定提交
`10373b87fd0d24fa38311b1b59758c7727624194`（v1.0.0）。本地 `xuanrikka` 分支已从
`1f0d294e95b184bb9480d41e44d602d0e6a3dd7f` 快进到该提交；Rust 库源码保持原样。
本次同步对应 C++ 1.1.0；库与命令行的改动范围见下文。

## 同步范围

- 新 Slide 解码思路移植为有界块复制，保留逐字节版本。
- 裸格式和 SDS 内部格式探测统一提供 `detect_format`，完整解码仍独立验证所有结构。
- TLG5/TLG6 的 little-endian 读取和写入与上游一致；上游这部分 byteorder 重构没有改变线格式。
- 新图片转换 CLI；TLG 转码保留元数据，输出先在同目录临时文件中完成后替换，失败保留原输出。
- 上游新增的真实照片基准纳入测量；同时保留固定种子合成数据，防止只优化某张图。

上游仍存在的一些高层 API 缺陷没有照搬：裸 `TlgReader::read()` 在已消费签名后再次读签名；
SDS 标签按分隔符拆分；Gray 的 `read_to_image` 构造 RGB 图像。C++ 使用独立的像素 oracle，
同时用固定 SDS 样本与长度边界测试检查元数据。可通过 `interop probe_reader FILE` 观察未经修改的上游行为。

## 开发者自己选择

两条 C++ 运行路线可以一键选择：

```powershell
build/msvc/Release/tlg_tool.exe encode --profile reference input.png reference.tlg
build/msvc/Release/tlg_tool.exe decode --profile reference reference.tlg reference.raw
build/msvc/Release/tlg_tool.exe encode --profile optimized input.png optimized.tlg
build/msvc/Release/tlg_tool.exe decode --profile optimized optimized.tlg optimized.raw
```

`reference` 组合为 `greedy / primary / reference / scalar`；默认 `optimized` 组合为
`greedy / adaptive / factored / bulk`。这些是本项目的可运行 C++ 对照与优化路线，
不是两份独立 checkout，也不能称为逐字节复刻 Rust 所有编码决策。
原始 Rust 本身保存在 `xuanrikka`。按命令行顺序，后面的单项选项可以覆盖 profile：
例如 `--profile optimized --slide-parse optimal` 选择更耗时的块级最短路径编码。

```cpp
tlg::EncodeOptions encode_options;
encode_options.slide_parse = tlg::SlideParse::optimal; // 更小的块，明显更多计算
encode_options.tlg6_filter_search = tlg::Tlg6FilterSearch::factored;
encode_options.max_slide_working_bytes = std::size_t{256} << 20;
tlg::DecodeOptions decode_options;
decode_options.slide_decode = tlg::SlideDecode::bulk;
```

| 参数 | 默认 | 可选对照 | 保证与取舍 |
|---|---|---|---|
| `slide_parse` | `greedy` | `optimal` | 同一起始字典的单个块最短；额外 `20N+128 KiB` 解析临时状态，解析更慢 |
| `tlg6_filter_search` | `factored` | `reference` | 完整精确搜索，输出逐字节一致；每个预测器通道计价最多 48→14 次 |
| `slide_decode` | `bulk` | `scalar` | 同像素、同持久字典；前者输出历史与倍增，后者逐字节环形复制 |
| `slide_match` | `adaptive` | `primary` | 同编码字节；照片中的长三字节链按需改查四字节索引，触发后增加 144 KiB |

TLG6 滤镜表只有一个 Slide 块，因此相同滤镜搜索下 `optimal` 的 TLG6 文件大小不大于 `greedy`。
`max_slide_working_bytes` 限制最优解析临时状态，不含固定匹配索引和编码输出。
TLG5 的 raw/compressed 决策会分叉后续字典，不能由局部最优推出整个文件最优。
这两个限制明确保留；对所有可能输入都同时更快、更小的说法没有证明。

命令行对应 `--slide-parse`、`--slide-memory`、`--slide-match`、`--filter-search` 和 `--slide-decode`，例如：

```powershell
build/msvc/Release/tlg_tool.exe transcode --format 6 --slide-parse optimal --filter-search factored input.tlg smaller.tlg
build/msvc/Release/tlg_tool.exe decode --slide-decode scalar smaller.tlg restored.raw
```

## 图片转换

库本身仍只依赖标准库。便携图片解析器和 Windows WIC、可选 libavif 都只属于 CLI。
不支持的格式、资源上限、不可表示的深度、动画或多表面输入给出明确错误。
默认转换要求能保留目标 8 位像素值；输入降位量化以及 JPEG 输出须显式 `--allow-lossy`。
PNG/BMP/TIFF 无损输出回读核验每个通道，包含 alpha=0 下的 RGB。

| 格式 | 后端与范围 |
|---|---|
| PBM/PGM/PPM/PAM/PNM | 便携，P1–P7，ASCII/二进制、精确位深映射 |
| QOI、farbfeld | 便携规范读写，保留透明 RGB；Gray 按格式扩展通道 |
| TGA | 便携，灰度、真彩色、调色板、RLE、图像原点与有效尾部边界；写灰度/真彩色 |
| DDS | 便携，raw Gray/RGB/RGBA、BC1/2/3 及所选 DX10 类型；写无压缩像素；多表面/mip 链明确拒绝 |
| PNG/JPEG/BMP/TIFF | Windows WIC；PNG/BMP/TIFF 无损回读验证，JPEG 输出为显式有损选择 |
| AVIF | 可选 libavif 1.3+；默认无损，支持裁剪、旋转、镜像，限制 65536 像素边长与至多 4 工作线程 |

AVIF 启用方法：

```powershell
vcpkg install libavif[aom]:x64-windows
cmake -S . -B build/avif -G "Visual Studio 17 2022" -A x64 -DTLG_TOOL_AVIF=ON -DCMAKE_TOOLCHAIN_FILE=C:/Users/jingy/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/avif --config Release --parallel
ctest --test-dir build/avif -C Release --output-on-failure
```

需要带编码器和解码器的 libavif 1.3+。默认 AVIF 输出使用完整范围、RGB identity/YUV444 或 Gray/YUV400，
并用质量 100 的 AV1 和 alpha 编码，完成后逐字节检查。格式没有原布局标识时可能扩展通道；
QOI 的 Gray 会变 RGB，farbfeld 的 Gray/RGB 会变 RGBA，颜色与 alpha 保持。

## 复现最新版互操作

```powershell
git archive xuanrikka --output=build/oracle/latest.tar
New-Item -ItemType Directory -Force build/oracle/latest
tar -xf build/oracle/latest.tar -C build/oracle/latest
./tests/interop/run.ps1 -RustTree build/oracle/latest
./tests/interop/algorithms.ps1 -RustOracle build/oracle/latest/target/release/examples/interop.exe
```

原来的不可变样本保留在旧提交上，不能因为更新上游就重生成。在线脚本现在现场调用所测试 Rust 编码器，
避免将旧样本误当作最新版双向证据。792 项跨语言检查覆盖两种格式、三种布局、两种解析、匹配搜索与解码路径、
两种 TLG6 搜索，以及单行、单列、8×8 尾块、随机和周期数据。CI 固定同一个 Rust SHA。

`tests/interop/images.ps1` 用独立 image-rs 读取器检查 21 个 C++ 图片输出；
只在临时 Rust 树启用图片测试依赖，结束后恢复 manifest，库源码不改。
`tests/interop/avif.ps1` 要求启用 AVIF 的工具、FFmpeg 和支持 AVIF 的 Pillow；
FFmpeg 检查颜色平面，Pillow 检查完整 RGBA。当前 FFmpeg 将辅助 alpha 解为独立流，不能把自动补 255 的颜色输出当作 alpha oracle。

实际执行：

```powershell
./tests/interop/images.ps1 -RustTree build/oracle/latest
./tests/interop/avif.ps1 -CppTool build/avif/Release/tlg_tool.exe
```

本机 Release/Debug/ASan、关闭 SIMD 与 WIC 的便携构建、可选 AVIF 构建结果和所有独立验证，
详见 [测试记录](../tests/README.md)。可选 AVIF 还单独在 ASan 下验证了该后端和完整 CLI 转换测试。

数学与内存边界见 [算法说明](algorithms.md)，逐进程性能数据与口径见 [基准说明](benchmark.md)。

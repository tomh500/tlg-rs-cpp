# 测试与互操作

## 2026-10-13 的补充审查

再次只读核对远端 HEAD，仍为下文固定的 `10373b8`；oracle 中 19 个 Rust 库源码文件
逐个匹配该提交的 Git blob。两条算法路线另经独立代码审查，块级最优性和精确等价的说明保持成立。

补齐 PNM 注释的 CR/LF/CRLF 边界、最大值及魔数后直接注释、P1 紧凑位串和以空白起始的合法尾部。
规则依据 [Netpbm PBM 规范](https://netpbm.sourceforge.net/doc/pbm.html) 与
[PGM 规范](https://netpbm.sourceforge.net/doc/pgm.html)；53 项新增语法回归同时检查二进制首像素
为 LF、CR、TAB、空格或 `#` 时不被当作头部吞掉。另完成 12 项 P1/P5/P6 魔数后注释的
完整 CLI 往返（TLG5/TLG6 × Release/ASan），像素逐字节相同。

补充非法 `SlideMatch` 参数检查后，库测试为 33,887 项。
最终 Release、Debug、ASan、关闭 SIMD/WIC 的便携 Release、AVIF Release 和 AVIF ASan
均重新构建受影响目标，并通过 `tlg_tests`、`tlg_image_io_tests`、`tlg_tool_help` 和
`tlg_tool_roundtrip` 四项回归。此前完整 CTest 与跨语言记录保留在下文；未重复宣称远端 CI 已执行。

## 2026-10-12 的最新版验证

C++ 1.1.0 与固定 Rust v1.0.0 提交 `10373b87fd0d24fa38311b1b59758c7727624194` 完成了本机验证。
Release、Debug、ASan RelWithDebInfo、关闭 SIMD/WIC 的便携 Release 都通过全部 7 项 CTest；
启用 libavif 1.3.0/AOM 3.13.1 的 Release 通过全部 8 项。新增 AVIF 路径与图片 CLI 在 ASan 配置也通过。
远端 CI 已更新，尚未将本地改动推送执行；不能把本机结果称为远端结果。

- 普通库测试 33,886 项检查，包括 735 组完整 TLG6 `factored/reference` 字节差分与全部解析/解码选项。
- Slide 测试 445,596 项检查，包括全部 4096 距离、重叠/环绕/多次调用、字典和 store/restore、
  简单参考解码器、暴力最短路径 oracle，以及 `adaptive/primary` 的长候选链与激活/恢复边界。
  530,000 次空回滚实际跨越 31 位年龄重定位边界，之后编码与清表对照逐字节相同。
- `tlg6_tests.cpp` 用独立逐位 writer、生产 reader 往返和线性颜色系数 oracle 检查 100,000 个随机残差块、
  800,000 项 Rice/writer/reader、300,000 项滤镜选择、2,000 组完整颜色系数、10,000 个任意位流。
- 图片测试以手写 wire bytes 验证 P1–P7、QOI opcode、TGA 调色板/RLE/方向/尾部、farbfeld BE16、
  DDS BC1/2/3 透明度和截断，以及输出失败不覆盖已有文件。
- AVIF 有 507 项检查，包含无损/布局、全部不透明和透明 alpha、裁剪/旋转/镜像、深度、动画、
  预乘 alpha、过大 ispe 声明和截断；另经 FFmpeg 颜色平面和 Pillow 完整 RGBA 解码核验。
- 792 项最新版 Rust 双向检查，加 21 个独立 image-rs 图片输出检查；测试中不改 Rust 库源码。

完整命令与算法保证边界见 [本次更新](../docs/upstream-v1.md)。下文 2026-10-10 的记录与不可变旧样本保持原样。

## 基础构建

正常测试只需 C++ 工具链，无需 Rust、图像库或外部测试框架：

```powershell
cmake --preset msvc
cmake --build --preset release
ctest --preset release
```

`tests.cpp` 对 TLG5、TLG6 的 Gray/RGB/RGBA 做 954 组逐字节往返。
尺寸覆盖 1、2、3、7、8、9、15、16、17、31、32、33 的笛卡尔积，
以及 1×129、129×1、65×73；数据包括零值、常数、梯度、棋盘格和
固定种子随机像素，专门覆盖块的部分尾部、单行与单列。

测试还检查确定性编码、SDS 字节长度元数据（中文、逗号、冒号、
等号、换行、NUL、空字符串）、非法尺寸/布局/格式、编码与解码资源限额、
所有前缀截断、异常块长度、TLG6 标志位、尾随数据策略、文件 I/O 错误，
以及编码失败时已有输出文件不被截断。完整图像且不包含扩展块的 SDS
前缀本身是合法文档，测试按此边界处理。

TLG5 另有手写的未压缩灰度流，固定跨行/跨列预测结果，覆盖 1、2、3、7、9、99
的块高，验证对合法非默认块高的读取；其预期像素不由编码器生成。

额外进行 4,000 次固定种子的有界字节突变和 500 个随机垃圾输入。
允许仍然合法的突变成功解码，但检查像素尺寸与缓冲长度一致；
失败必须包含错误说明与输入内的字节位置。这是确定性的回归扫查，
并不代表覆盖了任意输入空间。

`slide_tests.cpp` 以单独的简单参考解码器检查 LZSS 编码结果，
另有人工编写的位流固定字面量、17/18/273 字节匹配、距离 1/2 的重叠复制、
4096 字节环形字典边界、恰好一个窗口的距离、跨调用字典状态、初始化字典、
丢弃压缩结果后的 store/restore 和截断/输出溢出拒绝行为。正常 MSVC Release
运行只需很短时间；不依赖 encoder/decoder 恰好产生同一种错误的往返结果。

`allocation_tests.cpp` 在独立测试进程中拦截普通分配，设每次 64 KiB 的上限。
它构造声明大图像的短小无效 TLG5/TLG6 流、缺失熵块的 TLG6、超过元数据限额
或扩展块截断的 SDS，要求这些输入在申请图像或大解压缓冲前返回对应解析错误。
接口还通过编译期检查禁止从临时 `Image` 调用 `view()`。

CTest 还覆盖命令行帮助与原始像素文件往返。`msvc-asan` configure preset、
`asan` build/test preset 为上述测试启用 AddressSanitizer；非 MSVC 的
`TLG_ENABLE_SANITIZERS=ON` 同时启用 address/undefined sanitizers。

2026-10-10 在本机 MSVC 19.44.35228 上，Release、Debug、ASan RelWithDebInfo、
ASan Debug 及 `TLG_ENABLE_SIMD=OFF` 的 Release 构建均通过全部五项 CTest。
库测试包含 22,358 次检查，Slide 参考测试包含 26,336 次检查，另外验证了
中文文件路径及命令行元数据、CMake 安装后通过 `find_package(tlg)` 链接的独立应用。
这里记录本机结果；远端 MSVC 与 GCC/UBSan 的执行记录见 [GitHub Actions](https://github.com/tomh500/tlg-rs-cpp/actions/workflows/cpp.yml)。

## 与保留的 Rust 分支双向验证

[`fixtures/README.md`](fixtures/README.md) 记录不可变样本的 Rust 提交、
生成工具链与方法；[`fixtures/SHA256SUMS`](fixtures/SHA256SUMS) 固定全部二进制
样本。CTest 会解码 12 个 Rust TLG5/TLG6 样本和 2 个 Rust SDS 样本，
与原始像素及预期元数据比较。

TLG6 的小型 `tlg6_filters.bin` bundle 使用不变的 Rust Golomb 通道流和
32 种合法 filter/predictor code，预期图像全部来自原 Rust 直接解码器。
它独立覆盖所有 16 种颜色逆变换和 MED/AVG 两种预测，而非仅靠自身往返。

若要重跑在线双向检查，将 `xuanrikka` 分支提取到临时目录，保持其库源码
原样，只添加 `interop/rust_oracle.rs` 作为 example：

```powershell
git archive xuanrikka --output=build/oracle/rust-source.tar
New-Item -ItemType Directory -Force build/oracle/rust-source
tar -xf build/oracle/rust-source.tar -C build/oracle/rust-source
./tests/interop/run.ps1 -RustTree build/oracle/rust-source
```

该脚本构建原 Rust 的 `--release --no-default-features` example，逐个验证
C++ 编码→Rust 解码，以及 Rust 编码→C++ 解码；6 组图像、2 种格式合计
24 项方向检查。形状、布局和像素必须完全一致。可通过 `-Cargo`、
`-CppTool`、`-OutputDirectory` 指定工具及临时输出位置。原 Rust 高层
`TlgReader` 的签名和元数据缺陷已在 fixtures 说明中记录，因此像素 oracle
使用其直接 decoder，SDS 则由 C++ 测试独立验证。

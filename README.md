这么做的意义是什么就连我也不知道？

# tlg-cpp

一个使用现代 C++ 实现的 TLG 格式编解码库，支持 TLG5、TLG6 和 SDS 元信息。

## 什么是 TLG

TLG 是视觉小说引擎 Kirikiri2（krkr2）内置的一种无损图片格式，常见版本为 TLG5 和 TLG6。官方实现可参考 [tlg-wic-codec](https://github.com/krkrz/tlg-wic-codec)。

## 这个项目做了什么

- 基于原 Rust 实现重写 TLG5、TLG6 和 Slide 编解码器；
- 使用 `std::span`、`std::expected` 和标准库容器提供 C++ 接口；
- 支持 Gray、RGB、RGBA 像素与 SDS 元信息；
- 提供 CMake 构建、命令行工具和兼容性测试。

库本身只依赖 C++ 标准库。MSVC 使用 `/std:c++latest`，其他编译器需要支持 C++23 及 `std::expected`。原 Rust 实现保留在 `xuanrikka` 分支。

当前 C++ 版本为 1.1.0，已同步上游 Rust v1.0.0 的提交
[`10373b8`](https://github.com/XuanRikka/tlg-rs/commit/10373b87fd0d24fa38311b1b59758c7727624194)。
同步范围、算法选择与可复现的验证见 [本次更新](docs/upstream-v1.md)。

## 快速开始

### 添加依赖

通过 CMake 子目录加入项目：

```cmake
add_subdirectory(tlg-rs-cpp)
target_link_libraries(my_app PRIVATE tlg::tlg)
```

也可以安装后使用 `find_package`：

```powershell
cmake --preset msvc
cmake --build --preset release --parallel
cmake --install build/msvc --config Release --prefix C:/libraries/tlg
```

```cmake
find_package(tlg CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE tlg::tlg)
```

配置应用时，将安装目录加入 `CMAKE_PREFIX_PATH`。

### 示例：读取和写入 TLG 文件

```cpp
#include <tlg/tlg.hpp>
#include <iostream>

int main() {
    auto document = tlg::read_file("input.tlg");
    if (!document) {
        std::cerr << document.error().message << '\n';
        return 1;
    }

    auto result = tlg::write_file(
        "output.tlg", document->image.view(),
        {.format = tlg::Format::tlg6}, document->tags);
    if (!result) {
        std::cerr << result.error().message << '\n';
        return 1;
    }
}
```

`read_file` 自动识别 TLG5、TLG6 和 SDS 容器，`tags` 保存文件中的元信息。编码接口借用输入像素，不修改或转移其所有权。

### 示例：直接使用内存接口

```cpp
#include <tlg/tlg.hpp>
#include <array>

int main() {
    const std::array<std::uint8_t, 6> pixels{255, 0, 0, 0, 255, 0};
    const tlg::ImageView image{2, 1, tlg::PixelLayout::rgb, pixels};

    auto encoded = tlg::encode(image, {.format = tlg::Format::tlg5});
    if (!encoded) return 1;
    auto decoded = tlg::decode(*encoded);
    if (!decoded) return 1;
}
```

像素按从上到下的顺序紧密排列，通道顺序为 Gray、RGB 或 RGBA，不包含行填充。`encode` 的第三个参数可以传入元信息；非空元信息会写入 SDS 容器。长度按字节计算，内容可以包含逗号、冒号、等号和 NUL。

### 命令行

```powershell
build/msvc/Release/tlg_tool.exe encode --format 6 --width 1920 --height 1080 --layout rgba --tag "author=example" input.raw output.tlg
build/msvc/Release/tlg_tool.exe info output.tlg
build/msvc/Release/tlg_tool.exe decode output.tlg restored.raw
```

命令行工具也可以自动读取图片并转换为 TLG，或把 TLG 输出为图片；TLG 间转码保留 SDS 元信息。
便携格式不需要第三方库，Windows 默认通过系统 WIC 支持 PNG/JPEG/BMP/TIFF。
AVIF 可通过 `TLG_TOOL_AVIF=ON` 和包含 AV1 编码器、解码器的 libavif 1.3+ 启用。JPEG 输出仅接受 Gray/RGB，且须显式指定 `--allow-lossy`；需要降低输入精度时也须指定此参数。

```powershell
build/msvc/Release/tlg_tool.exe encode -v 6 input.png output.tlg
build/msvc/Release/tlg_tool.exe decode output.tlg restored.png
build/msvc/Release/tlg_tool.exe transcode --format 5 --slide-parse optimal input.tlg output.tlg
```

`detect` 只检查文件头中的 TLG 类型，`info` 验证并解码完整文件；`--help` 显示全部选项。

## 构建与测试

Windows 预设使用 Visual Studio 2022、MSVC x64 和 CMake 3.24 或更新版本：

```powershell
cmake --preset msvc
cmake --build --preset release --parallel
ctest --preset release
```

本机已验证 MSVC 19.44.35228 和 CMake 4.3.2。调试构建使用 `debug` build/test preset；AddressSanitizer 使用 `msvc-asan` configure preset 和 `asan` build/test preset：

```powershell
cmake --preset msvc-asan
cmake --build --preset asan --parallel
ctest --preset asan
```

若 ASan 运行时不在 `PATH`，请在 x64 Native Tools Command Prompt 中运行测试。其他支持 C++23 的工具链可直接使用 CMake：

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

## 接口说明

操作结果通过 `std::expected` 返回，错误包含类别、输入字节位置和说明。内部使用异常展开解析路径，构建需要启用 C++ 异常支持。

`DecodeOptions` 默认限制为 268,435,456 像素、1 GiB 图像字节、1 GiB 输入和 16 MiB SDS 扩展块，并拒绝裸图像尾随数据。`EncodeOptions` 默认使用 TLG6，限制相同的像素数量和 1 GiB 输入像素。调用者可以调整这些选项。

默认采用 `SlideParse::greedy`、`Tlg6FilterSearch::factored` 和 `SlideDecode::bulk`。
匹配搜索默认 `SlideMatch::adaptive`；`primary` 保留三字节索引，两版编码字节一致。
`optimal` 以更多计算和内存换取单个 Slide 块的最短编码，默认解析临时状态预算为 256 MiB，不含匹配索引和输出；
`reference` 和 `scalar` 保留完整枚举、逐字节实现，方便开发者对照实验。
`detect_format` 可在不分配图像的情况下识别裸 TLG5/TLG6 或 SDS 内嵌图像。

测试和 Rust 互操作方法见 [测试说明](tests/README.md)，算法见 [算法说明](docs/algorithms.md)，测量方法和结果见 [基准说明](docs/benchmark.md)，本次审查的复现与修复见 [审查记录](docs/review.md)。

## 鸣谢

- Kirikiri 引擎开发者；
- XuanRikka 的原 Rust 实现。

## 许可证

本项目使用 [MIT 许可证](LICENSE)。

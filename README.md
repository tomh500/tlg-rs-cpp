# tlg-cpp

为了伟大的轩酱。

从 OIER 和计算机科学学生的视角，用现代 C++ 重写的 Kirikiri TLG5/TLG6 无损图像编解码库。MSVC 使用 **`/std:c++latest`**，公开 API 使用 `std::span`、`std::expected` 和 RAII；库本身只依赖标准库。

原 Rust `master` 已保存在 **`xuanrikka`** 分支，新 **`main`** 保存 C++ 实现。原始提交为 `1f0d294e95b184bb9480d41e44d602d0e6a3dd7f`，历史与 MIT 许可证保留。

## 构建与测试

本机 Windows + Visual Studio 2022/MSVC，CMake 3.24 或更新版本：

本次验证使用 MSVC 19.44.35228 与 CMake 4.3.2。其他工具链也必须提供 `std::expected`。

```powershell
cmake --preset msvc
cmake --build --preset release --parallel
ctest --preset release
```

调试构建用 `debug` preset。AddressSanitizer 用 `msvc-asan` configure preset 和 `asan` build/test preset：

```powershell
cmake --preset msvc-asan
cmake --build --preset asan --parallel
# 如 ASan DLL 不在 PATH，在 x64 Native Tools Command Prompt 中运行：
ctest --preset asan
```

其他编译器需要支持 C++23 的标准库（包括 `std::expected`）：

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

## 在 C++ 中使用

作为子目录加入，链接 `tlg::tlg`：

```cmake
add_subdirectory(tlg-cpp)
target_link_libraries(my_app PRIVATE tlg::tlg)
```

也可 `cmake --install build/msvc --config Release --prefix <目录>`，然后通过 `find_package(tlg CONFIG REQUIRED)` 使用导出的同名 target。

```cpp
#include <tlg/tlg.hpp>
#include <iostream>

int main() {
    auto document = tlg::read_file("input.tlg");
    if (!document) {
        std::cerr << document.error().message << '\n';
        return 1;
    }
    auto saved = tlg::write_file("output.tlg", document->image.view(),
                                {.format = tlg::Format::tlg6}, document->tags);
    if (!saved) {
        std::cerr << saved.error().message << '\n';
        return 1;
    }
}
```

内存接口为 `encode(ImageView, EncodeOptions, Tags)` 和 `decode(span, DecodeOptions)`。编码借用像素，不复制输入、不消费图像。像素紧密排列，通道顺序为 Gray/RGB/RGBA，不要求对齐，也没有行填充。TLG5、TLG6 与 SDS 容器自动识别；包含中文、逗号、冒号、等号乃至 NUL 的元信息按字节长度正确处理。`Tags` 使用有序 map，输出确定且可复现。

错误携带 `ErrorCode`、字节位置和说明；内部解析错误与内存分配失败转换成 `std::unexpected`。默认解码限制为 268,435,456 像素、1 GiB 图像字节、1 GiB 输入、16 MiB SDS 扩展块，调用者可以通过 `DecodeOptions` 调整。默认编码也限制为相同像素数量和 1 GiB 输入像素，可通过 `EncodeOptions` 调整。解码默认拒绝裸图像尾随数据。

接口返回 `expected`，并不意味着应用无需 C++ 异常支持；内部使用异常展开解析路径，MSVC 库构建启用 `/EHsc`。

## 命令行

无需其他图像库即可处理原始像素文件：

```powershell
build/msvc/Release/tlg_tool.exe encode --format 6 --width 1920 --height 1080 --layout rgba --tag "作者=轩酱" input.raw output.tlg
build/msvc/Release/tlg_tool.exe info output.tlg
build/msvc/Release/tlg_tool.exe decode output.tlg restored.raw
```

工具支持 Unicode 路径，拒绝输入输出指向同一文件，`--help` 显示完整用法。`info` 会验证并解码文件；`decode` 输出原始像素，元信息可通过 `info` 或库接口读取。这里没有隐含 PNG/JPEG 转换依赖。

## 为什么值得重写

- 修复原 Reader 的签名消费、SDS 签名比较、Gray 图像构造和含标点元信息解析问题。
- 将尺寸乘法、块边界、位长度、游程、Slide 输出和分配上限纳入格式解析；不依赖补零掩盖截断。
- Slide 支持重叠匹配与完整尾部匹配；TLG5 原始平面使用零拷贝视图，解码接受任意正块高。
- TLG6 直接输出所需像素布局；候选缓冲放在栈上，计分考虑 alpha 和 Gray 的两种预测，用双遍编码控制额外驻留内存。
- 提供模块化算法、中文推导、不可变 Rust 样本、双向互操作、畸形输入测试、复现基准和可安装的 CMake target。

实测速度和压缩率取决于数据，比较结果与适用范围见 [基准说明](docs/benchmark.md)。测试与互操作方法见 [测试说明](tests/README.md)，算法、复杂度和不变量见 [算法说明](docs/algorithms.md)。

## 来源与许可证

保留 XuanRikka 的原 Rust 工作与 MIT 许可证。格式核对参考 [Kirikiri 官方实现](https://github.com/krkrz/krkrz/blob/master/visual/LoadTLG.cpp)。`xuanrikka` 分支保留 Rust 生态的原 API；C++ 主线使用独立的标准库接口。

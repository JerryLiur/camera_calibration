# Spike 标定工具 · Qt 6 / C++ / OpenCV

这是原 Python（HTTP + WebView2 + 浏览器工作台）实现的**技术栈重构版**，用于直接对接相机厂商的 C++ SDK。

- **界面**：Qt 6 Widgets 原生窗口，不再需要 Python、HTTP 服务、浏览器或 WebView2 Runtime。
- **算法**：C++17 + OpenCV，标定流程、校验规则、坐标约定、输出 JSON 结构与 Python 版保持一致。
- **设备**：新增 `ICameraDevice` 设备边界和一个独立采集线程，厂商 SDK 通过一个 C 函数指针表接入，不污染算法与 UI 代码。
- **数据**：继续读写 `spike_calibration_workspace/*.json`，Python 版已有工程、角点 JSON、导出标定包可以直接使用。

标定对象仍是「共用一个镜头，经 1:1 分光器进入 Spike」的完整成像支路；RGB 联合标定、六自由度外参不在范围内。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/core/` | 配置/观测校验、靶标检测与覆盖、Spike 重建与文件导入、内参求解、独立验证、转台零位、视线查询、工程持久化、C++ SDK 读取类 |
| `src/device/` | `ICameraDevice` 接口、离线文件回放设备、厂商 SDK 适配器、采集线程控制器 |
| `src/ui/` | Qt Widgets 主窗口与 5 个流程页（采集/内参/转台/配置/设备） |
| `src/app/main.cpp` | 桌面入口 |
| `tests/selftest.cpp` | 控制台几何自检，可用仓库里的模拟真值数据做跨语言对照 |
| `examples/VendorSdkShim.*` | 厂商 SDK 接线模板 |
| `docs/ARCHITECTURE.md` | Python → C++ 模块映射、数据流与线程模型 |
| `docs/SDK_INTEGRATION.md` | 接入真实 C++ SDK 的完整步骤 |

## 依赖

- CMake ≥ 3.21
- Qt **6.2+**（`Core`、`Gui`、`Widgets`）
- OpenCV **4.x 或 5.x**（`core`、`imgproc`、`calib3d`、`imgcodecs`）
- C++17 编译器（Windows 推荐 MSVC 2022；Linux/macOS 用 GCC/Clang 亦可）

以 vcpkg + MSVC 为例：

```powershell
vcpkg install opencv4:x64-windows
cmake -S qt -B qt/build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="<Qt6 安装目录>/msvc2022_64" `
  -DCMAKE_TOOLCHAIN_FILE="<vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build qt/build --config Release
```

若 OpenCV 不是通过 CMake package 提供的，可显式指定 `-DOpenCV_DIR=<包含 OpenCVConfig.cmake 的目录>`。

## 运行

```powershell
qt\build\Release\SpikeCalibrationTool.exe --data E:\code\camera_calibration\spike_calibration_workspace
```

`--data` 缺省为当前工作目录下的 `spike_calibration_workspace`。Windows 分发时用 `windeployqt` 补齐 Qt 运行库，并把 OpenCV 的 DLL 放到 exe 同目录。

界面流程与 Python 版一致：

1. 新建工程 → 在「04 工程配置」按实测填写标定板、方向、光路记录与验收阈值。
2. 「01 采集与检查」导入图像 / 二值 Spike NPY / 明确布局的 DAT/BIN / 角点 JSON，检查角点、用途划分与视场覆盖。
3. 「02 内参与畸变」求解（≥6 个有效训练姿态）→ 独立验证（冻结内参）→ 像素查询视线。
4. 「03 转台关系」录入编码角观测，选择远场或近距离已知几何求单轴零位。
5. 「05 设备连接与控制」连接相机 C++ SDK，预览并把停稳的采集帧直接导入当前工程；顶部可导出标定包 JSON 与自包含 HTML 报告。

## 自检与跨语言对照

```powershell
cmake --build qt/build --config Release --target spike_selftest
ctest --test-dir qt/build -C Release --output-on-failure
```

也可以直接运行，并让 C++ 版读取仓库里已有的独立模拟真值数据做数值对照：

```powershell
qt\build\Release\spike_selftest.exe `
  --data qt\build\selftest_workspace `
  --output qt\build\selftest_output `
  --observations output\mock_calibration_data\scenes\clean `
  --ground-truth output\mock_calibration_data\ground_truth.json
```

自检覆盖：内参求解、留出验证、导出包与 C++ SDK 回读、模拟真值 K/畸变、视线方位/俯仰角、远场与有限距离转台零位。输出 `self_test.json`（含逐项实测值）、`calibration.json`、`report.html`。

## 与 Python 版的差异（算法流程不变）

| 项目 | 说明 |
| --- | --- |
| 进程模型 | 去掉 HTTP 服务与 WebView2；`Workbench` 直接被 Widgets 调用，工程文件仍是同一套 JSON |
| 合成夹具 | 噪声用 `std::mt19937_64 + normal_distribution(0, 0.035)`，与 NumPy PCG64 不是逐位相同，但统计特性与容差一致 |
| 数值健壮性 | `intrinsic_std` 中出现非有限值时写为 `null`，避免 `dumps(allow_nan=False)` 失败（原版已知问题） |
| 圆点阵检测 | 使用 `cv::findCirclesGrid` 标准重载并捕获异常；OpenCV 5 的 `CirclesGridFinderParameters` 回退路径未移植 |
| ChArUco | 原版 `validate_config` 本就不允许该类型，因此未移植（需要 opencv-contrib） |
| 数字签名 | `sha256` 与工程 `config_fingerprint` 的字节构造保持 Python 语义；`json.dumps` 缩进由 Qt 的 4 空格改为 Qt 默认缩进，JSON 语义不变 |
| 传输 | 不再有 64 MiB HTTP 请求体与 Base64 上传；UI 直接读本地文件，单个文件仍按 64 MiB 上限校验 |

原 Python 源码保留在仓库根目录，作为算法与数据契约的参考实现；`scripts/check_mock_calibration.py` 仍可用于对同一份模拟数据做双实现对照。

## 接入真实相机 SDK

见 [docs/SDK_INTEGRATION.md](docs/SDK_INTEGRATION.md)。核心是：实现 `VendorSdkApi` 的 `open / close / setInt / setDouble / grab` 回调 → 启动时调用 `registerVendorSdkApi()` → 界面「05 设备连接与控制」即可连接、预览、采集。未注册适配器时界面仍可用「离线文件回放」走通全部流程。

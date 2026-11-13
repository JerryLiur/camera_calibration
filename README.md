# Spike 单支路标定工具

针对共用一个镜头、镜头与传感器之间由 1:1 分光器分光的光路，只标定完整 Spike 成像支路。提供 Windows 桌面窗口、本机浏览器工作台、Python SDK 和命令行。

## 功能

- 求解内参 `fx、fy、cx、cy` 和五项畸变 `k1、k2、p1、p2、k3`。
- 导入图像、二值 Spike NPY、明确布局的位打包 DAT/BIN 和角点 JSON；检查角点质量及视场覆盖。
- 通过独立采集序列验证冻结内参，报告重投影误差。
- 可选单轴转台水平零位求解，支持远场和已知有限距离几何。
- 像素到单位视线/角度查询，导出标定 JSON 和自包含 HTML 报告。

本仓库是离线工具源码。RGB 联合标定、相机在线采集、转台控制和完整六自由度外参不在本版范围内。合成示例和软件测试不代表真实设备标定精度。

## Qt 6 / C++ / OpenCV 重构版（设备接入）

相机 SDK 是 C++ 的，为便于设备连接与控制，已新增同流程、同算法的原生实现：[`qt/`](qt/README.md)。

- 界面改为 **Qt 6 Widgets** 原生窗口，去掉 Python、本机 HTTP 服务、浏览器与 WebView2 Runtime。
- 算法与校验用 **C++17 + OpenCV** 重写，标定流程、坐标约定、工程 JSON、导出包和 HTML 报告与 Python 版兼容；`spike_calibration_workspace` 里的工程可直接打开。
- 新增 `ICameraDevice` 设备边界与独立采集线程：厂商 SDK 通过一个函数指针表接入，界面可连接设备、调曝光/增益、实时预览并把停稳帧直接采集成标定观测。接入步骤见 [SDK 接入说明](qt/docs/SDK_INTEGRATION.md)，模块映射见 [架构说明](qt/docs/ARCHITECTURE.md)。
- 控制台自检 `spike_selftest` 可读取本仓库的模拟真值数据，对 K、畸变、视线角和转台零位做跨实现数值对照。

构建、测试与运行：

```powershell
cmake -S qt -B qt/build -DCMAKE_PREFIX_PATH="<Qt6 安装目录>"
cmake --build qt/build --config Release
ctest --test-dir qt/build -C Release --output-on-failure
qt\build\Release\spike_selftest.exe --observations output\mock_calibration_data\scenes\clean --ground-truth output\mock_calibration_data\ground_truth.json
```

`ctest` 包含三个用例：几何自检、模拟数据集成测试（导入链路、7 个场景矩阵、文件回放设备、模拟厂商 SDK、持久化）和 Qt Widgets 冒烟测试。集成测试需要 `output\mock_calibration_data`，缺失时会自动 SKIP。

根目录下的 Python 源码保留为算法与数据契约的参考实现，不再作为设备联调的主技术栈。

## 安装与启动

验证环境为 Windows、Python 3.12。先克隆仓库，在仓库根目录执行：

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-desktop.txt
.\启动Spike标定工具.cmd
```

桌面模式需要 Microsoft Edge WebView2 Runtime。安装依赖后也可直接运行：

```powershell
.\.venv\Scripts\python.exe -m spike_calibration_tool desktop
```

仅使用浏览器模式时，安装 `requirements.txt` 即可，不需要桌面依赖：

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\启动Spike标定工具.ps1 -Browser
```

或使用前台命令（Ctrl+C 停止）：

```powershell
.\.venv\Scripts\python.exe -m spike_calibration_tool serve --port 8776 --open
```

服务只监听 `127.0.0.1`。默认工作目录为 `spike_calibration_workspace`，不纳入 Git；可通过命令行 `--data <目录>` 指定其他位置。详细数据格式、坐标约定和操作流程见 [使用说明](spike_calibration_tool/README.md)，接口约定见 [开发说明](spike_calibration_tool/DEVELOPMENT.md)。

## 测试

```powershell
.\.venv\Scripts\python.exe -m unittest tests.test_spike_calibration_engine tests.test_spike_calibration_tool -v
.\.venv\Scripts\python.exe scripts/verify_spike_calibration_tool.py --output output/http-verification
.\.venv\Scripts\python.exe -m spike_calibration_tool --data output/demo-workspace demo --output output/demo
```

测试包含参数求解、独立验证、数据导入、持久化、HTTP 流程、SDK 和桌面导出接口。HTTP 检查使用临时工作区和临时端口；合成数据始终标记为 `synthetic`。

## 构建 Windows 版本

安装 `requirements-desktop.txt` 后执行：

```powershell
.\spike_calibration_tool\build_windows.ps1
```

输出位于 `output/spike_calibration_windows/Spike标定工具/Spike标定工具.exe`。分发时保留整个目录及 `_internal`；打包版无需 Python，仍需 WebView2 Runtime。

## 源码组织

| 路径 | 内容 |
| --- | --- |
| `qt/` | **新版**：Qt 6 Widgets + C++ + OpenCV 桌面实现，含 C++ 相机 SDK 接入层 |
| `spike_calibration_tool/` | Python 参考实现：求解、导入、工程管理、HTTP、SDK、桌面和命令行 |
| `spike_calibration_tool/web/` | Python 版本机工作台界面，无外部前端 CDN 依赖 |
| `calibration_system/targets.py` | 原工程共用的靶标检测、质量及覆盖分析 |
| `rgb_spike_fusion/spatial_calibration.py` | 原工程共用的几何标定辅助实现 |
| `tests/` | 标定工具回归测试 |
| `scripts/verify_spike_calibration_tool.py` | HTTP 全流程验证 |

共用模块保留原算法实现，包初始化仅加载本仓库提供的子集。仓库不包含采集数据、工作工程、虚拟环境、历史材料或生成的 EXE。

# Spike 单支路标定工具

针对共用一个镜头、镜头与传感器之间由 1:1 分光器分光的光路，只标定完整 Spike 成像支路。提供 Windows 桌面窗口、本机浏览器工作台、Python SDK 和命令行。

## 功能

- 求解内参 `fx、fy、cx、cy` 和五项畸变 `k1、k2、p1、p2、k3`。
- 导入图像、二值 Spike NPY、明确布局的位打包 DAT/BIN 和角点 JSON；检查角点质量及视场覆盖。
- 通过独立采集序列验证冻结内参，报告重投影误差。
- 可选单轴转台水平零位求解，支持远场和已知有限距离几何。
- 像素到单位视线/角度查询，导出标定 JSON 和自包含 HTML 报告。

本仓库是离线工具源码。RGB 联合标定、相机在线采集、转台控制和完整六自由度外参不在本版范围内。合成示例和软件测试不代表真实设备标定精度。

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
| `spike_calibration_tool/` | 求解、导入、工程管理、HTTP、SDK、桌面和命令行 |
| `spike_calibration_tool/web/` | 本机工作台界面，无外部前端 CDN 依赖 |
| `calibration_system/targets.py` | 原工程共用的靶标检测、质量及覆盖分析 |
| `rgb_spike_fusion/spatial_calibration.py` | 原工程共用的几何标定辅助实现 |
| `tests/` | 标定工具回归测试 |
| `scripts/verify_spike_calibration_tool.py` | HTTP 全流程验证 |

共用模块保留原算法实现，包初始化仅加载本仓库提供的子集。仓库不包含采集数据、工作工程、虚拟环境、历史材料或生成的 EXE。

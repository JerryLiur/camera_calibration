# 架构与模块映射

## 分层

```
┌──────────────────────────────────────────────────────────────┐
│ src/ui           Qt 6 Widgets，仅负责呈现与输入                  │
│  MainWindow / CapturePanel / IntrinsicsPanel / TurntablePanel  │
│  ConfigPanel / DevicePanel / ImageViewer                       │
└───────────────┬──────────────────────────────────────────────┘
                │ 直接调用（无 HTTP、无 IPC）
┌───────────────▼──────────────────────────────────────────────┐
│ src/core/Workbench  工程持久化、失效规则、导出与报告            │
├──────────────────────────────────────────────────────────────┤
│ src/core 算法层（Qt Core/Gui + OpenCV，不依赖 Widgets）         │
│  Model / Targets / Importers / Calibration / Fixtures / SDK    │
├──────────────────────────────────────────────────────────────┤
│ src/device  ICameraDevice ← 厂商 C++ SDK 适配器 / 文件回放      │
│              DeviceWorker 运行在 QThread 采集线程               │
└──────────────────────────────────────────────────────────────┘
```

`spike_core` 是静态库，只链接 `Qt6::Core`、`Qt6::Gui` 与 OpenCV；`SpikeCalibrationQt` 链接 `Qt6::Widgets`。因此算法层可被 `spike_selftest`、`CalibrationSDK` 或你们自己的设备程序单独复用，不需要拉起界面。

## Python → C++ 模块映射

| Python | C++ | 说明 |
| --- | --- | --- |
| `spike_calibration_tool/engine.py` | `src/core/Model.cpp`（配置/观测校验、视线查询）<br>`src/core/Calibration.cpp`（求解/验证/转台） | 函数一一对应：`validate_config`、`validate_capture`、统计、签名、退化检查、`solvePnP` + `projectPoints`、单轴零位 |
| `rgb_spike_fusion/spatial_calibration.py::fit_camera_intrinsics` | `Calibration.cpp::fitCameraIntrinsics` | `cv::calibrateCameraExtended`（flags=0，criteria 100/1e-9），返回 K、畸变、逐视图 RMS、内参标准差 |
| `calibration_system/targets.py` | `src/core/Targets.cpp` | 灰度拉伸、质量指标、棋盘/圆点检测、6×4 覆盖引导、Spike `count/rate/interval` 重建、旋转镜像 |
| `spike_calibration_tool/importers.py` | `src/core/Importers.cpp` | 图片 `imdecode`、`.npy` v1/v2/v3 头解析、DAT `packed_flat/row_packed` 位解包、预览 PNG/Base64 |
| `spike_calibration_tool/fixtures.py` | `src/core/Fixtures.cpp` | 16 姿态合成夹具，train 12 / test 4，独立 `sequence_id` |
| `spike_calibration_tool/service.py` | `src/core/Workbench.cpp` | 工程 CRUD、revision 冲突、几何配置变更清空观测、阈值变更失效验证、导出包、HTML 报告、角点预览 |
| `spike_calibration_tool/sdk.py` | `src/core/CalibrationSDK.cpp` | 读取导出 JSON 并查询视线，供下游 C++ 程序调用 |
| `spike_calibration_tool/server.py` | **移除** | 本机 HTTP 与 CSP/Host 校验不再需要；UI 直接调用 `Workbench` |
| `spike_calibration_tool/desktop.py` | **替换为 `src/ui/`** | 不再依赖 pywebview / WebView2 |
| `spike_calibration_tool/cli.py` | `src/app/main.cpp` + `tests/selftest.cpp` | `--data` 与 `demo/self-test` 分别对应 GUI 入口与控制台自检 |
| `spike_calibration_tool/web/*` | `src/ui/*` | 四个流程标签页 + 新增设备页；交互语义（忙碌锁定、确认框、失效提示）保留 |

## 数据流

**离线**：`文件 → importCapture → detectTarget → Capture JSON → Workbench 保存 → solveIntrinsics → validateIntrinsics → queryRays → exportProject/reportHtml`

**在线（新增）**：`厂商 SDK → DeviceWorker(QThread) → frameReady(QImage) → 界面预览`；`captureStill → PNG → Workbench::importFiles → 与离线完全相同的检测与校验链路`。采集帧不绕过校验，也不会改写原始脉冲数据。

## 线程模型

- UI 线程：`MainWindow` / 各 Panel / `Workbench`（`std::recursive_mutex` 保护工程内存与磁盘写入）。
- 采集线程：`DeviceWorker` 持有 `ICameraDevice`，用 `QTimer` 按间隔触发 `grab()`，因此厂商 SDK 的阻塞采集不会卡住界面，停止预览也能及时被事件循环处理。
- 跨线程只传 `QImage`、`QByteArray`、`QVariantMap` 等 Qt 值类型；`cv::Mat` 不会跨线程逃逸。
- `DeviceController` 析构时用阻塞队列调用 `closeDevice()`，确保 SDK 句柄被释放。

## 错误处理

所有校验失败统一抛 `spike::Error`（`std::runtime_error` 子类，携带中文说明）。`ProjectPanel::run()` 捕获后通过 `notify(text, true)` 显示；`spike_selftest` 直接返回非零退出码。对应原版 `{error, code}` 的 400/404/403/500 分级在进程内不再需要，但错误文案保持一致。

## 保持不变的数据契约

- 工程文件：`<workspace>/<32位小写十六进制 id>.json`，字段 `id,name,config,captures,model,validation,turntable,revision,status,updated_at`。
- `config`：`target/orientation/optics/acceptance/operator`，几何配置变更清空 `captures` 并使结果失效；只改阈值保留 K，清除验证/转台结果。
- `capture`：`image_points` / `object_points` 位于 `transformed_pixels` 域，`source.sha256` 为文件哈希，训练与验证的 `sequence_id` 不得交叉，`captureSignature` 用于拦截「改文件名冒充独立验证」。
- 导出包：`format=spike-calibration-bundle`、`format_version=1`、`hardware_certified=false`、`limitations` 三条。
- 模型：`pinhole_brown_5`，K 3×3、畸变 5 参数、逐视图 RMS、覆盖网格、姿态约束奇异值。
- 坐标：像素左上原点；方位右正、俯仰上正；`ray_camera` 为 X 右 / Y 下 / Z 前。

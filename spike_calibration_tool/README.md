# Spike 标定工具 V1.0

Windows 本机标定工具，基于项目已有 `calibration_system` 靶标检测和覆盖分析能力开发。标定对象是“共用一个镜头，经 1:1 分光器进入 Spike”的完整成像支路。原 RGB＋Spike 标定软件保持独立。

## 启动

Windows 打包版直接双击同目录 `Spike标定工具.exe`；请保留整个目录和 `_internal`。无需另装 Python，需要 Microsoft Edge WebView2 Runtime。

源码工作区双击根目录 `启动Spike标定工具.cmd`。默认在独立 Windows 窗口运行；关闭窗口会关闭本次窗口的 HTTP 服务。需要 Microsoft Edge WebView2 Runtime。

使用浏览器工作台：

```powershell
./启动Spike标定工具.ps1 -Browser
```

默认地址 `http://127.0.0.1:8776`，只监听本机。源码工程存储在项目根目录 `spike_calibration_workspace`；Windows 打包版存储在 `%LOCALAPPDATA%/SpikeCalibrationTool/workspace`，日志在对应目录的 `logs`。重新启动可继续读取已保存工程。

源码环境使用根目录 `.venv`。首次使用请按[仓库安装说明](../README.md)创建 Python 3.12 环境，安装 `requirements-desktop.txt`；仅浏览器模式安装 `requirements.txt` 即可。核心依赖为 NumPy 和 OpenCV，桌面另需 pywebview。

## 操作步骤

1. 新建工程，在“工程配置”按实际标定板填写内角点列数/行数和格长（米），确认镜头、对焦和分光器安装记录。初始 9×6、25 mm 仅为可编辑示例，不能代替实际测量。
2. 在完整正式光路下停稳标定板，改变板位置、倾角和清晰距离，建议首轮采集 20～30 个不同姿态；仅沿导轨前后移动不足以形成良好标定。
3. 按文件导入图像、二值 Spike NPY 或明确布局的位打包 DAT/BIN。每次指定真实采集序列以及求解/独立验证用途；同一采集序列不能跨两种用途。可导入工具约定的角点 JSON。
4. 检查每组预览、角点数量、质量和视场覆盖；排除不合适的观测。选择“求解内参”，至少需要 6 个可用且姿态有变化的训练视图。数学最低数量不是工程充分性保证。
5. 运行“独立验证”。验证使用冻结内参估计留出板姿态并计算重投影残差，不更新内参。未填写验收阈值时只报告实测值；这是几何一致性验证，不能替代独立角度计量真值。
6. 若需要机构角度，在“转台关系”录入单轴编码器读数和同一固定参考点的像素位置。远场模型需要明确确认远场条件；近距离模式要求填写同一参考坐标系中的目标位置和镜头相对旋转中心偏移，单位米。
7. 查询目标像素对应的单位视线、水平角和俯仰角，导出标定 JSON 包和自包含 HTML 报告。桌面版使用原生保存对话框；浏览器版使用浏览器下载。

## 数据与坐标约定

- 图像支持 PNG/JPEG/TIFF/BMP 等。NPY 必须为无 pickle 的二值 `[T,H,W]` 数组。支持脉冲计数、间隔表示，以及在提供已核实采样周期后使用发放率表示。
- DAT/BIN 只支持用户明确声明的 `packed_flat`（逐帧连续位）或 `row_packed`（每行独立字节对齐）布局，需确认宽、高、bitorder、文件头、帧步长等元数据。不自动猜测相机厂商封装；未知布局先完成解码核验。
- 单个 HTTP 请求最多 64 MiB，Base64 约有 1/3 膨胀。大文件应先截取停稳姿态的完整有效数据窗，再分批导入；不能在工具中把截取窗当作独立采集序列。
- 配置中的旋转和镜像作用于重建图与特征坐标，原始 Spike 不修改。角点 JSON 和后续 SDK 查询输入必须已经使用同一方向配置，坐标域为 `transformed_pixels`。
- 像素原点位于左上，向右/向下增长；输出方位角向右为正、仰角向上为正。转台参考坐标在零编码器位置采用右手系 x 向右、y 向下、z 向前，正向单轴绕 +y 转动；`sign` 绑定实际编码器方向。
- 修改标定板、方向、镜头或分光器配置会清除本工程旧观测及结果；请先导出或新建工程。只修改阈值/操作者保留已求内参，清除验证/转台结果，需重新验证。
- 合成示例只能载入空工程；所有来源始终标记 `synthetic`，导出包不会认证实机合格。实际设备参数、采样周期、距离和验收阈值不由演示值推定。

角点文件示例结构（完整结构可参考导出包 `project.captures`，数组应填写真实点对）：

```json
{"captures":[{"name":"pose-01","sequence_id":"capture-fit-01","split":"train","evidence":"hardware","enabled":true,"found":true,"image_size":[1000,1000],"object_points":[[0,0,0]],"image_points":[[300,400]],"source":{"coordinate_domain":"transformed_pixels"}}]}
```

以上单点仅说明字段，不能用于求解。有效观测至少需 6 对非共线点。

## SDK 和命令行

```python
from spike_calibration_tool.sdk import SpikeCalibrationSDK
calibration = SpikeCalibrationSDK("calibration.json")
result = calibration.query([[500, 500]], encoder_deg=0.0)
```

完整合成数值自检：

```powershell
./.venv/Scripts/python.exe -m spike_calibration_tool --data output/spike_tool_demo_workspace demo --output output/spike_tool_demo
./.venv/Scripts/python.exe -m unittest tests.test_spike_calibration_engine tests.test_spike_calibration_tool -v
```

构建独立 Windows 目录：

```powershell
./spike_calibration_tool/build_windows.ps1
```

生成 `output/spike_calibration_windows/Spike标定工具/Spike标定工具.exe`，需保留整个目录及 `_internal`。打包版无需 Python；仍需 WebView2 Runtime。

## 实机边界

本版支持离线工程流程，没有伪装成在线的相机或运动机构连接。真实硬件在线采集、机构控制和独立角度计量需要具体 SDK/协议及设备联调。单轴模型只求方向/零位，不把单轴数据解释成完整六自由度外参；有限距离模型依赖用户输入的真实几何。拆装光路、重新对焦或改变工作配置后应重新标定。

## 开发团队与证据

开发时采用 Backend Architect、Frontend Developer、Test Automation Engineer 角色；主代理负责集成、持久化、桌面与交付。API 见 `DEVELOPMENT.md`。原工作区的历史验证材料不随源码分发，可按根目录说明重新运行测试并在 `output/` 生成报告。软件自检和浏览器验证与真实设备验收分开记录。

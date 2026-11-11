# Spike 标定工具开发约定

依据 Spike 单目标定方案与已有靶标、几何模块开发。本仓库提取所需共用模块，保留原算法实现；两包初始化仅声明本仓库提供的子集。原工作区的采集数据、历史交付材料和开发环境不随源码分发。

## 范围

共用一个镜头，经 1:1 分光器到两传感器；只标定完整 Spike 支路。原始脉冲不插值、不改写；图像/坐标方向统一记录。支持离线图像、二值 Spike NPY、明确布局的位打包数据、角点 JSON；多姿态内参/畸变；独立验证；单轴转台方向/零位；像素视线查询；标定 JSON 和 HTML 报告；Windows 启动入口。没有实机连接时明确显示离线，不虚构硬件精度。

## 团队与文件所有权

- 主代理：服务、HTTP、持久化、SDK/CLI、桌面入口、启动脚本、说明与最终集成。
- Backend Architect：`engine.py`、`importers.py`、`fixtures.py`、`tests/test_spike_calibration_engine.py`。
- Frontend Developer：`web/index.html`、`web/app.js`、`web/styles.css`。
- Test Automation Engineer：`tests/test_spike_calibration_tool.py`、`scripts/verify_spike_calibration_tool.py`、独立验证报告。

以上为开发时的角色分工记录。原开发工程保持独立，当前仓库可自行安装、测试及构建。

## 项目/API 契约

本机服务端口默认 8776，项目存储 `spike_calibration_workspace`。所有写请求 `application/json`，结果直接返回对象；错误 `{error, code}`。

`GET /api/state` → `{app:'spike-calibration-tool',version,projects:[{id,name,status,capture_count,updated_at}],capabilities}`。

`POST /api/projects {name}` → 完整项目。

完整项目：`{id,name,config,captures,model,validation,turntable,revision,status,updated_at}`。所有变更接口返回完整项目，query 例外。

`config`：`{target:{kind:'chessboard',cols:9,rows:6,square_length_m:0.025},orientation:{rotation_deg:0,flip_x:false,flip_y:false},optics:{shared_lens:true,split_ratio:'1:1',lens_id:'',focus_note:'',splitter_note:''},acceptance:{intrinsics_p95_px:null,turntable_p95_deg:null},operator:''}`。板尺寸默认仅为可编辑示例，界面应提示实测填写。

- `GET /api/projects/{id}`：读取。
- `PATCH /api/projects/{id}/config {config}`：保存并使旧结果失效，改变几何配置时清空旧采集，前端明确确认。
- `POST /api/projects/{id}/demo {}`：载入明确为 synthetic 的多姿态夹具，不自动求解。
- `POST /api/projects/{id}/import {files:[{name,content_base64}],split:'train'|'test',sequence_id, evidence:'hardware'|'public',options:{...}}`：图片/NPY/明确布局 DAT；每组用户填写采集序列。同名文件去重/拒绝。小批量请求总上限 64MiB。
- `POST /api/projects/{id}/observations {captures:[...]}`：角点 JSON 导入；格式同 capture。
- `PATCH /api/projects/{id}/captures/{capture_id} {enabled,split}`：保留观测，更新是否参与求解/验证，旧结果失效。
- `POST /api/projects/{id}/solve {}`：真实数值求解（前端显示忙碌并 await）。
- `POST /api/projects/{id}/validate {}`：仅使用 test 集验证冻结内参，不修改模型。
- `POST /api/projects/{id}/turntable {observations:[{sequence_id,split,encoder_deg,pixel:[u,v]}],reference:{mode:'far_field',reference_angle_deg:0,sign:1,far_field_confirmed:true}}`：远场角度模型。近距离模式 `mode:'finite_distance',target_position_m:[x,y,z],camera_offset_m:[x,y,z],sign:1`，由已知几何处理旋转中心偏移；只标定单轴零位，不估计完整六自由度。需要独立 train/test 序列。
- `POST /api/projects/{id}/query {points:[[u,v]],encoder_deg:0}` → `{points:[{pixel,ray_camera,azimuth_deg,elevation_deg,...}],coordinate_domain}`。
- `GET /api/projects/{id}/preview/{capture_id}`：PNG，优先带角点叠加；`?undistort=1` 展示去畸变。
- `GET /api/projects/{id}/export`：下载 JSON（配置、观测、模型、验证、限制和来源）。
- `GET /api/projects/{id}/report`：下载自包含 HTML 报告。

capture：`{id,name,sequence_id,split:'train'|'test',evidence:'synthetic'|'hardware'|'public',enabled:true,image_size:[W,H],object_points:[[X,Y,Z]],image_points:[[u,v]],found:true,quality:{...},preview_base64:'<PNG base64>',source:{...}}`。未检测到靶标允许保存 `found:false` 与空点。所有像素均为配置指定方向变换后的坐标；以 transformed_pixels 标记。

## 算法模块契约

- `fixtures.demo_captures(config) -> list[capture]`，确定性多姿态，train/test 按独立 sequence 分开，图像预览与点相符。
- `importers.import_capture(file_record, config, *, split, sequence_id, evidence, options=None) -> capture`，不得读任意外部路径、不得猜未知 DAT 布局。NPY allow_pickle=False。
- `engine.validate_config(config) -> config`，返回规范化/校验后的完整配置。
- `engine.solve_intrinsics(captures, config) -> model`：至少6个有效训练姿态，屏蔽 test 参与优化，输出 K/distortion/image_size、逐视图残差/覆盖/警告、source evidence。
- `engine.validate_intrinsics(model,captures,config) -> validation`：独立 test；输出 stats/p95_px/status；未配置阈值保持 measured 而非 passed。
- `engine.fit_turntable(model,observations,reference,config) -> turntable`：方向/零位、train/test残差、适用边界。
- `engine.query_rays(model,points,turntable=None,encoder_deg=None) -> dict`：输入已经是统一方向坐标；返回相机单位视线、角度（右正/上正），可选单轴机构角。

## 验证要求

验证数值几何、留出隔离、镜像旋转、坏输入、缺姿态、几何变更失效、持久化、HTTP与实际浏览器完整操作链。合成通过与实机验收严格区分。不得擅自为焦距、工作距离、验收阈值填写设备事实。

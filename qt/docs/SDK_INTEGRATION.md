# 接入相机厂商 C++ SDK

重构的主要动机是「设备的连接和控制」。本工程把 SDK 隔离在一个稳定边界后面，算法与界面代码都不 include 厂商头文件。

## 边界

`src/device/ICameraDevice.h`：

```cpp
class ICameraDevice {
public:
    virtual QString backend() const = 0;
    virtual bool open(const QVariantMap& parameters, QString* error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QVariantMap parameters() const = 0;
    virtual bool setParameter(const QString& key, const QVariant& value, QString* error) = 0;
    virtual bool grab(DeviceFrame& frame, int timeoutMs, QString* error) = 0;
};
```

`open/close` 在采集线程调用；`grab` 只在采集线程调用。请**不要**在回调里操作 QWidget 或 `Workbench`。

`VendorSdkCameraDevice` 已经实现了这个接口，它不认识具体 SDK，只调用一张 C 函数指针表 `VendorSdkApi`（`src/device/VendorSdkCameraDevice.h`）：

| 回调 | 责任 | 返回值 |
| --- | --- | --- |
| `version()` | SDK/固件版本字符串，可为空 | `const char*` |
| `open(connection, error, errorCapacity)` | 枚举/打开设备，写入失败原因 | `void*` 句柄；失败返回 `nullptr` |
| `close(handle)` | 释放句柄、停止采集流 | — |
| `setInt(handle, key, value)` | 整型参数：`exposure_us`、`gain`、`trigger_mode`、`binning` | 0 成功，非 0 失败 |
| `setDouble(handle, key, value)` | 浮点参数（如帧率、增益 dB） | 0 成功 |
| `getInt(handle, key, int*)` | 读回参数（可选） | 0 成功 |
| `grab(handle, data, capacity, w, h, channels, timeoutMs)` | 拷贝一帧 8 位数据到 `data`，写回宽高/通道数 | `0` 成功、`1` 超时、`<0` 错误 |

`grab` 的 `capacity = max_width × max_height × channels`，界面默认 4096×3000×1；按真实传感器分辨率调整。返回 1（超时）不会弹错误，便于连续预览。通道数支持 1（灰度）、3（BGR）、4（BGRA，自动转 BGR）。

## 三步接入

### 1. 写适配层

复制 `examples/VendorSdkShim.h/.cpp` 到你们自己的目录（例如 `camera_sdk/`），把 TODO 替换为真实 SDK 调用：

```cpp
#include "VendorSdkShim.h"
#include "device/CameraDeviceFactory.h"
#include "VendorCameraApi.h"          // 只有这一个文件 include 厂商头

#include <cstdio>
#include <cstring>

namespace {
Vendor::Camera* g_camera = nullptr;

void* shimOpen(const char* connection, char* error, int errorCapacity)
{
    try {
        g_camera = Vendor::Camera::Open(connection);   // 厂商 C++ SDK
        g_camera->Start();
        return g_camera;
    } catch (const std::exception& e) {
        std::snprintf(error, errorCapacity, "%s", e.what());
        return nullptr;
    }
}

void shimClose(void* handle)
{
    auto* camera = static_cast<Vendor::Camera*>(handle);
    camera->Stop();
    Vendor::Camera::Release(camera);
    g_camera = nullptr;
}

int shimSetInt(void* handle, const char* key, int value)
{
    auto* camera = static_cast<Vendor::Camera*>(handle);
    return camera->SetProperty(key, value) ? 0 : -1;  // 按厂商 API 调整
}

int shimGrab(void* handle, unsigned char* data, int capacity,
             int* width, int* height, int* channels, int timeoutMs)
{
    auto* camera = static_cast<Vendor::Camera*>(handle);
    Vendor::Frame frame;
    if (!camera->WaitFrame(&frame, timeoutMs)) return 1;
    const int size = frame.width * frame.height * frame.channels;
    if (size > capacity) return -2;
    std::memcpy(data, frame.data, size);               // 或直接从 SDK buffer 拷
    *width = frame.width; *height = frame.height; *channels = frame.channels;
    return 0;
}
// setDouble / getInt / version 同理
} // namespace

namespace spike {
void registerVendorSdkShim()
{
    VendorSdkApi api;
    api.version = shimVersion;
    api.open = shimOpen;
    api.close = shimClose;
    api.setInt = shimSetInt;
    api.setDouble = shimSetDouble;
    api.getInt = shimGetInt;
    api.grab = shimGrab;
    registerVendorSdkApi(api);       // 注册后 UI 出现「相机厂商 C++ SDK」后端
}
} // namespace spike
```

若 SDK 输出 10/12/16 位，请在 shim 内完成位深转换（例如线性缩放到 8 位），并在 `source` 中记录；标定工具只把 8 位预览用于几何角点检测，不把它当辐射量。Spike 原始脉冲若要保留，仍走 `importCapture` 的 NPY/DAT 通道。

### 2. 打开编译开关

```powershell
cmake -S qt -B qt/build `
  -DCMAKE_PREFIX_PATH="<Qt6>" `
  -DSPIKE_USE_VENDOR_SHIM=ON `
  -DSPIKE_VENDOR_SHIM_DIR="E:/code/camera_calibration/camera_sdk" `
  -DSPIKE_VENDOR_SDK_INCLUDE_DIR="<厂商头文件目录>" `
  -DSPIKE_VENDOR_SDK_LIBRARY="<厂商 .lib / 导入库>"
cmake --build qt/build --config Release
```

`main.cpp` 在 `#ifdef SPIKE_USE_VENDOR_SHIM` 下 include `VendorSdkShim.h` 并在创建窗口前调用 `registerVendorSdkShim()`；`CMakeLists.txt` 会把 `VendorSdkShim.cpp` 编进 GUI 目标，并把 `SPIKE_VENDOR_SDK_LIBRARY` 链接到 `spike_core`。

如果厂商 SDK 有多个库或需要特殊编译选项，直接在 `CMakeLists.txt` 的 `target_link_libraries(spike_core ...)` 追加即可，不需要改动算法或界面代码。

### 3. 启动并使用

界面「05 设备连接与控制」：

- 后端选「相机厂商 C++ SDK」，填序列号 / 设备地址、最大帧尺寸与通道数，点「连接设备」。
- 「开始预览」按 100 ms 间隔拉流；`frameReady(QImage)` 只做显示。
- 「下发采集参数」把 `exposure_us`、`gain`、`trigger_mode` 推给 `setInt`。
- 填写采集序列 ID 与用途后点「采集当前帧并加入工程」，帧会走 `Workbench::importFiles`：按当前工程配置做旋转/镜像、靶标检测、质量与坐标域校验，然后进入「01 采集与检查」。

连接状态会同步到 `Workbench::state()` 的 `capabilities.hardware_connected`。**连接硬件不等于实机精度验收**：`evidence` 仍由界面选择，导出包 `hardware_certified` 恒为 `false`。

## 不接 SDK 也能开发

不选 `SPIKE_USE_VENDOR_SHIM` 时，设备页只有「离线文件回放（无需 SDK）」：选择一个目录，按顺序回放 PNG/JPEG/TIFF/NPY/DAT 帧，预览与「采集为观测」按钮同样可用。`examples/VendorSdkShim.cpp` 是可编译的占位实现，故意让 `open()` 返回带说明的失败信息，避免误以为已接上真实设备。

## 联调检查清单

1. `grab` 返回的 `width/height/channels` 与 `capacity` 一致；连续预览 5 分钟无内存增长。
2. `open` 失败时 `error` 有可读原因；拔线 / 重插后 `close` 能释放句柄。
3. 停稳标定板后再采集，避免运动模糊；每个姿态使用独立 `sequence_id`，训练与验证序列不得交叉。
4. 采集帧的角点数、视场覆盖与「未检出」项在「01 采集与检查」逐张确认。
5. 用 `spike_selftest --observations ... --ground-truth ...` 先确认算法侧数值，再对比实机结果。
6. 修改镜头、对焦、分光器或图像方向后，几何配置变更会清空旧观测——这是刻意保留的失效规则。

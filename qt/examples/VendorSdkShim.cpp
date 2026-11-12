#include "VendorSdkShim.h"

#include "device/CameraDeviceFactory.h"

#include <cstring>

// ---------------------------------------------------------------------------
// TEMPLATE ONLY. Replace the bodies below with real SDK calls, for example:
//   #include "VendorCameraApi.h"
//   static Vendor::Camera* g_camera = nullptr;
//   open(): g_camera = Vendor::Camera::Open(connection); ...
//   grab(): copy one frame from the SDK buffer into `data`, set width/height/
//           channels, return 0 on success, 1 on timeout, negative on failure.
// The SDK stays entirely inside this translation unit.
// ---------------------------------------------------------------------------
namespace {

void copyError(char* error, int capacity, const char* text)
{
    if (!error || capacity <= 0) return;
    std::strncpy(error, text, static_cast<size_t>(capacity - 1));
    error[capacity - 1] = '\0';
}

const char* shimVersion() { return "vendor-shim-template/1.0"; }

void* shimOpen(const char*, char* error, int errorCapacity)
{
    copyError(error, errorCapacity,
              "请把 examples/VendorSdkShim.cpp 中的 TODO 替换为厂商 SDK 实现");
    return nullptr;
}

void shimClose(void*) {}

int shimSetInt(void*, const char*, int) { return 0; }

int shimSetDouble(void*, const char*, double) { return 0; }

int shimGetInt(void*, const char*, int* value)
{
    if (value) *value = 0;
    return 0;
}

int shimGrab(void*, unsigned char*, int, int*, int*, int*, int)
{
    return -1;                 // no frame: template only
}

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
    registerVendorSdkApi(api);
}

} // namespace spike

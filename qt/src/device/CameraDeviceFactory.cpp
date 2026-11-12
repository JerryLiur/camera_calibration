#include "CameraDeviceFactory.h"

#include "FileCameraDevice.h"

namespace spike {
namespace {

VendorSdkApi g_vendorApi;
bool g_vendorRegistered = false;

} // namespace

void registerVendorSdkApi(const VendorSdkApi& api)
{
    g_vendorApi = api;
    g_vendorRegistered = api.open != nullptr && api.grab != nullptr;
}

bool hasVendorSdk() { return g_vendorRegistered; }

QStringList availableBackends()
{
    QStringList backends{QStringLiteral("file")};
    if (g_vendorRegistered) backends.append(QStringLiteral("vendor-sdk"));
    return backends;
}

CameraDevicePtr createCameraDevice(const QString& backend)
{
    if (backend == QStringLiteral("vendor-sdk") && g_vendorRegistered) {
        return std::make_unique<VendorSdkCameraDevice>(g_vendorApi);
    }
    return std::make_unique<FileCameraDevice>();
}

} // namespace spike

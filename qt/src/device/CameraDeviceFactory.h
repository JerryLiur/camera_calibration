#pragma once

#include "ICameraDevice.h"
#include "VendorSdkCameraDevice.h"

#include <QStringList>

namespace spike {

// Called once from main() (or from the integrator's SDK shim) before the UI starts.
void registerVendorSdkApi(const VendorSdkApi& api);
bool hasVendorSdk();
QStringList availableBackends();
CameraDevicePtr createCameraDevice(const QString& backend);

} // namespace spike

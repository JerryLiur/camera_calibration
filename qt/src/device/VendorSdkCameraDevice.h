#pragma once

#include "ICameraDevice.h"

#include <QByteArray>
#include <QVariantMap>

namespace spike {

// ABI-neutral bridge to a vendor C++ SDK. The integrator writes a tiny shim in
// their own translation unit that fills this table with functions calling the
// actual SDK classes, then calls registerVendorSdkApi() at startup.
// No vendor header is needed to compile spike_core.
struct VendorSdkApi {
    const char* (*version)() = nullptr;
    // connection: serial number / device path / camera id, UTF-8.
    void* (*open)(const char* connection, char* error, int errorCapacity) = nullptr;
    void (*close)(void* handle) = nullptr;
    int (*setInt)(void* handle, const char* key, int value) = nullptr;
    int (*setDouble)(void* handle, const char* key, double value) = nullptr;
    int (*getInt)(void* handle, const char* key, int* value) = nullptr;
    // Copies one 8-bit frame into data. 0 = ok, 1 = timeout, < 0 = error.
    int (*grab)(void* handle, unsigned char* data, int capacity, int* width,
                int* height, int* channels, int timeoutMs) = nullptr;
};

class VendorSdkCameraDevice : public ICameraDevice {
public:
    explicit VendorSdkCameraDevice(VendorSdkApi api);
    ~VendorSdkCameraDevice() override;
    VendorSdkCameraDevice(const VendorSdkCameraDevice&) = delete;
    VendorSdkCameraDevice& operator=(const VendorSdkCameraDevice&) = delete;

    QString backend() const override { return QStringLiteral("vendor-sdk"); }
    bool open(const QVariantMap& parameters, QString* error) override;
    void close() override;
    bool isOpen() const override { return m_handle != nullptr; }
    QVariantMap parameters() const override { return m_parameters; }
    bool setParameter(const QString& key, const QVariant& value, QString* error) override;
    bool grab(DeviceFrame& frame, int timeoutMs, QString* error) override;

private:
    VendorSdkApi m_api;
    void* m_handle = nullptr;
    QVariantMap m_parameters;
    QByteArray m_buffer;
    quint64 m_frameIndex = 0;
};

} // namespace spike

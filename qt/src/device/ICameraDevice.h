#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <opencv2/core.hpp>

#include <memory>

namespace spike {

struct DeviceFrame {
    cv::Mat image;                 // CV_8UC1 / CV_8UC3 / CV_8UC4, BGR channel order for colour
    quint64 frameIndex = 0;
    quint64 timestampNs = 0;
    QVariantMap metadata;
};

// Device boundary for the camera vendor's C++ SDK. Implement this interface once
// per SDK; the rest of the application never sees a vendor header or type.
// All methods except open()/close() are called from the acquisition worker thread.
class ICameraDevice {
public:
    virtual ~ICameraDevice() = default;

    virtual QString backend() const = 0;
    virtual bool open(const QVariantMap& parameters, QString* error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QVariantMap parameters() const = 0;
    virtual bool setParameter(const QString& key, const QVariant& value, QString* error) = 0;
    virtual bool grab(DeviceFrame& frame, int timeoutMs, QString* error) = 0;
};

using CameraDevicePtr = std::unique_ptr<ICameraDevice>;

} // namespace spike

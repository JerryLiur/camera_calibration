#pragma once

#include "ICameraDevice.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QThread>
#include <QVariantMap>

class QTimer;

namespace spike {

// Lives in the acquisition thread. The UI only ever talks to DeviceController,
// so a blocking vendor SDK grab() can never freeze the Widgets event loop.
class DeviceWorker : public QObject {
    Q_OBJECT
public:
    explicit DeviceWorker(QObject* parent = nullptr);

public slots:
    void openDevice(const QString& backend, const QVariantMap& parameters);
    void closeDevice();
    void setParameter(const QString& key, const QVariant& value);
    void startStream(int intervalMs);
    void stopStream();
    void grabStill();

signals:
    void opened(bool ok, const QString& message, const QVariantMap& parameters);
    void closed(const QString& message);
    void frameReady(const QImage& image, quint64 index, const QString& source);
    void stillCaptured(const QByteArray& png, const QString& name, const QString& message);
    void errorOccurred(const QString& message);
    void parametersChanged(const QVariantMap& parameters);

private:
    bool grabInternal(bool still, QString* error);

    CameraDevicePtr m_device;
    QTimer* m_timer = nullptr;
};

class DeviceController : public QObject {
    Q_OBJECT
public:
    explicit DeviceController(QObject* parent = nullptr);
    ~DeviceController() override;

    void connectDevice(const QString& backend, const QVariantMap& parameters);
    void disconnectDevice();
    void setParameter(const QString& key, const QVariant& value);
    void startPreview(int intervalMs = 33);
    void stopPreview();
    void captureStill();

signals:
    void connectionChanged(bool connected, const QString& message);
    void frameReady(const QImage& image, quint64 index, const QString& source);
    void stillCaptured(const QByteArray& png, const QString& name, const QString& message);
    void errorOccurred(const QString& message);
    void parametersChanged(const QVariantMap& parameters);

private:
    QThread m_thread;
    DeviceWorker* m_worker = nullptr;
};

} // namespace spike

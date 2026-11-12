#pragma once

#include "ICameraDevice.h"

#include <QJsonObject>
#include <QStringList>

namespace spike {

// Offline device: replays captured frames (images / Spike NPY / packed DAT) so the
// acquisition UI can be exercised before the vendor SDK is wired in.
class FileCameraDevice : public ICameraDevice {
public:
    QString backend() const override { return QStringLiteral("file"); }
    bool open(const QVariantMap& parameters, QString* error) override;
    void close() override;
    bool isOpen() const override { return !m_files.isEmpty(); }
    QVariantMap parameters() const override;
    bool setParameter(const QString& key, const QVariant& value, QString* error) override;
    bool grab(DeviceFrame& frame, int timeoutMs, QString* error) override;

private:
    QStringList m_files;
    QJsonObject m_decodeOptions;
    QVariantMap m_parameters;
    int m_index = 0;
    bool m_loop = false;
    quint64 m_frameIndex = 0;
};

} // namespace spike

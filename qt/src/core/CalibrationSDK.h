#pragma once

#include "Model.h"

namespace spike {

// sdk.SpikeCalibrationSDK equivalent for downstream C++ clients that already own
// Spike pixels. Pixels must use the same rotation/flip convention as the project.
class CalibrationSDK {
public:
    explicit CalibrationSDK(const QString& bundlePath);

    QJsonObject query(const QJsonArray& points,
                      const QJsonValue& encoderDeg = QJsonValue()) const;
    QJsonObject config() const { return m_project.value(QStringLiteral("config")).toObject(); }
    QJsonArray evidence() const { return m_evidence; }
    QJsonObject project() const { return m_project; }

private:
    QJsonObject m_project;
    QJsonArray m_evidence;
};

} // namespace spike

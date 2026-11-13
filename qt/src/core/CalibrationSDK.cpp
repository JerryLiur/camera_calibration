#include "CalibrationSDK.h"

#include <QFile>

namespace spike {

CalibrationSDK::CalibrationSDK(const QString& bundlePath)
{
    QFile file(bundlePath);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取标定包"));
    const QJsonObject bundle = requireObject(strictLoads(file.readAll()), QStringLiteral("标定包"));
    require(bundle.value(QStringLiteral("format")).toString() == QStringLiteral("spike-calibration-bundle")
                && bundle.value(QStringLiteral("format_version")).toInt() == 1,
            QStringLiteral("不支持的标定包格式"));
    m_project = requireObject(bundle.value(QStringLiteral("project")), QStringLiteral("标定包工程"));
    cameraFromModel(requireObject(m_project.value(QStringLiteral("model")), QStringLiteral("标定包内参模型")));
    const auto config = requireObject(m_project.value(QStringLiteral("config")), QStringLiteral("标定包配置"));
    validateConfig(config);
    require(m_project.value(QStringLiteral("model")).toObject()
                .value(QStringLiteral("config_fingerprint")).toString() == configFingerprint(config),
            QStringLiteral("内参模型与工程光学/方向配置不一致，请重新标定"));
    m_evidence = bundle.value(QStringLiteral("evidence")).toArray();
}

QJsonObject CalibrationSDK::query(const QJsonArray& points, const QJsonValue& encoderDeg) const
{
    return queryRays(m_project.value(QStringLiteral("model")).toObject(), points,
                     m_project.value(QStringLiteral("turntable")), encoderDeg);
}

} // namespace spike

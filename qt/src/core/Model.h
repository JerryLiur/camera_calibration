#pragma once

#include "Common.h"

#include <opencv2/core.hpp>

#include <vector>

namespace spike {

inline bool isNone(const QJsonValue& value) { return value.isNull() || value.isUndefined(); }

struct TargetConfig {
    QString kind = QStringLiteral("chessboard");
    int cols = 9;
    int rows = 6;
    double squareLengthM = 0.025;
};

struct OrientationConfig {
    int rotationDeg = 0;
    bool flipX = false;
    bool flipY = false;
};

struct OpticsConfig {
    bool sharedLens = true;
    QString splitRatio = QStringLiteral("1:1");
    QString lensId;
    QString focusNote;
    QString splitterNote;
};

struct AcceptanceConfig {
    std::optional<double> intrinsicsP95Px;
    std::optional<double> turntableP95Deg;
};

struct Config {
    TargetConfig target;
    OrientationConfig orientation;
    OpticsConfig optics;
    AcceptanceConfig acceptance;
    QString oper;
};

struct Capture {
    QJsonObject raw;                 // full JSON record, including any extra fields
    QString id;
    QString name;
    QString sequenceId;
    QString split;
    QString evidence;
    QString coordinateDomain = QStringLiteral("transformed_pixels");
    bool enabled = true;
    bool found = true;
    int width = 0;
    int height = 0;
    std::vector<cv::Point2d> imagePoints;
    std::vector<cv::Point3d> objectPoints;
    QJsonObject quality;
    QJsonObject source;
    QString previewBase64;
    QStringList reasons;
};

struct Camera {
    cv::Matx33d K = cv::Matx33d::eye();
    std::vector<double> distortion;
    int width = 0;
    int height = 0;
};

QJsonObject defaultConfigJson();
QJsonObject configToJson(const Config& config);
Config validateConfig(const QJsonObject& input);
QJsonObject targetConfigJson(const Config& config);
QJsonObject orientationJson(const Config& config);

QJsonObject captureToJson(const Capture& capture);
Capture validateCapture(const QJsonObject& input);
QJsonArray capturesToJson(const std::vector<Capture>& captures);
std::vector<Capture> capturesFromJson(const QJsonArray& captures);
std::vector<Capture> activeCaptures(const std::vector<Capture>& captures);
QString captureSignature(const Capture& capture);

QJsonArray point2Array(const std::vector<cv::Point2d>& points);
QJsonArray point3Array(const std::vector<cv::Point3d>& points);
std::vector<cv::Point2d> point2Vector(const QJsonValue& value, const QString& name);
std::vector<cv::Point3d> point3Vector(const QJsonValue& value, const QString& name);

Camera cameraFromModel(const QJsonObject& model);
QJsonObject queryRays(const QJsonObject& model, const QJsonArray& points,
                      const QJsonValue& turntable = QJsonValue(),
                      const QJsonValue& encoderDeg = QJsonValue());

} // namespace spike

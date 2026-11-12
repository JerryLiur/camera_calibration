#pragma once

#include "Model.h"

#include <opencv2/core.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace spike {

// calibration_system/targets.py equivalents.
cv::Mat toGray8(const cv::Mat& image, double blackLevel = 0.0, double whiteLevel = 255.0);

QJsonObject imageQuality(const cv::Mat& image,
                         const std::vector<cv::Point2d>* points = nullptr,
                         double blackLevel = 0.0, double whiteLevel = 255.0);

struct TargetResult {
    bool found = false;
    std::vector<cv::Point2d> points;
    std::vector<cv::Point3d> objectPoints;
    std::vector<int> ids;
    QJsonObject quality;
    QStringList reasons;
    QJsonObject record;
};

TargetResult detectTarget(const cv::Mat& gray8, const QJsonObject& target);

QJsonObject captureGuidance(const std::vector<std::vector<cv::Point2d>>& views,
                            cv::Size imageSize, cv::Size grid = cv::Size(6, 4));

struct SpikeRepresentation {
    cv::Mat image;                 // CV_64F, one reconstructed calibration image
    QString method;
    QString unit;
    std::optional<double> periodS;
    QJsonObject quality;
    QString semantics;
};

SpikeRepresentation spikeRepresentation(const std::vector<uchar>& binary, int ticks, int height,
                                        int width, const QString& method,
                                        std::optional<double> periodS);

cv::Mat applyOrientation(const cv::Mat& image, const OrientationConfig& orientation);

std::pair<std::vector<cv::Point2d>, cv::Size> orientPoints(
    const std::vector<cv::Point2d>& points, cv::Size size,
    const OrientationConfig& orientation);

} // namespace spike

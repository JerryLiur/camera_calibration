#include "Targets.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace spike {
namespace {

constexpr double kPi = 3.14159265358979323846;

double ptp(const std::vector<double>& values)
{
    if (values.empty()) return 0.0;
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    return *high - *low;
}

std::vector<double> unwrap(const std::vector<double>& angles)
{
    std::vector<double> result;
    result.reserve(angles.size());
    double shift = 0.0;
    for (size_t i = 0; i < angles.size(); ++i) {
        if (i > 0) {
            double delta = angles[i] - angles[i - 1];
            delta -= std::round(delta / (2.0 * kPi)) * 2.0 * kPi;
            shift += delta - (angles[i] - angles[i - 1]);
        }
        result.push_back(angles[i] + shift);
    }
    return result;
}

} // namespace

cv::Mat toGray8(const cv::Mat& image, double blackLevel, double whiteLevel)
{
    require(!image.empty() && (image.channels() == 1 || image.channels() == 3 || image.channels() == 4),
            QStringLiteral("image must be a nonempty 2D or RGB image, up to 25 megapixels"));
    require(static_cast<double>(image.rows) * image.cols <= 25000000.0,
            QStringLiteral("image must be a nonempty 2D or RGB image, up to 25 megapixels"));
    require(std::isfinite(blackLevel) && std::isfinite(whiteLevel) && whiteLevel > blackLevel,
            QStringLiteral("finite pixels and ordered black/white levels are required"));

    cv::Mat data;
    image.convertTo(data, CV_64F);
    cv::Mat normalized = (data - blackLevel) / (whiteLevel - blackLevel) * 255.0;
    cv::Mat clipped = normalized.clone();
    for (int r = 0; r < clipped.rows; ++r) {
        double* row = clipped.ptr<double>(r);
        for (int c = 0; c < clipped.cols * clipped.channels(); ++c) {
            row[c] = std::clamp(row[c], 0.0, 255.0);
        }
    }
    cv::Mat gray8;
    clipped.convertTo(gray8, CV_8U);
    if (gray8.channels() == 4) {
        cv::cvtColor(gray8, gray8, cv::COLOR_RGBA2GRAY);
    } else if (gray8.channels() == 3) {
        cv::cvtColor(gray8, gray8, cv::COLOR_RGB2GRAY);
    }
    return gray8;
}

QJsonObject imageQuality(const cv::Mat& image, const std::vector<cv::Point2d>* points,
                         double blackLevel, double whiteLevel)
{
    cv::Mat data;
    image.convertTo(data, CV_64F);
    const cv::Mat gray = toGray8(data, blackLevel, whiteLevel);

    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(laplacian, mean, stddev);
    cv::meanStdDev(gray, mean, stddev);
    const double contrastStd = stddev[0] / 255.0;
    const double meanNormalized = mean[0] / 255.0;

    double saturatedCount = 0.0, blackCount = 0.0;
    for (int r = 0; r < data.rows; ++r) {
        const double* row = data.ptr<double>(r);
        for (int c = 0; c < data.cols * data.channels(); ++c) {
            if (row[c] >= whiteLevel) saturatedCount += 1.0;
            if (row[c] <= blackLevel) blackCount += 1.0;
        }
    }
    const double total = static_cast<double>(data.total() * data.channels());
    const double saturated = saturatedCount / total;
    const double black = blackCount / total;

    QJsonObject metrics;
    metrics.insert(QStringLiteral("sharpness_laplacian_variance"),
                   [&]() {
                       cv::Scalar m, s;
                       cv::meanStdDev(laplacian, m, s);
                       return s[0] * s[0];
                   }());
    metrics.insert(QStringLiteral("contrast_std"), contrastStd);
    metrics.insert(QStringLiteral("mean_normalized"), meanNormalized);
    metrics.insert(QStringLiteral("saturated_fraction"), saturated);
    metrics.insert(QStringLiteral("black_fraction"), black);
    metrics.insert(QStringLiteral("image_size"),
                   QJsonArray{QJsonValue(gray.cols), QJsonValue(gray.rows)});
    metrics.insert(QStringLiteral("quality_thresholds"),
                   QStringLiteral("not applied; configure them for the actual sensor/mode"));
    if (points) {
        metrics.insert(QStringLiteral("feature_coverage"),
                       captureGuidance({*points}, cv::Size(gray.cols, gray.rows)));
    }

    QJsonArray suggestions;
    if (contrastStd < 0.03) suggestions.append(QStringLiteral("低对比度：检查照明、阈值和重建窗长"));
    if (saturated > 0.05) suggestions.append(QStringLiteral("部分像素剪切：降低光源亮度或增益"));

    QJsonObject result;
    result.insert(QStringLiteral("metrics"), metrics);
    result.insert(QStringLiteral("suggestions"), suggestions);
    result.insert(QStringLiteral("heuristic_note"),
                  QStringLiteral("建议采用可配置经验阈值，不是客户验收判定"));
    return result;
}

TargetResult detectTarget(const cv::Mat& gray8, const QJsonObject& targetConfig)
{
    const QString kind = targetConfig.value(QStringLiteral("kind")).toString(QStringLiteral("chessboard"));
    const int cols = targetConfig.value(QStringLiteral("cols")).toInt(9);
    const int rows = targetConfig.value(QStringLiteral("rows")).toInt(6);
    const double length = targetConfig.value(QStringLiteral("square_length_m")).toDouble(0.025);
    require((kind == QStringLiteral("chessboard") || kind == QStringLiteral("circles")
             || kind == QStringLiteral("asymmetric_circles"))
                && cols >= 2 && cols <= 100 && rows >= 2 && rows <= 100
                && std::isfinite(length) && length > 0.0,
            QStringLiteral("unsupported calibration target"));

    const double blackLevel = targetConfig.value(QStringLiteral("black_level")).toDouble(0.0);
    const double whiteLevel = targetConfig.value(QStringLiteral("white_level")).toDouble(255.0);
    const cv::Mat gray = toGray8(gray8, blackLevel, whiteLevel);

    std::vector<cv::Point2f> corners;
    std::vector<cv::Point3d> objects;
    bool found = false;
    if (kind == QStringLiteral("chessboard")) {
        found = cv::findChessboardCornersSB(gray, cv::Size(cols, rows), corners,
                                            cv::CALIB_CB_NORMALIZE_IMAGE);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                objects.emplace_back(x * length, y * length, 0.0);
            }
        }
    } else {
        const int flags = kind == QStringLiteral("asymmetric_circles")
                              ? cv::CALIB_CB_ASYMMETRIC_GRID : cv::CALIB_CB_SYMMETRIC_GRID;
        cv::SimpleBlobDetector::Params params;
        params.minArea = 8.0f;
        params.maxArea = static_cast<float>(std::max(100.0, static_cast<double>(gray.total()) / 20.0));
        params.filterByArea = true;
        const cv::Ptr<cv::SimpleBlobDetector> detector = cv::SimpleBlobDetector::create(params);
        try {
            found = cv::findCirclesGrid(gray, cv::Size(cols, rows), corners, flags, detector);
        } catch (const cv::Exception&) {
            found = false;
        }
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const double px = (kind == QStringLiteral("asymmetric_circles")
                                       ? 2.0 * x + (y % 2) : x) * length;
                objects.emplace_back(px, y * length, 0.0);
            }
        }
    }

    TargetResult result;
    result.record.insert(QStringLiteral("target"), targetConfig);
    if (!found) {
        result.quality = imageQuality(gray8);
        result.reasons << QStringLiteral("target_not_detected; check target dimensions, contrast, focus and visibility");
        result.record.insert(QStringLiteral("found"), false);
        result.record.insert(QStringLiteral("points"), QJsonArray());
        result.record.insert(QStringLiteral("object_points"), QJsonArray());
        result.record.insert(QStringLiteral("ids"), QJsonArray());
        result.record.insert(QStringLiteral("quality"), result.quality);
        QJsonArray reasons;
        for (const QString& reason : result.reasons) reasons.append(reason);
        result.record.insert(QStringLiteral("reasons"), reasons);
        return result;
    }

    for (const auto& p : corners) result.points.emplace_back(p.x, p.y);
    result.objectPoints = objects;
    for (int i = 0; i < static_cast<int>(result.points.size()); ++i) result.ids.push_back(i);
    result.quality = imageQuality(gray8, &result.points);

    result.record.insert(QStringLiteral("found"), true);
    result.record.insert(QStringLiteral("points"), point2Array(result.points));
    result.record.insert(QStringLiteral("object_points"), point3Array(result.objectPoints));
    QJsonArray ids;
    for (int id : result.ids) ids.append(id);
    result.record.insert(QStringLiteral("ids"), ids);
    result.record.insert(QStringLiteral("quality"), result.quality);
    result.record.insert(QStringLiteral("localization"),
                         QStringLiteral("OpenCV target localization; no calibrated point covariance available"));
    result.record.insert(QStringLiteral("coordinate_domain"), QStringLiteral("raw_pixels"));
    result.record.insert(QStringLiteral("manual_edits"), QJsonArray());
    result.record.insert(QStringLiteral("reasons"), QJsonArray());
    return result;
}

QJsonObject captureGuidance(const std::vector<std::vector<cv::Point2d>>& views,
                            cv::Size imageSize, cv::Size grid)
{
    const double width = imageSize.width;
    const double height = imageSize.height;
    require(width > 0 && height > 0, QStringLiteral("image dimensions must be positive"));
    const int cellsX = grid.width, cellsY = grid.height;
    std::vector<std::vector<int>> occupancy(static_cast<size_t>(cellsY),
                                            std::vector<int>(static_cast<size_t>(cellsX), 0));
    QJsonArray centroids, areas;
    std::vector<double> areaValues, angles;

    for (const auto& view : views) {
        require(view.size() >= 4, QStringLiteral("each view must contain at least four finite points"));
        double sumX = 0.0, sumY = 0.0;
        std::vector<cv::Point2f> valid;
        std::vector<cv::Point2f> hullInput;
        for (const auto& p : view) {
            require(std::isfinite(p.x) && std::isfinite(p.y),
                    QStringLiteral("each view must contain at least four finite points"));
            sumX += p.x;
            sumY += p.y;
            hullInput.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
            if (p.x >= 0.0 && p.x < width && p.y >= 0.0 && p.y < height) {
                valid.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
                const int cx = std::clamp(static_cast<int>(std::floor(p.x / width * cellsX)), 0, cellsX - 1);
                const int cy = std::clamp(static_cast<int>(std::floor(p.y / height * cellsY)), 0, cellsY - 1);
                occupancy[static_cast<size_t>(cy)][static_cast<size_t>(cx)] = 1;
            }
        }
        centroids.append(QJsonArray{QJsonValue(sumX / view.size() / width),
                                    QJsonValue(sumY / view.size() / height)});
        std::vector<cv::Point2f> hull;
        cv::convexHull(hullInput, hull);
        const double area = static_cast<double>(cv::contourArea(hull)) / (width * height);
        areaValues.push_back(area);
        areas.append(area);
        const cv::Point2d vector = view.back() - view.front();
        angles.push_back(std::atan2(vector.y, vector.x));
    }

    QJsonArray missing;
    int filled = 0;
    QJsonArray occupied;
    for (int y = 0; y < cellsY; ++y) {
        QJsonArray row;
        for (int x = 0; x < cellsX; ++x) {
            const int value = occupancy[static_cast<size_t>(y)][static_cast<size_t>(x)];
            row.append(value);
            filled += value;
            if (!value) missing.append(QJsonArray{QJsonValue(x), QJsonValue(y)});
        }
        occupied.append(row);
    }

    QJsonArray recommendations;
    if (!missing.isEmpty()) recommendations.append(QStringLiteral("补采尚未覆盖的视场区域，特别是四角与边缘"));
    if (views.size() < 6) recommendations.append(QStringLiteral("内参求解至少补充至六组独立姿态，并改变倾角与距离"));
    if (views.size() > 1 && ptp(areaValues) < 0.05)
        recommendations.append(QStringLiteral("靶标成像尺寸变化较小，增加不同距离的观测"));
    if (views.size() > 1 && ptp(unwrap(angles)) < 0.2)
        recommendations.append(QStringLiteral("靶标旋转变化较小，增加不同倾角；二维指标不能代替三维姿态可观测性"));

    QJsonObject result;
    result.insert(QStringLiteral("grid"), QJsonArray{QJsonValue(cellsX), QJsonValue(cellsY)});
    result.insert(QStringLiteral("occupied_cells"), occupied);
    result.insert(QStringLiteral("coverage_fraction"),
                  static_cast<double>(filled) / (cellsX * cellsY));
    result.insert(QStringLiteral("missing_cells"), missing);
    result.insert(QStringLiteral("view_count"), static_cast<qint64>(views.size()));
    result.insert(QStringLiteral("centroids_normalized"), centroids);
    result.insert(QStringLiteral("board_area_fractions"), areas);
    result.insert(QStringLiteral("recommendations"), recommendations);
    return result;
}

SpikeRepresentation spikeRepresentation(const std::vector<uchar>& binary, int ticks, int height,
                                        int width, const QString& method,
                                        std::optional<double> periodS)
{
    require(ticks > 0 && height > 0 && width > 0,
            QStringLiteral("raw Spike must be a nonempty binary [T,H,W] array"));
    require(method == QStringLiteral("count") || method == QStringLiteral("rate")
                || method == QStringLiteral("interval"),
            QStringLiteral("representation must be count, rate or interval"));
    const size_t pixels = static_cast<size_t>(height) * width;
    require(binary.size() == static_cast<size_t>(ticks) * pixels,
            QStringLiteral("raw Spike must be a nonempty binary [T,H,W] array"));
    if (periodS) {
        require(std::isfinite(*periodS) && *periodS > 0.0,
                QStringLiteral("sample period must be finite and positive"));
    }
    if (method == QStringLiteral("rate")) {
        require(periodS.has_value(), QStringLiteral("count rate requires a verified sample period"));
    }

    cv::Mat image(height, width, CV_64F, cv::Scalar(0.0));
    size_t allZero = 0, alwaysFiring = 0;
    for (size_t p = 0; p < pixels; ++p) {
        int count = 0;
        int first = ticks;
        int last = -1;
        for (int t = 0; t < ticks; ++t) {
            if (binary[static_cast<size_t>(t) * pixels + p] != 0) {
                ++count;
                first = std::min(first, t);
                last = std::max(last, t);
            }
        }
        if (count == 0) ++allZero;
        if (count == ticks) ++alwaysFiring;
        double value = 0.0;
        if (method == QStringLiteral("count")) {
            value = static_cast<double>(count);
        } else if (method == QStringLiteral("rate")) {
            value = static_cast<double>(count) / (static_cast<double>(ticks) * *periodS);
        } else {
            const double duration = static_cast<double>(std::max(last - first, 1));
            value = count >= 2 ? (count - 1) / duration : 0.0;
            if (periodS) value /= *periodS;
        }
        image.at<double>(static_cast<int>(p / width), static_cast<int>(p % width)) = value;
    }

    SpikeRepresentation result;
    result.image = image;
    result.method = method;
    result.periodS = periodS;
    result.semantics = QStringLiteral("window statistic, not an instantaneous image");
    if (method == QStringLiteral("count")) {
        result.unit = QStringLiteral("spikes_per_window");
    } else if (method == QStringLiteral("rate")) {
        result.unit = QStringLiteral("Hz");
    } else {
        result.unit = periodS ? QStringLiteral("Hz") : QStringLiteral("inverse_ticks");
    }
    result.quality.insert(QStringLiteral("all_zero_fraction"),
                          static_cast<double>(allZero) / static_cast<double>(pixels));
    result.quality.insert(QStringLiteral("always_firing_fraction"),
                          static_cast<double>(alwaysFiring) / static_cast<double>(pixels));
    return result;
}

cv::Mat applyOrientation(const cv::Mat& image, const OrientationConfig& orientation)
{
    cv::Mat result = image.clone();
    for (int i = 0; i < orientation.rotationDeg / 90; ++i) {
        cv::rotate(result, result, cv::ROTATE_90_CLOCKWISE);
    }
    if (orientation.flipX) cv::flip(result, result, 1);
    if (orientation.flipY) cv::flip(result, result, 0);
    return result;
}

std::pair<std::vector<cv::Point2d>, cv::Size> orientPoints(
    const std::vector<cv::Point2d>& points, cv::Size size,
    const OrientationConfig& orientation)
{
    std::vector<cv::Point2d> xy = points;
    double width = size.width;
    double height = size.height;
    for (int i = 0; i < orientation.rotationDeg / 90; ++i) {
        std::vector<cv::Point2d> rotated;
        rotated.reserve(xy.size());
        for (const auto& p : xy) rotated.emplace_back(height - 1.0 - p.y, p.x);
        xy = std::move(rotated);
        std::swap(width, height);
    }
    if (orientation.flipX) {
        for (auto& p : xy) p.x = width - 1.0 - p.x;
    }
    if (orientation.flipY) {
        for (auto& p : xy) p.y = height - 1.0 - p.y;
    }
    return {xy, cv::Size(static_cast<int>(width), static_cast<int>(height))};
}

} // namespace spike

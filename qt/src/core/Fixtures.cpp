#include "Fixtures.h"

#include "Importers.h"
#include "Targets.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <random>

namespace spike {
namespace {

cv::Point2d projectAt(const cv::Mat& projected, int index)
{
    if (projected.depth() == CV_64F) {
        const cv::Vec2d point = projected.at<cv::Vec2d>(index, 0);
        return {point[0], point[1]};
    }
    const cv::Vec2f point = projected.at<cv::Vec2f>(index, 0);
    return {point[0], point[1]};
}

} // namespace

std::vector<Capture> demoCaptures(const Config& config)
{
    require(config.target.kind == QStringLiteral("chessboard"),
            QStringLiteral("内置演示采用棋盘格，请先将靶标类型设置为 chessboard"));
    const int cols = config.target.cols;
    const int rows = config.target.rows;
    const double spacing = config.target.squareLengthM;
    std::vector<cv::Point3d> objectPoints;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) objectPoints.emplace_back(x * spacing, y * spacing, 0.0);
    }
    const cv::Mat objectMat(static_cast<int>(objectPoints.size()), 1, CV_64FC3,
                            objectPoints.data());
    const double extent = std::max((cols + 1) * spacing, (rows + 1) * spacing);
    // Explicit element assignment instead of the `Mat_ <<` comma initializer,
    // which OpenCV 5 deprecated.
    cv::Mat K = cv::Mat::eye(3, 3, CV_64F);
    K.at<double>(0, 0) = 620.0;
    K.at<double>(0, 2) = 319.0;
    K.at<double>(1, 1) = 610.0;
    K.at<double>(1, 2) = 239.0;
    cv::Mat distortion = cv::Mat::zeros(1, 5, CV_64F);
    const double distortionCoefficients[5] = {-0.055, 0.012, 0.0007, -0.0005, 0.0};
    for (int i = 0; i < 5; ++i) distortion.at<double>(0, i) = distortionCoefficients[i];

    std::mt19937_64 rng(20261104);
    std::normal_distribution<double> noise(0.0, 0.035);
    std::vector<Capture> captures;
    for (int i = 0; i < 16; ++i) {
        const cv::Vec3d rotation((i % 2 ? -1.0 : 1.0) * (0.16 + 0.055 * (i % 5)),
                                 (i % 3 ? -1.0 : 1.0) * (0.13 + 0.06 * (i % 4)),
                                 (i % 5 - 2) * 0.09);
        cv::Vec3d translation(-(cols - 1) * spacing / 2.0 + (i % 4 - 1.5) * extent * 0.26,
                              -(rows - 1) * spacing / 2.0 + (i % 3 - 1) * extent * 0.18,
                              extent * (1.55 + 0.18 * (i % 4)));
        const cv::Mat rvec(3, 1, CV_64F, const_cast<double*>(&rotation[0]));
        const cv::Mat tvec(3, 1, CV_64F, &translation[0]);
        auto projectBoard = [&]() {
            cv::Mat projected;
            cv::projectPoints(objectMat, rvec, tvec, K, distortion, projected);
            return projected;
        };
        cv::Mat projected = projectBoard();
        for (int attempt = 0; attempt < 10; ++attempt) {
            bool inside = true;
            for (int p = 0; p < static_cast<int>(objectPoints.size()); ++p) {
                const cv::Point2d point = projectAt(projected, p);
                if (point.x < 8.0 || point.y < 8.0 || point.x > 631.0 || point.y > 471.0) {
                    inside = false;
                    break;
                }
            }
            if (inside) break;
            translation[2] *= 1.09;
            projected = projectBoard();
        }
        std::vector<cv::Point2d> imagePoints;
        for (int p = 0; p < static_cast<int>(objectPoints.size()); ++p) {
            cv::Point2d point = projectAt(projected, p);
            point.x += noise(rng);
            point.y += noise(rng);
            imagePoints.push_back(point);
        }

        cv::Mat image(480, 640, CV_8U, cv::Scalar(175));
        for (int y = -1; y < rows; ++y) {
            for (int x = -1; x < cols; ++x) {
                const std::vector<cv::Point3d> square = {
                    {x * spacing, y * spacing, 0.0},
                    {(x + 1) * spacing, y * spacing, 0.0},
                    {(x + 1) * spacing, (y + 1) * spacing, 0.0},
                    {x * spacing, (y + 1) * spacing, 0.0}};
                const cv::Mat squareMat(static_cast<int>(square.size()), 1, CV_64FC3,
                                        const_cast<cv::Point3d*>(square.data()));
                cv::Mat polygon;
                cv::projectPoints(squareMat, rvec, tvec, K, distortion, polygon);
                std::vector<cv::Point> polygonPoints;
                for (int p = 0; p < static_cast<int>(square.size()); ++p) {
                    const cv::Point2d point = projectAt(polygon, p);
                    polygonPoints.emplace_back(cvRound(point.x), cvRound(point.y));
                }
                cv::fillConvexPoly(image, polygonPoints,
                                   cv::Scalar((x + y) % 2 ? 235.0 : 25.0), cv::LINE_AA);
            }
        }
        cv::putText(image, "SYNTHETIC / GEOMETRY DEMO", cv::Point(12, 21),
                    cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(55), 1, cv::LINE_AA);

        const cv::Mat orientedImage = applyOrientation(image, config.orientation);
        const auto [orientedPoints, orientedSize] =
            orientPoints(imagePoints, cv::Size(640, 480), config.orientation);
        const QString split = i < 12 ? QStringLiteral("train") : QStringLiteral("test");

        QJsonObject capture;
        capture.insert(QStringLiteral("id"), QStringLiteral("demo-%1").arg(i, 2, 10, QLatin1Char('0')));
        capture.insert(QStringLiteral("name"),
                       QStringLiteral("synthetic_pose_%1.png").arg(i + 1, 2, 10, QLatin1Char('0')));
        capture.insert(QStringLiteral("sequence_id"),
                       QStringLiteral("synthetic-%1-%2").arg(split).arg(i, 2, 10, QLatin1Char('0')));
        capture.insert(QStringLiteral("split"), split);
        capture.insert(QStringLiteral("evidence"), QStringLiteral("synthetic"));
        capture.insert(QStringLiteral("enabled"), true);
        capture.insert(QStringLiteral("found"), true);
        capture.insert(QStringLiteral("image_size"),
                       QJsonArray{QJsonValue(orientedSize.width), QJsonValue(orientedSize.height)});
        capture.insert(QStringLiteral("object_points"), point3Array(objectPoints));
        capture.insert(QStringLiteral("image_points"), point2Array(orientedPoints));
        capture.insert(QStringLiteral("coordinate_domain"), QStringLiteral("transformed_pixels"));
        capture.insert(QStringLiteral("quality"), imageQuality(orientedImage, &orientedPoints));
        capture.insert(QStringLiteral("preview_base64"), encodePreview(orientedImage));
        QJsonObject source;
        source.insert(QStringLiteral("kind"), QStringLiteral("synthetic_projected_board"));
        source.insert(QStringLiteral("seed"), 20261104);
        source.insert(QStringLiteral("orientation"), orientationJson(config));
        source.insert(QStringLiteral("note"),
                      QStringLiteral("独立数值多姿态夹具，角点含0.035px合成扰动；不是实际相机或实际工作距离"));
        capture.insert(QStringLiteral("source"), source);
        captures.push_back(validateCapture(capture));
    }
    return captures;
}

} // namespace spike

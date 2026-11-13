#include "Calibration.h"

#include "Targets.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace spike {
namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

double ptpRange(const std::vector<double>& values)
{
    if (values.empty()) return 0.0;
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    return *high - *low;
}

cv::Point2d projectedAt(const cv::Mat& projected, int index)
{
    if (projected.depth() == CV_64F) {
        const cv::Vec2d point = projected.at<cv::Vec2d>(index, 0);
        return {point[0], point[1]};
    }
    const cv::Vec2f point = projected.at<cv::Vec2f>(index, 0);
    return {point[0], point[1]};
}

struct IntrinsicFit {
    cv::Mat K;
    cv::Mat distortion;
    double rms = 0.0;
    std::vector<double> perViewRms;
    std::vector<double> intrinsicStd;
    double normalSpanDeg = 0.0;
};

IntrinsicFit fitCameraIntrinsics(
    const std::vector<std::vector<cv::Point3d>>& objects,
    const std::vector<std::vector<cv::Point2d>>& images, cv::Size size)
{
    require(objects.size() == images.size() && objects.size() >= 6,
            QStringLiteral("at least six matching calibration-board views are required"));
    std::vector<std::vector<cv::Point3f>> objectFloat;
    std::vector<std::vector<cv::Point2f>> imageFloat;
    for (size_t i = 0; i < objects.size(); ++i) {
        require(objects[i].size() == images[i].size() && objects[i].size() >= 6,
                QStringLiteral("object_points must be finite planar [N,3] board points with z=0"));
        double meanX = 0.0, meanY = 0.0;
        for (const auto& p : objects[i]) {
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
                        && std::abs(p.z) < 1e-12,
                    QStringLiteral("object_points must be finite planar [N,3] board points with z=0"));
            meanX += p.x;
            meanY += p.y;
        }
        meanX /= static_cast<double>(objects[i].size());
        meanY /= static_cast<double>(objects[i].size());
        cv::Mat centered(static_cast<int>(objects[i].size()), 2, CV_64F);
        for (size_t j = 0; j < objects[i].size(); ++j) {
            centered.at<double>(static_cast<int>(j), 0) = objects[i][j].x - meanX;
            centered.at<double>(static_cast<int>(j), 1) = objects[i][j].y - meanY;
        }
        cv::Mat w;
        cv::SVD::compute(centered, w, cv::SVD::NO_UV);
        require(w.rows >= 2 && w.at<double>(0) > 0.0
                    && w.at<double>(1) / w.at<double>(0) >= 1e-6,
                QStringLiteral("calibration board must have noncollinear points"));
        std::vector<cv::Point3f> objectRow;
        std::vector<cv::Point2f> imageRow;
        for (size_t j = 0; j < objects[i].size(); ++j) {
            objectRow.emplace_back(static_cast<float>(objects[i][j].x),
                                   static_cast<float>(objects[i][j].y),
                                   static_cast<float>(objects[i][j].z));
            require(std::isfinite(images[i][j].x) && std::isfinite(images[i][j].y),
                    QStringLiteral("image_points must have finite shape [N,2], N >= 6"));
            imageRow.emplace_back(static_cast<float>(images[i][j].x),
                                  static_cast<float>(images[i][j].y));
        }
        objectFloat.push_back(objectRow);
        imageFloat.push_back(imageRow);
    }

    IntrinsicFit result;
    result.K = cv::Mat::eye(3, 3, CV_64F);
    std::vector<cv::Mat> rvecs, tvecs;
    cv::Mat perViewErrors;
    const cv::TermCriteria criteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER,
                                    100, 1e-9);
#if defined(CV_VERSION_MAJOR) && CV_VERSION_MAJOR >= 4
    cv::Mat intrinsicStd, extrinsicStd;
    // The extended C++ API is an overload; calibrateCameraExtended is its Python name.
    result.rms = cv::calibrateCamera(
        objectFloat, imageFloat, size, result.K, result.distortion, rvecs, tvecs,
        intrinsicStd, extrinsicStd, perViewErrors, 0, criteria);
    for (int i = 0; i < intrinsicStd.rows; ++i) {
        for (int j = 0; j < intrinsicStd.cols; ++j) {
            result.intrinsicStd.push_back(intrinsicStd.at<double>(i, j));
        }
    }
#else
    result.rms = cv::calibrateCamera(objectFloat, imageFloat, size, result.K,
                                     result.distortion, rvecs, tvecs, 0, criteria);
    perViewErrors = cv::Mat(static_cast<int>(objectFloat.size()), 1, CV_64F, cv::Scalar(0.0));
    for (size_t i = 0; i < objectFloat.size(); ++i) {
        std::vector<cv::Point2f> projected;
        cv::projectPoints(objectFloat[i], rvecs[i], tvecs[i], result.K,
                          result.distortion, projected);
        double sum = 0.0;
        for (size_t j = 0; j < projected.size(); ++j) {
            const double dx = projected[j].x - imageFloat[i][j].x;
            const double dy = projected[j].y - imageFloat[i][j].y;
            sum += dx * dx + dy * dy;
        }
        perViewErrors.at<double>(static_cast<int>(i), 0) =
            std::sqrt(sum / static_cast<double>(projected.size()));
    }
#endif
    std::vector<cv::Vec3d> normals;
    for (const cv::Mat& rvec : rvecs) {
        cv::Mat rotation;
        cv::Rodrigues(rvec, rotation);
        normals.emplace_back(rotation.at<double>(0, 2), rotation.at<double>(1, 2),
                             rotation.at<double>(2, 2));
    }
    for (size_t i = 0; i < normals.size(); ++i) {
        for (size_t j = i + 1; j < normals.size(); ++j) {
            const double angle = std::acos(std::clamp(normals[i].dot(normals[j]), -1.0, 1.0)) * kRadToDeg;
            result.normalSpanDeg = std::max(result.normalSpanDeg, angle);
        }
    }
    for (int i = 0; i < perViewErrors.rows; ++i) {
        result.perViewRms.push_back(perViewErrors.at<double>(i, 0));
    }
    return result;
}

std::vector<double> distortionVector(const cv::Mat& distortion)
{
    cv::Mat flat = distortion.reshape(1, 1);
    std::vector<double> values;
    values.reserve(static_cast<size_t>(flat.cols));
    for (int i = 0; i < flat.cols; ++i) values.push_back(flat.at<double>(0, i));
    return values;
}

QString imageSignature(const Capture& capture)
{
    QByteArray bytes;
    for (const auto& p : capture.imagePoints) {
        quint64 ux = 0, uy = 0;
        std::memcpy(&ux, &p.x, sizeof(ux));
        std::memcpy(&uy, &p.y, sizeof(uy));
        for (int i = 0; i < 8; ++i) {
            bytes.append(static_cast<char>((ux >> (8 * i)) & 0xffu));
        }
        for (int i = 0; i < 8; ++i) {
            bytes.append(static_cast<char>((uy >> (8 * i)) & 0xffu));
        }
    }
    return sha256Hex(bytes);
}

QJsonArray matrixToArray(const cv::Mat& matrix)
{
    QJsonArray rows;
    for (int r = 0; r < matrix.rows; ++r) {
        QJsonArray row;
        for (int c = 0; c < matrix.cols; ++c) row.append(matrix.at<double>(r, c));
        rows.append(row);
    }
    return rows;
}

QJsonArray vectorToArray(const std::vector<double>& values)
{
    QJsonArray array;
    for (double value : values) {
        array.append(std::isfinite(value) ? QJsonValue(value) : QJsonValue());
    }
    return array;
}

QJsonArray stringSetToArray(const std::set<QString>& values)
{
    QJsonArray array;
    for (const QString& value : values) array.append(value);
    return array;
}

} // namespace

QJsonObject solveIntrinsics(const std::vector<Capture>& captures, const Config& config)
{
    const std::vector<Capture> active = activeCaptures(captures);
    std::vector<Capture> views;
    for (const Capture& capture : active) {
        if (capture.split == QStringLiteral("train")) views.push_back(capture);
    }
    require(views.size() >= 6,
            QStringLiteral("内参求解至少需要六个有效训练姿态，请改变标定板倾角、位置和距离"));
    const cv::Size size(views.front().width, views.front().height);
    for (const Capture& capture : active) {
        require(cv::Size(capture.width, capture.height) == size,
                QStringLiteral("同一工程的有效观测必须使用相同图像尺寸"));
    }
    std::set<QString> signatures;
    for (const Capture& capture : views) signatures.insert(imageSignature(capture));
    require(signatures.size() >= 6, QStringLiteral("训练姿态重复，至少需要六个不同姿态"));

    // Zhang-style homography constraints: reject poses that are only translated/frontal.
    cv::Mat constraints(static_cast<int>(views.size() * 2), 6, CV_64F);
    for (size_t v = 0; v < views.size(); ++v) {
        const Capture& capture = views[v];
        std::vector<cv::Point2d> objectXY, imageXY;
        const double scale = static_cast<double>(std::max(size.width, size.height));
        for (size_t i = 0; i < capture.imagePoints.size(); ++i) {
            objectXY.emplace_back(capture.objectPoints[i].x, capture.objectPoints[i].y);
            imageXY.emplace_back((capture.imagePoints[i].x - size.width / 2.0) / scale,
                                 (capture.imagePoints[i].y - size.height / 2.0) / scale);
        }
        const cv::Mat objectMat(static_cast<int>(objectXY.size()), 1, CV_64FC2, objectXY.data());
        const cv::Mat imageMat(static_cast<int>(imageXY.size()), 1, CV_64FC2, imageXY.data());
        const cv::Mat H = cv::findHomography(objectMat, imageMat, 0);
        require(!H.empty(), QStringLiteral("靶标投影退化，请补采不同倾角"));
        auto vec = [&H](int i, int j) {
            return cv::Vec6d(H.at<double>(0, i) * H.at<double>(0, j),
                             H.at<double>(0, i) * H.at<double>(1, j)
                                 + H.at<double>(1, i) * H.at<double>(0, j),
                             H.at<double>(1, i) * H.at<double>(1, j),
                             H.at<double>(2, i) * H.at<double>(0, j)
                                 + H.at<double>(0, i) * H.at<double>(2, j),
                             H.at<double>(2, i) * H.at<double>(1, j)
                                 + H.at<double>(1, i) * H.at<double>(2, j),
                             H.at<double>(2, i) * H.at<double>(2, j));
        };
        const cv::Vec6d first = vec(0, 1);
        const cv::Vec6d second = vec(0, 0) - vec(1, 1);
        for (int k = 0; k < 2; ++k) {
            const cv::Vec6d row = k == 0 ? first : second;
            const double norm = std::sqrt(row.dot(row));
            const double divisor = std::max(norm, 1e-15);
            for (int c = 0; c < 6; ++c) {
                constraints.at<double>(static_cast<int>(v * 2 + k), c) = row[c] / divisor;
            }
        }
    }
    cv::Mat singular;
    cv::SVD::compute(constraints, singular, cv::SVD::NO_UV);
    constexpr double minimumConstraintRatio = 1e-3;
    require(singular.rows >= 2 && singular.at<double>(0) > 0.0
                && singular.at<double>(singular.rows - 2) / singular.at<double>(0) >= minimumConstraintRatio,
            QStringLiteral("姿态约束退化：只平移或正对相机不足以标定，请增加不同倾角"));
    std::vector<double> singularValues;
    for (int i = 0; i < singular.rows; ++i) singularValues.push_back(singular.at<double>(i));

    std::vector<std::vector<cv::Point3d>> objects;
    std::vector<std::vector<cv::Point2d>> images;
    std::vector<std::vector<cv::Point2d>> coverageViews;
    for (const Capture& capture : views) {
        objects.push_back(capture.objectPoints);
        images.push_back(capture.imagePoints);
        coverageViews.push_back(capture.imagePoints);
    }
    const IntrinsicFit fitted = fitCameraIntrinsics(objects, images, size);
    const std::vector<double> distortion = distortionVector(fitted.distortion);
    require(std::isfinite(fitted.rms) && !distortion.empty(),
            QStringLiteral("内参求解产生无效参数，请检查角点、板尺寸及姿态"));
    for (int r = 0; r < fitted.K.rows; ++r) {
        for (int c = 0; c < fitted.K.cols; ++c) {
            require(std::isfinite(fitted.K.at<double>(r, c)),
                    QStringLiteral("内参求解产生无效参数，请检查角点、板尺寸及姿态"));
        }
    }
    for (double value : distortion) {
        require(std::isfinite(value), QStringLiteral("内参求解产生无效参数，请检查角点、板尺寸及姿态"));
    }
    for (double value : fitted.intrinsicStd) {
        require(std::isfinite(value), QStringLiteral("姿态约束退化：内参不确定度不可估计，请增加不同倾角与视场覆盖后重采"));
    }
    // Conservative acquisition-quality gate, not an angular accuracy certificate.
    // Noise/distortion can mask degeneracy in the homography-only rank check.
    require(fitted.normalSpanDeg >= 3.0,
            QStringLiteral("姿态约束退化：训练板法向跨度仅 %1°，至少需要 3°，请增加不同倾角后重采")
                .arg(fitted.normalSpanDeg, 0, 'f', 3));

    const QJsonObject coverage = captureGuidance(coverageViews, size);
    QJsonArray warnings;
    for (const QJsonValue& item : coverage.value(QStringLiteral("recommendations")).toArray()) {
        warnings.append(item);
    }
    if (!(fitted.K.at<double>(0, 2) >= 0 && fitted.K.at<double>(0, 2) < size.width
          && fitted.K.at<double>(1, 2) >= 0 && fitted.K.at<double>(1, 2) < size.height)) {
        warnings.append(QStringLiteral("主点位于图像范围外，建议检查姿态分布及角点后重采"));
    }
    std::set<QString> evidence;
    for (const Capture& capture : views) evidence.insert(capture.evidence);
    if (evidence != std::set<QString>{QStringLiteral("hardware")}) {
        warnings.append(QStringLiteral("包含合成或公开数据；结果不能作为本设备实机精度验收"));
    }

    QJsonArray perView;
    for (size_t i = 0; i < views.size(); ++i) {
        QJsonObject item;
        item.insert(QStringLiteral("capture_id"), views[i].id);
        item.insert(QStringLiteral("name"), views[i].name);
        item.insert(QStringLiteral("rms_px"), fitted.perViewRms[i]);
        perView.append(item);
    }
    QJsonObject diagnostics;
    diagnostics.insert(QStringLiteral("fit_rms_px"), fitted.rms);
    diagnostics.insert(QStringLiteral("per_view_rms_px"), vectorToArray(fitted.perViewRms));
    diagnostics.insert(QStringLiteral("per_view"), perView);
    diagnostics.insert(QStringLiteral("intrinsic_std"),
                       fitted.intrinsicStd.empty() ? QJsonValue()
                                                   : QJsonValue(vectorToArray(fitted.intrinsicStd)));
    diagnostics.insert(QStringLiteral("constraint_singular_values"), vectorToArray(singularValues));
    diagnostics.insert(QStringLiteral("constraint_rank_ratio"), singular.at<double>(singular.rows - 2) / singular.at<double>(0));
    diagnostics.insert(QStringLiteral("minimum_constraint_rank_ratio"), minimumConstraintRatio);
    diagnostics.insert(QStringLiteral("board_normal_span_deg"), fitted.normalSpanDeg);
    diagnostics.insert(QStringLiteral("minimum_board_normal_span_deg"), 3.0);
    diagnostics.insert(QStringLiteral("coverage"), coverage);

    std::set<QString> trainSequences, trainHashes, trainSignatures;
    QJsonArray trainCaptureIds;
    for (const Capture& capture : views) {
        trainSequences.insert(capture.sequenceId);
        trainCaptureIds.append(capture.id);
        trainSignatures.insert(captureSignature(capture));
        const QString hash = capture.source.value(QStringLiteral("sha256")).toString();
        if (!hash.isEmpty()) trainHashes.insert(hash);
    }

    QJsonObject model;
    model.insert(QStringLiteral("sensor"), QStringLiteral("spike"));
    model.insert(QStringLiteral("model_type"), QStringLiteral("pinhole_brown_5"));
    model.insert(QStringLiteral("K"), matrixToArray(fitted.K));
    model.insert(QStringLiteral("distortion"), vectorToArray(distortion));
    model.insert(QStringLiteral("image_size"),
                 QJsonArray{QJsonValue(size.width), QJsonValue(size.height)});
    model.insert(QStringLiteral("coordinate_domain"), QStringLiteral("transformed_pixels"));
    model.insert(QStringLiteral("orientation"), orientationJson(config));
    model.insert(QStringLiteral("evidence"), stringSetToArray(evidence));
    model.insert(QStringLiteral("train_sequence_ids"), stringSetToArray(trainSequences));
    model.insert(QStringLiteral("train_capture_ids"), trainCaptureIds);
    model.insert(QStringLiteral("train_source_hashes"), stringSetToArray(trainHashes));
    model.insert(QStringLiteral("train_observation_signatures"), stringSetToArray(trainSignatures));
    model.insert(QStringLiteral("diagnostics"), diagnostics);
    model.insert(QStringLiteral("warnings"), warnings);
    model.insert(QStringLiteral("scope"),
                 QStringLiteral("完整镜头＋1:1分光器＋Spike支路；不包含RGB配准及双机时序"));
    return model;
}

QJsonObject validateIntrinsics(const QJsonObject& model,
                               const std::vector<Capture>& captures,
                               const Config& config)
{
    const Camera camera = cameraFromModel(model);
    const std::vector<Capture> active = activeCaptures(captures);
    std::vector<Capture> views;
    for (const Capture& capture : active) {
        if (capture.split == QStringLiteral("test")) views.push_back(capture);
    }
    require(!views.empty(), QStringLiteral("请导入与训练采集序列独立的验证姿态"));

    std::set<QString> trainSequences, trainIds, trainHashes, trainSignatures;
    for (const QJsonValue& value : model.value(QStringLiteral("train_sequence_ids")).toArray()) {
        trainSequences.insert(value.toString());
    }
    for (const QJsonValue& value : model.value(QStringLiteral("train_capture_ids")).toArray()) {
        trainIds.insert(value.toString());
    }
    for (const QJsonValue& value : model.value(QStringLiteral("train_source_hashes")).toArray()) {
        trainHashes.insert(value.toString().toLower());
    }
    for (const QJsonValue& value : model.value(QStringLiteral("train_observation_signatures")).toArray()) {
        trainSignatures.insert(value.toString());
    }

    const cv::Mat K = cv::Mat(camera.K).clone();
    std::vector<double> distortionValues = camera.distortion;
    cv::Mat d(1, static_cast<int>(distortionValues.size()), CV_64F, distortionValues.data());
    QJsonArray perView;
    std::vector<double> errors;
    std::set<QString> testSequences;
    std::set<QString> testEvidence;
    bool allHardware = model.value(QStringLiteral("evidence")).toArray().size() == 1
        && model.value(QStringLiteral("evidence")).toArray().at(0).toString() == QStringLiteral("hardware");
    for (const Capture& capture : views) {
        const QString hash = capture.source.value(QStringLiteral("sha256")).toString();
        require(!trainSequences.count(capture.sequenceId)
                    && !trainIds.count(capture.id)
                    && (hash.isEmpty() || !trainHashes.count(hash))
                    && !trainSignatures.count(captureSignature(capture)),
                QStringLiteral("验证数据与冻结模型的训练数据重叠，必须重新独立采集"));
        require(capture.width == camera.width && capture.height == camera.height,
                QStringLiteral("验证图像尺寸与冻结模型不同"));
        cv::Mat objectMat(static_cast<int>(capture.objectPoints.size()), 1, CV_64FC3,
                          const_cast<cv::Point3d*>(capture.objectPoints.data()));
        cv::Mat imageMat(static_cast<int>(capture.imagePoints.size()), 1, CV_64FC2,
                         const_cast<cv::Point2d*>(capture.imagePoints.data()));
        cv::Mat rvec, tvec;
        require(cv::solvePnP(objectMat, imageMat, K, d, rvec, tvec),
                QStringLiteral("独立验证姿态估计失败"));
        cv::Mat projected;
        cv::projectPoints(objectMat, rvec, tvec, K, d, projected);
        std::vector<double> residual;
        for (size_t i = 0; i < capture.imagePoints.size(); ++i) {
            const cv::Point2d point = projectedAt(projected, static_cast<int>(i));
            residual.push_back(std::hypot(point.x - capture.imagePoints[i].x,
                                          point.y - capture.imagePoints[i].y));
            errors.push_back(residual.back());
        }
        QJsonObject record;
        record.insert(QStringLiteral("capture_id"), capture.id);
        record.insert(QStringLiteral("sequence_id"), capture.sequenceId);
        record.insert(QStringLiteral("name"), capture.name);
        record.insert(QStringLiteral("stats"), errorStats(residual, QStringLiteral("px")));
        perView.append(record);
        testSequences.insert(capture.sequenceId);
        testEvidence.insert(capture.evidence);
        if (capture.evidence != QStringLiteral("hardware")) allHardware = false;
    }

    const QJsonObject stats = errorStats(errors, QStringLiteral("px"));
    double worst = 0.0;
    for (const QJsonValue& value : perView) {
        worst = std::max(worst, value.toObject().value(QStringLiteral("stats"))
                                    .toObject().value(QStringLiteral("p95_px")).toDouble());
    }
    const std::optional<double> gate = config.acceptance.intrinsicsP95Px;
    QString status = QStringLiteral("measured");
    if (gate) status = worst <= *gate ? QStringLiteral("passed") : QStringLiteral("failed");

    QJsonObject result;
    result.insert(QStringLiteral("status"), status);
    result.insert(QStringLiteral("stats"), stats);
    result.insert(QStringLiteral("p95_px"), stats.value(QStringLiteral("p95_px")));
    result.insert(QStringLiteral("worst_view_p95_px"), worst);
    result.insert(QStringLiteral("threshold_px"), gate ? QJsonValue(*gate) : QJsonValue());
    result.insert(QStringLiteral("per_view"), perView);
    result.insert(QStringLiteral("test_sequence_ids"), stringSetToArray(testSequences));
    result.insert(QStringLiteral("evidence"), stringSetToArray(testEvidence));
    result.insert(QStringLiteral("evidence_all_hardware"), allHardware);
    result.insert(QStringLiteral("hardware_qualified"), false);
    result.insert(QStringLiteral("note"),
                  QStringLiteral("冻结K/畸变，仅拟合每个验证板的位姿；重投影误差不等同绝对测量精度。未设置阈值时仅报告测量结果。"));
    return result;
}

QJsonObject fitTurntable(const QJsonObject& model, const QJsonArray& observations,
                         const QJsonObject& referenceInput, const Config& config)
{
    QJsonObject reference = referenceInput;
    const QJsonValue signValue = reference.value(QStringLiteral("sign"));
    const int sign = isNone(signValue) ? 1 : requireInt(signValue, QStringLiteral("转台正方向 sign"));
    require(sign == 1 || sign == -1, QStringLiteral("转台正方向 sign 必须为 1 或 -1"));
    reference.insert(QStringLiteral("sign"), sign);
    const QString mode = reference.value(QStringLiteral("mode")).toString();
    double referenceAngle = 0.0;
    std::vector<double> targetPosition, cameraOffset;
    if (mode == QStringLiteral("far_field")) {
        require(reference.value(QStringLiteral("far_field_confirmed")).toBool(),
                QStringLiteral("远场近似必须明确确认；近距离目标请使用已知几何模式"));
        const QJsonValue angle = reference.value(QStringLiteral("reference_angle_deg"));
        referenceAngle = isNone(angle) ? 0.0 : requireNumber(angle, QStringLiteral("参考方向"));
        reference.insert(QStringLiteral("reference_angle_deg"), referenceAngle);
    } else if (mode == QStringLiteral("finite_distance")) {
        targetPosition = numberVector(reference.value(QStringLiteral("target_position_m")),
                                      QStringLiteral("目标相对转台轴位置"), 3);
        cameraOffset = numberVector(reference.value(QStringLiteral("camera_offset_m")),
                                    QStringLiteral("相机入口瞳相对转轴位置"), 3);
        reference.insert(QStringLiteral("target_position_m"), vectorToArray(targetPosition));
        reference.insert(QStringLiteral("camera_offset_m"), vectorToArray(cameraOffset));
        const double distance = std::sqrt(
            (targetPosition[0] - cameraOffset[0]) * (targetPosition[0] - cameraOffset[0])
            + (targetPosition[1] - cameraOffset[1]) * (targetPosition[1] - cameraOffset[1])
            + (targetPosition[2] - cameraOffset[2]) * (targetPosition[2] - cameraOffset[2]));
        require(distance > 1e-8, QStringLiteral("目标不能位于相机入口瞳位置"));
    } else {
        fail(QStringLiteral("转台参考 mode 必须为 far_field 或 finite_distance"));
    }

    auto expectedAzimuth = [&](const std::vector<double>& encoders) {
        std::vector<double> result;
        result.reserve(encoders.size());
        for (double encoder : encoders) {
            const double q = sign * encoder;
            if (mode == QStringLiteral("far_field")) {
                result.push_back(wrapDegrees(referenceAngle - q));
            } else {
                const double radians = q * kDegToRad;
                const double x = std::cos(radians) * targetPosition[0]
                    - std::sin(radians) * targetPosition[2] - cameraOffset[0];
                const double z = std::sin(radians) * targetPosition[0]
                    + std::cos(radians) * targetPosition[2] - cameraOffset[2];
                require(z > 0.0,
                        QStringLiteral("已知几何在部分转角下位于相机支路背面；请检查坐标及范围"));
                result.push_back(std::atan2(x, z) * kRadToDeg);
            }
        }
        return result;
    };

    require(observations.size() >= 4,
            QStringLiteral("转台至少需要三条训练观测和一条独立验证观测"));
    QJsonArray checked;
    std::vector<double> observedAzimuth, encoderTrain, encoderAll;
    for (const QJsonValue& item : observations) {
        const QJsonObject observation = requireObject(item, QStringLiteral("转台观测"));
        const QString split = observation.value(QStringLiteral("split")).toString();
        require(split == QStringLiteral("train") || split == QStringLiteral("test"),
                QStringLiteral("转台观测需要 split=train/test 与有效 sequence_id"));
        const QString sequence = requireString(observation.value(QStringLiteral("sequence_id")),
                                               QStringLiteral("转台观测 sequence_id"));
        require(!sequence.trimmed().isEmpty(), QStringLiteral("转台观测需要 split=train/test 与有效 sequence_id"));
        const double encoder = requireNumber(observation.value(QStringLiteral("encoder_deg")),
                                             QStringLiteral("转台角度"));
        const auto pixel = numberVector(observation.value(QStringLiteral("pixel")),
                                        QStringLiteral("转台特征像点"), 2);
        const QJsonObject rays = queryRays(
            model, QJsonArray{QJsonArray{QJsonValue(pixel[0]), QJsonValue(pixel[1])}});
        const double azimuth = rays.value(QStringLiteral("points")).toArray().at(0)
                                   .toObject().value(QStringLiteral("azimuth_deg")).toDouble();
        QJsonObject record = observation;
        record.insert(QStringLiteral("encoder_deg"), encoder);
        record.insert(QStringLiteral("pixel"), QJsonArray{QJsonValue(pixel[0]), QJsonValue(pixel[1])});
        record.insert(QStringLiteral("observed_azimuth_deg"), azimuth);
        checked.append(record);
        observedAzimuth.push_back(azimuth);
        encoderAll.push_back(encoder);
        if (split == QStringLiteral("train")) encoderTrain.push_back(encoder);
    }

    std::vector<double> trainObserved, testObserved;
    std::set<QString> trainSequences, testSequences;
    std::vector<double> trainEncoders;
    for (const QJsonValue& item : checked) {
        const QJsonObject record = item.toObject();
        const bool train = record.value(QStringLiteral("split")).toString() == QStringLiteral("train");
        const double azimuth = record.value(QStringLiteral("observed_azimuth_deg")).toDouble();
        const double encoder = record.value(QStringLiteral("encoder_deg")).toDouble();
        (train ? trainObserved : testObserved).push_back(azimuth);
        (train ? trainSequences : testSequences).insert(record.value(QStringLiteral("sequence_id")).toString());
        if (train) trainEncoders.push_back(encoder);
    }
    require(trainObserved.size() >= 3 && !testObserved.empty()
                && ptpRange(trainEncoders) >= 1.0,
            QStringLiteral("转台训练需至少三个观测且角度跨度至少1度，并保留独立验证"));
    for (const QString& sequence : trainSequences) {
        require(!testSequences.count(sequence), QStringLiteral("转台训练与验证必须使用独立采集序列"));
    }

    const std::vector<double> trainExpected = expectedAzimuth(trainEncoders);
    std::vector<double> offsets;
    double sine = 0.0, cosine = 0.0;
    for (size_t i = 0; i < trainExpected.size(); ++i) {
        const double offset = wrapDegrees(trainExpected[i] - trainObserved[i]);
        offsets.push_back(offset);
        sine += std::sin(offset * kDegToRad);
        cosine += std::cos(offset * kDegToRad);
    }
    sine /= static_cast<double>(offsets.size());
    cosine /= static_cast<double>(offsets.size());
    require(std::hypot(sine, cosine) >= 0.5,
            QStringLiteral("零位关系不一致，请检查转台正方向、标记点和单位"));
    const double zero = std::atan2(sine, cosine) * kRadToDeg;

    const std::vector<double> allExpected = expectedAzimuth(encoderAll);
    QJsonArray checkedOutput;
    std::vector<double> trainResiduals, testResiduals;
    for (int i = 0; i < static_cast<int>(checked.size()); ++i) {
        QJsonObject record = checked.at(i).toObject();
        const double predicted = wrapDegrees(allExpected[static_cast<size_t>(i)] - zero);
        const double observed = record.value(QStringLiteral("observed_azimuth_deg")).toDouble();
        const double residual = std::abs(wrapDegrees(observed - predicted));
        record.insert(QStringLiteral("predicted_azimuth_deg"), predicted);
        record.insert(QStringLiteral("residual_deg"), residual);
        checkedOutput.append(record);
        (record.value(QStringLiteral("split")).toString() == QStringLiteral("train")
             ? trainResiduals : testResiduals).push_back(residual);
    }
    const QJsonObject trainStats = errorStats(trainResiduals, QStringLiteral("deg"));
    const QJsonObject testStats = errorStats(testResiduals, QStringLiteral("deg"));
    const std::optional<double> gate = config.acceptance.turntableP95Deg;
    QString status = QStringLiteral("measured");
    if (gate) {
        status = testStats.value(QStringLiteral("p95_deg")).toDouble() <= *gate
                     ? QStringLiteral("passed") : QStringLiteral("failed");
    }
    const auto [low, high] = std::minmax_element(trainEncoders.begin(), trainEncoders.end());

    QJsonObject result;
    result.insert(QStringLiteral("model_type"), QStringLiteral("single_axis_yaw_zero"));
    result.insert(QStringLiteral("zero_offset_deg"), zero);
    result.insert(QStringLiteral("sign"), sign);
    result.insert(QStringLiteral("reference"), reference);
    result.insert(QStringLiteral("train_stats"), trainStats);
    result.insert(QStringLiteral("test_stats"), testStats);
    result.insert(QStringLiteral("test_p95_deg"), testStats.value(QStringLiteral("p95_deg")));
    result.insert(QStringLiteral("status"), status);
    result.insert(QStringLiteral("threshold_deg"), gate ? QJsonValue(*gate) : QJsonValue());
    result.insert(QStringLiteral("observations"), checkedOutput);
    result.insert(QStringLiteral("evidence"), model.value(QStringLiteral("evidence")));
    result.insert(QStringLiteral("hardware_qualified"), false);
    result.insert(QStringLiteral("encoder_range_deg"),
                  QJsonArray{QJsonValue(*low), QJsonValue(*high)});
    result.insert(QStringLiteral("coordinate_convention"),
                  QStringLiteral("零位时转台坐标X右、Y下、Z前；绕+Y旋转将Z指向+X；相机偏置为转动体坐标米。零位为相机相对该体的水平偏航。"));
    result.insert(QStringLiteral("limitations"),
                  QJsonArray{
                      QStringLiteral("仅估计一个转轴的水平零位；假定俯仰/滚转已机械对准，不估计完整六自由度或机构回差模型。"),
                      QStringLiteral("须使用停稳后的观测；动态编码器与Spike时间对齐另行验证。"),
                      QStringLiteral("远场结果仅在已确认近似条件下适用；近距离模式依赖所填目标位置及入口瞳偏置的测量质量。")});
    return result;
}

} // namespace spike

#include "Model.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <QRegularExpression>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>
#include <utility>

namespace spike {
namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

const QLatin1String kTarget("target");
const QLatin1String kOrientation("orientation");
const QLatin1String kOptics("optics");
const QLatin1String kAcceptance("acceptance");

QByteArray doubleBytesLE(double value)
{
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "IEEE-754 double required");
    std::memcpy(&bits, &value, sizeof(value));
    QByteArray out(8, Qt::Uninitialized);
    for (int i = 0; i < 8; ++i) out[i] = static_cast<char>((bits >> (8 * i)) & 0xffu);
    return out;
}

int matrixRank(const std::vector<cv::Point3d>& points)
{
    cv::Mat centered(static_cast<int>(points.size()), 2, CV_64F);
    double meanX = 0.0, meanY = 0.0;
    for (const auto& p : points) { meanX += p.x; meanY += p.y; }
    meanX /= static_cast<double>(points.size());
    meanY /= static_cast<double>(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        centered.at<double>(static_cast<int>(i), 0) = points[i].x - meanX;
        centered.at<double>(static_cast<int>(i), 1) = points[i].y - meanY;
    }
    cv::Mat w;
    cv::SVD::compute(centered, w, cv::SVD::NO_UV);
    if (w.rows == 0 || w.at<double>(0) <= 0.0) return 0;
    const double tolerance = std::max(centered.rows, centered.cols) * DBL_EPSILON * w.at<double>(0);
    int rank = 0;
    for (int i = 0; i < w.rows; ++i) {
        if (w.at<double>(i) > tolerance) ++rank;
    }
    return rank;
}

} // namespace

QJsonObject defaultConfigJson()
{
    QJsonObject target;
    target.insert(QStringLiteral("kind"), QStringLiteral("chessboard"));
    target.insert(QStringLiteral("cols"), 9);
    target.insert(QStringLiteral("rows"), 6);
    target.insert(QStringLiteral("square_length_m"), 0.025);
    QJsonObject orientation;
    orientation.insert(QStringLiteral("rotation_deg"), 0);
    orientation.insert(QStringLiteral("flip_x"), false);
    orientation.insert(QStringLiteral("flip_y"), false);
    QJsonObject optics;
    optics.insert(QStringLiteral("shared_lens"), true);
    optics.insert(QStringLiteral("split_ratio"), QStringLiteral("1:1"));
    optics.insert(QStringLiteral("lens_id"), QString());
    optics.insert(QStringLiteral("focus_note"), QString());
    optics.insert(QStringLiteral("splitter_note"), QString());
    QJsonObject acceptance;
    acceptance.insert(QStringLiteral("intrinsics_p95_px"), QJsonValue());
    acceptance.insert(QStringLiteral("turntable_p95_deg"), QJsonValue());
    QJsonObject config;
    config.insert(kTarget, target);
    config.insert(kOrientation, orientation);
    config.insert(kOptics, optics);
    config.insert(kAcceptance, acceptance);
    config.insert(QStringLiteral("operator"), QString());
    return config;
}

QJsonObject configToJson(const Config& c)
{
    QJsonObject config = defaultConfigJson();
    QJsonObject target = config.value(kTarget).toObject();
    target.insert(QStringLiteral("kind"), c.target.kind);
    target.insert(QStringLiteral("cols"), c.target.cols);
    target.insert(QStringLiteral("rows"), c.target.rows);
    target.insert(QStringLiteral("square_length_m"), c.target.squareLengthM);
    config.insert(kTarget, target);
    QJsonObject orientation = config.value(kOrientation).toObject();
    orientation.insert(QStringLiteral("rotation_deg"), c.orientation.rotationDeg);
    orientation.insert(QStringLiteral("flip_x"), c.orientation.flipX);
    orientation.insert(QStringLiteral("flip_y"), c.orientation.flipY);
    config.insert(kOrientation, orientation);
    QJsonObject optics = config.value(kOptics).toObject();
    optics.insert(QStringLiteral("shared_lens"), c.optics.sharedLens);
    optics.insert(QStringLiteral("split_ratio"), c.optics.splitRatio);
    optics.insert(QStringLiteral("lens_id"), c.optics.lensId);
    optics.insert(QStringLiteral("focus_note"), c.optics.focusNote);
    optics.insert(QStringLiteral("splitter_note"), c.optics.splitterNote);
    config.insert(kOptics, optics);
    QJsonObject acceptance;
    acceptance.insert(QStringLiteral("intrinsics_p95_px"),
                      c.acceptance.intrinsicsP95Px ? QJsonValue(*c.acceptance.intrinsicsP95Px)
                                                   : QJsonValue());
    acceptance.insert(QStringLiteral("turntable_p95_deg"),
                      c.acceptance.turntableP95Deg ? QJsonValue(*c.acceptance.turntableP95Deg)
                                                   : QJsonValue());
    config.insert(kAcceptance, acceptance);
    config.insert(QStringLiteral("operator"), c.oper);
    return config;
}

QJsonObject targetConfigJson(const Config& c)
{
    QJsonObject target;
    target.insert(QStringLiteral("kind"), c.target.kind);
    target.insert(QStringLiteral("cols"), c.target.cols);
    target.insert(QStringLiteral("rows"), c.target.rows);
    target.insert(QStringLiteral("square_length_m"), c.target.squareLengthM);
    return target;
}

QJsonObject orientationJson(const Config& c)
{
    QJsonObject orientation;
    orientation.insert(QStringLiteral("rotation_deg"), c.orientation.rotationDeg);
    orientation.insert(QStringLiteral("flip_x"), c.orientation.flipX);
    orientation.insert(QStringLiteral("flip_y"), c.orientation.flipY);
    return orientation;
}

Config validateConfig(const QJsonObject& input)
{
    QJsonObject merged = defaultConfigJson();
    const QStringList keys = input.keys();
    for (const QString& key : keys) {
        if (!merged.contains(key)) fail(QStringLiteral("未知配置字段：%1").arg(key));
        const QJsonValue patch = input.value(key);
        if (merged.value(key).isObject()) {
            const QJsonObject patchObject = requireObject(patch, key);
            QJsonObject base = merged.value(key).toObject();
            const QStringList subKeys = patchObject.keys();
            for (const QString& sub : subKeys) {
                if (!base.contains(sub)) fail(QStringLiteral("未知配置字段：%1.%2").arg(key, sub));
            }
            for (const QString& sub : subKeys) base.insert(sub, patchObject.value(sub));
            merged.insert(key, base);
        } else {
            merged.insert(key, patch);
        }
    }

    Config c;
    const QJsonObject target = merged.value(kTarget).toObject();
    c.target.kind = requireString(target.value(QStringLiteral("kind")), QStringLiteral("靶标类型"));
    require(c.target.kind == QStringLiteral("chessboard")
                || c.target.kind == QStringLiteral("circles")
                || c.target.kind == QStringLiteral("asymmetric_circles"),
            QStringLiteral("靶标支持棋盘格、圆点或非对称圆点阵列"));
    c.target.cols = requireInt(target.value(QStringLiteral("cols")), QStringLiteral("靶标列数"));
    c.target.rows = requireInt(target.value(QStringLiteral("rows")), QStringLiteral("靶标行数"));
    require(c.target.cols >= 2 && c.target.cols <= 100 && c.target.rows >= 2 && c.target.rows <= 100,
            QStringLiteral("靶标行列必须为 2～100 的整数（棋盘格填写内角点数）"));
    c.target.squareLengthM = requirePositiveNumber(target.value(QStringLiteral("square_length_m")),
                                                    QStringLiteral("靶标间距"));

    const QJsonObject orientation = merged.value(kOrientation).toObject();
    c.orientation.rotationDeg = requireInt(orientation.value(QStringLiteral("rotation_deg")),
                                           QStringLiteral("图像旋转角"));
    require(c.orientation.rotationDeg == 0 || c.orientation.rotationDeg == 90
                || c.orientation.rotationDeg == 180 || c.orientation.rotationDeg == 270,
            QStringLiteral("图像旋转角必须为 0、90、180 或 270 度（顺时针）"));
    c.orientation.flipX = requireBool(orientation.value(QStringLiteral("flip_x")),
                                      QStringLiteral("水平镜像"));
    c.orientation.flipY = requireBool(orientation.value(QStringLiteral("flip_y")),
                                      QStringLiteral("垂直镜像"));

    const QJsonObject optics = merged.value(kOptics).toObject();
    c.optics.sharedLens = requireBool(optics.value(QStringLiteral("shared_lens")),
                                      QStringLiteral("shared_lens"));
    c.optics.splitRatio = requireString(optics.value(QStringLiteral("split_ratio")),
                                        QStringLiteral("split_ratio"));
    require(c.optics.sharedLens && c.optics.splitRatio == QStringLiteral("1:1"),
            QStringLiteral("本工具对应共用一个镜头、1:1 分光的完整 Spike 支路"));
    c.optics.lensId = requireString(optics.value(QStringLiteral("lens_id")), QStringLiteral("lens_id"));
    c.optics.focusNote = requireString(optics.value(QStringLiteral("focus_note")), QStringLiteral("focus_note"));
    c.optics.splitterNote = requireString(optics.value(QStringLiteral("splitter_note")), QStringLiteral("splitter_note"));
    for (const QString& text : {c.optics.lensId, c.optics.focusNote, c.optics.splitterNote}) {
        require(text.size() <= 1000, QStringLiteral("光路记录文本不能超过 1000 字"));
    }

    const QJsonObject acceptance = merged.value(kAcceptance).toObject();
    if (!isNone(acceptance.value(QStringLiteral("intrinsics_p95_px")))) {
        c.acceptance.intrinsicsP95Px = requirePositiveNumber(
            acceptance.value(QStringLiteral("intrinsics_p95_px")), QStringLiteral("intrinsics_p95_px"));
    }
    if (!isNone(acceptance.value(QStringLiteral("turntable_p95_deg")))) {
        c.acceptance.turntableP95Deg = requirePositiveNumber(
            acceptance.value(QStringLiteral("turntable_p95_deg")), QStringLiteral("turntable_p95_deg"));
    }
    c.oper = requireString(merged.value(QStringLiteral("operator")), QStringLiteral("操作人"));
    require(c.oper.size() <= 200, QStringLiteral("操作人必须为不超过 200 字的文本"));
    return c;
}

QJsonArray point2Array(const std::vector<cv::Point2d>& points)
{
    QJsonArray array;
    for (const auto& p : points) array.append(QJsonArray{QJsonValue(p.x), QJsonValue(p.y)});
    return array;
}

QJsonArray point3Array(const std::vector<cv::Point3d>& points)
{
    QJsonArray array;
    for (const auto& p : points) array.append(QJsonArray{QJsonValue(p.x), QJsonValue(p.y), QJsonValue(p.z)});
    return array;
}

std::vector<cv::Point2d> point2Vector(const QJsonValue& value, const QString& name)
{
    const auto rows = numberMatrix(value, name, 2);
    std::vector<cv::Point2d> result;
    result.reserve(rows.size());
    for (const auto& row : rows) result.emplace_back(row[0], row[1]);
    return result;
}

std::vector<cv::Point3d> point3Vector(const QJsonValue& value, const QString& name)
{
    const auto rows = numberMatrix(value, name, 3);
    std::vector<cv::Point3d> result;
    result.reserve(rows.size());
    for (const auto& row : rows) result.emplace_back(row[0], row[1], row[2]);
    return result;
}

QJsonObject captureToJson(const Capture& c)
{
    QJsonObject out = c.raw;
    out.insert(QStringLiteral("id"), c.id);
    out.insert(QStringLiteral("name"), c.name);
    out.insert(QStringLiteral("sequence_id"), c.sequenceId);
    out.insert(QStringLiteral("split"), c.split);
    out.insert(QStringLiteral("evidence"), c.evidence);
    out.insert(QStringLiteral("enabled"), c.enabled);
    out.insert(QStringLiteral("found"), c.found);
    out.insert(QStringLiteral("image_size"), QJsonArray{QJsonValue(c.width), QJsonValue(c.height)});
    out.insert(QStringLiteral("object_points"), point3Array(c.objectPoints));
    out.insert(QStringLiteral("image_points"), point2Array(c.imagePoints));
    out.insert(QStringLiteral("coordinate_domain"), c.coordinateDomain);
    out.insert(QStringLiteral("quality"), c.quality);
    out.insert(QStringLiteral("source"), c.source);
    if (!c.previewBase64.isEmpty()) out.insert(QStringLiteral("preview_base64"), c.previewBase64);
    QJsonArray reasons;
    for (const QString& reason : c.reasons) reasons.append(reason);
    out.insert(QStringLiteral("reasons"), reasons);
    return out;
}

Capture validateCapture(const QJsonObject& input)
{
    QJsonObject r = input;
    for (const QString& key : {QStringLiteral("enabled"), QStringLiteral("found")}) {
        if (!r.contains(key)) r.insert(key, true);
    }
    if (!r.contains(QStringLiteral("coordinate_domain"))) {
        r.insert(QStringLiteral("coordinate_domain"), QStringLiteral("transformed_pixels"));
    }
    if (!r.contains(QStringLiteral("quality"))) r.insert(QStringLiteral("quality"), QJsonObject());
    if (!r.contains(QStringLiteral("source"))) r.insert(QStringLiteral("source"), QJsonObject());

    Capture c;
    c.raw = r;
    for (const QString& key : {QStringLiteral("id"), QStringLiteral("name"),
                               QStringLiteral("sequence_id")}) {
        const QString value = requireString(r.value(key), QStringLiteral("采集 ") + key);
        require(!value.trimmed().isEmpty() && value.size() <= 500,
                QStringLiteral("采集 %1 必须为有效文本").arg(key));
    }
    c.id = r.value(QStringLiteral("id")).toString();
    c.name = r.value(QStringLiteral("name")).toString();
    c.sequenceId = r.value(QStringLiteral("sequence_id")).toString();
    c.split = requireString(r.value(QStringLiteral("split")), QStringLiteral("观测 split"));
    require(c.split == QStringLiteral("train") || c.split == QStringLiteral("test"),
            QStringLiteral("观测 split 必须为 train 或 test"));
    c.evidence = requireString(r.value(QStringLiteral("evidence")), QStringLiteral("观测来源"));
    require(c.evidence == QStringLiteral("synthetic") || c.evidence == QStringLiteral("hardware")
                || c.evidence == QStringLiteral("public"),
            QStringLiteral("观测必须明确标记来源 synthetic、hardware 或 public"));
    c.enabled = requireBool(r.value(QStringLiteral("enabled")), QStringLiteral("enabled"));
    c.found = requireBool(r.value(QStringLiteral("found")), QStringLiteral("found"));

    const QJsonArray size = requireArray(r.value(QStringLiteral("image_size")), QStringLiteral("图像尺寸"));
    require(size.size() == 2, QStringLiteral("图像尺寸必须为有效的 [宽,高]"));
    c.width = requireInt(size.at(0), QStringLiteral("图像宽度"));
    c.height = requireInt(size.at(1), QStringLiteral("图像高度"));
    require(c.width > 0 && c.height > 0 && static_cast<double>(c.width) * c.height <= 25000000.0,
            QStringLiteral("图像尺寸必须为有效的 [宽,高]，且不超过 2500 万像素"));

    if (c.found) {
        c.imagePoints = point2Vector(r.value(QStringLiteral("image_points")), QStringLiteral("像点"));
        c.objectPoints = point3Vector(r.value(QStringLiteral("object_points")), QStringLiteral("靶标三维点"));
        require(c.imagePoints.size() >= 6 && c.imagePoints.size() <= 10000
                    && c.objectPoints.size() == c.imagePoints.size(),
                QStringLiteral("观测需要至少六个匹配的 [u,v] 像点与 [X,Y,Z] 靶标点"));
        for (const auto& p : c.objectPoints) {
            require(std::abs(p.z) <= 1e-10, QStringLiteral("靶标点必须为 Z=0 的非共线平面点，单位为米"));
        }
        require(matrixRank(c.objectPoints) >= 2,
                QStringLiteral("靶标点必须为 Z=0 的非共线平面点，单位为米"));
        for (const auto& p : c.imagePoints) {
            require(p.x >= 0.0 && p.y >= 0.0 && p.x < c.width && p.y < c.height,
                    QStringLiteral("角点超出图像范围"));
        }
        std::set<std::pair<double, double>> uniqueImage;
        std::set<std::tuple<double, double, double>> uniqueObject;
        for (const auto& p : c.imagePoints) uniqueImage.insert({p.x, p.y});
        for (const auto& p : c.objectPoints) uniqueObject.insert({p.x, p.y, p.z});
        require(uniqueImage.size() == c.imagePoints.size() && uniqueObject.size() == c.objectPoints.size(),
                QStringLiteral("角点与靶标点不得重复"));
    } else {
        require(r.value(QStringLiteral("image_points")).toArray().isEmpty()
                    && r.value(QStringLiteral("object_points")).toArray().isEmpty(),
                QStringLiteral("found=false 的观测不得携带有效角点"));
    }

    c.coordinateDomain = requireString(r.value(QStringLiteral("coordinate_domain")),
                                       QStringLiteral("coordinate_domain"));
    require(c.coordinateDomain == QStringLiteral("transformed_pixels"),
            QStringLiteral("导入角点必须已经按工程的旋转/镜像设置转换到 transformed_pixels"));
    c.quality = requireObject(r.value(QStringLiteral("quality")), QStringLiteral("quality"));
    c.source = requireObject(r.value(QStringLiteral("source")), QStringLiteral("source"));
    const QJsonValue digest = c.source.value(QStringLiteral("sha256"));
    if (!isNone(digest)) {
        static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{64}$"));
        require(digest.isString() && hex.match(digest.toString()).hasMatch(),
                QStringLiteral("source.sha256 必须为64位十六进制文本"));
    }
    if (r.contains(QStringLiteral("preview_base64")) && !r.value(QStringLiteral("preview_base64")).isNull()) {
        c.previewBase64 = requireString(r.value(QStringLiteral("preview_base64")),
                                        QStringLiteral("preview_base64"));
    }
    const QJsonArray reasons = r.value(QStringLiteral("reasons")).toArray();
    for (const QJsonValue& item : reasons) {
        if (item.isString()) c.reasons << item.toString();
    }
    return c;
}

QJsonArray capturesToJson(const std::vector<Capture>& captures)
{
    QJsonArray array;
    for (const Capture& capture : captures) array.append(captureToJson(capture));
    return array;
}

std::vector<Capture> capturesFromJson(const QJsonArray& captures)
{
    std::vector<Capture> result;
    result.reserve(static_cast<size_t>(captures.size()));
    for (const QJsonValue& item : captures) {
        result.push_back(validateCapture(requireObject(item, QStringLiteral("采集观测"))));
    }
    return result;
}

QString captureSignature(const Capture& c)
{
    QByteArray bytes;
    for (const auto& p : c.imagePoints) {
        bytes += doubleBytesLE(p.x);
        bytes += doubleBytesLE(p.y);
    }
    for (const auto& p : c.objectPoints) {
        bytes += doubleBytesLE(p.x);
        bytes += doubleBytesLE(p.y);
        bytes += doubleBytesLE(p.z);
    }
    return sha256Hex(bytes);
}

std::vector<Capture> activeCaptures(const std::vector<Capture>& captures)
{
    std::vector<Capture> checked;
    std::set<QString> trainSequences, testSequences, ids, trainSignatures;
    for (const Capture& c : captures) {
        if (!c.enabled || !c.found) continue;
        checked.push_back(c);
        if (!ids.insert(c.id).second) fail(QStringLiteral("采集 ID 不得重复"));
        (c.split == QStringLiteral("train") ? trainSequences : testSequences).insert(c.sequenceId);
        if (c.split == QStringLiteral("train")) trainSignatures.insert(captureSignature(c));
    }
    for (const QString& sequence : trainSequences) {
        require(!testSequences.count(sequence),
                QStringLiteral("训练与独立验证不得使用同一采集序列，请重新划分 sequence_id"));
    }
    for (const Capture& c : checked) {
        if (c.split == QStringLiteral("test")) {
            require(!trainSignatures.count(captureSignature(c)),
                    QStringLiteral("训练与验证包含完全相同的角点观测，修改文件名或序列名不能构成独立验证"));
        }
    }
    return checked;
}

Camera cameraFromModel(const QJsonObject& model)
{
    require(!model.isEmpty(), QStringLiteral("请先求解 Spike 内参"));
    const auto K = numberMatrix(model.value(QStringLiteral("K")), QStringLiteral("内参 K"), 3);
    require(K.size() == 3 && K[0].size() == 3 && K[1].size() == 3 && K[2].size() == 3,
            QStringLiteral("内参 K 的形状或数值无效"));
    for (const auto& row : K) {
        require(row.size() == 3, QStringLiteral("内参 K 的形状或数值无效"));
    }
    const auto d = numberVector(model.value(QStringLiteral("distortion")), QStringLiteral("畸变参数"));
    require(d.size() == 4 || d.size() == 5 || d.size() == 8 || d.size() == 12 || d.size() == 14,
            QStringLiteral("相机内参或畸变参数无效"));
    require(K[0][0] > 0 && K[1][1] > 0 && std::abs(K[2][0]) < 1e-9
                && std::abs(K[2][1]) < 1e-9 && std::abs(K[2][2] - 1.0) < 1e-9,
            QStringLiteral("相机内参或畸变参数无效"));
    const QJsonArray size = requireArray(model.value(QStringLiteral("image_size")), QStringLiteral("图像尺寸"));
    require(size.size() == 2, QStringLiteral("标定模型缺少有效图像尺寸"));
    Camera camera;
    camera.K = cv::Matx33d(K[0][0], K[0][1], K[0][2],
                           K[1][0], K[1][1], K[1][2],
                           K[2][0], K[2][1], K[2][2]);
    camera.distortion = d;
    camera.width = requireInt(size.at(0), QStringLiteral("图像宽度"));
    camera.height = requireInt(size.at(1), QStringLiteral("图像高度"));
    require(camera.width > 0 && camera.height > 0, QStringLiteral("标定模型缺少有效图像尺寸"));
    return camera;
}

QJsonObject queryRays(const QJsonObject& model, const QJsonArray& points,
                      const QJsonValue& turntable, const QJsonValue& encoderDeg)
{
    const Camera camera = cameraFromModel(model);
    const auto xy = point2Vector(points, QStringLiteral("查询像点"));
    require(xy.size() >= 1 && xy.size() <= 10000,
            QStringLiteral("查询像点必须为非空 [[u,v],...]，最多10000点"));
    for (const auto& p : xy) {
        require(p.x >= 0.0 && p.y >= 0.0 && p.x < camera.width && p.y < camera.height,
                QStringLiteral("查询像点超出标定图像范围"));
    }

    std::vector<cv::Point2d> source = xy;
    cv::Mat src(static_cast<int>(source.size()), 1, CV_64FC2, source.data());
    cv::Mat Kmat = cv::Mat(camera.K).clone();
    cv::Mat dmat(1, static_cast<int>(camera.distortion.size()), CV_64F,
                 camera.distortion.empty() ? nullptr : camera.distortion.data());
    cv::Mat undistorted;
    cv::undistortPoints(src, undistorted, Kmat, dmat);

    std::optional<double> encoder;
    if (!isNone(encoderDeg)) encoder = requireNumber(encoderDeg, QStringLiteral("当前转台角度"));
    QJsonObject table = isNone(turntable) ? QJsonObject()
                                          : requireObject(turntable, QStringLiteral("转台模型"));

    QJsonArray output;
    for (int i = 0; i < static_cast<int>(xy.size()); ++i) {
        double ux = 0.0, uy = 0.0;
        if (undistorted.depth() == CV_64F) {
            const cv::Vec2d u = undistorted.at<cv::Vec2d>(i, 0);
            ux = u[0];
            uy = u[1];
        } else {
            const cv::Vec2f u = undistorted.at<cv::Vec2f>(i, 0);
            ux = u[0];
            uy = u[1];
        }
        cv::Vec3d ray(ux, uy, 1.0);
        const double norm = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
        ray /= norm;
        const double azimuth = std::atan2(ray[0], ray[2]) * kRadToDeg;
        const double elevation = std::atan2(-ray[1], std::hypot(ray[0], ray[2])) * kRadToDeg;

        QJsonObject record;
        record.insert(QStringLiteral("pixel"), QJsonArray{QJsonValue(xy[static_cast<size_t>(i)].x),
                                                          QJsonValue(xy[static_cast<size_t>(i)].y)});
        record.insert(QStringLiteral("ray_camera"),
                      QJsonArray{QJsonValue(ray[0]), QJsonValue(ray[1]), QJsonValue(ray[2])});
        record.insert(QStringLiteral("azimuth_deg"), azimuth);
        record.insert(QStringLiteral("elevation_deg"), elevation);
        if (!table.isEmpty()) {
            const double zero = requireNumber(table.value(QStringLiteral("zero_offset_deg")),
                                              QStringLiteral("转台零位"));
            const int sign = requireInt(table.value(QStringLiteral("sign")), QStringLiteral("转台方向"));
            require(sign == 1 || sign == -1, QStringLiteral("转台模型方向无效"));
            const double body = wrapDegrees(azimuth + zero);
            record.insert(QStringLiteral("body_azimuth_deg"), body);
            if (encoder) {
                record.insert(QStringLiteral("world_azimuth_deg"),
                              wrapDegrees(sign * *encoder + body));
                const QJsonObject reference = table.value(QStringLiteral("reference")).toObject();
                if (reference.value(QStringLiteral("mode")).toString() == QStringLiteral("far_field")) {
                    record.insert(QStringLiteral("encoder_target_deg"), *encoder + azimuth / sign);
                    record.insert(QStringLiteral("encoder_target_semantics"),
                                  QStringLiteral("远场近似下按当前观测视线进行偏航补偿；仅适用于已确认的远场条件"));
                } else {
                    record.insert(QStringLiteral("direction_only_note"),
                                  QStringLiteral("输出视线方向；未测目标深度，不计算有限距离目标的转台指向角"));
                }
                const QJsonArray limits = table.value(QStringLiteral("encoder_range_deg")).toArray();
                if (limits.size() == 2) {
                    const double low = requireNumber(limits.at(0), QStringLiteral("转台标定范围"));
                    const double high = requireNumber(limits.at(1), QStringLiteral("转台标定范围"));
                    record.insert(QStringLiteral("encoder_within_calibrated_range"),
                                  low <= *encoder && *encoder <= high);
                }
            }
        }
        output.append(record);
    }
    QJsonObject result;
    result.insert(QStringLiteral("points"), output);
    result.insert(QStringLiteral("coordinate_domain"), QStringLiteral("transformed_pixels"));
    result.insert(QStringLiteral("angle_convention"),
                  QStringLiteral("水平向右为正，垂直向上为正；ray_camera按X右Y下Z前"));
    result.insert(QStringLiteral("image_size"),
                  QJsonArray{QJsonValue(camera.width), QJsonValue(camera.height)});
    return result;
}

} // namespace spike

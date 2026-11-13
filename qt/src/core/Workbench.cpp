#include "Workbench.h"

#include "Calibration.h"
#include "Fixtures.h"
#include "Importers.h"
#include "Targets.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVector>

#include <algorithm>
#include <set>
#include <utility>

namespace spike {
namespace {

constexpr qint64 kMaxUploadBytes = 64 * 1024 * 1024;

QJsonObject emptyProjectCore()
{
    QJsonObject project;
    project.insert(QStringLiteral("captures"), QJsonArray());
    project.insert(QStringLiteral("model"), QJsonValue());
    project.insert(QStringLiteral("validation"), QJsonValue());
    project.insert(QStringLiteral("turntable"), QJsonValue());
    return project;
}

bool nonCollinear(const QJsonValue& objectPoints)
{
    const auto rows = numberMatrix(objectPoints, QStringLiteral("靶标三维点"), 3);
    if (rows.size() < 3) return false;
    double meanX = 0.0, meanY = 0.0;
    for (const auto& row : rows) { meanX += row[0]; meanY += row[1]; }
    meanX /= static_cast<double>(rows.size());
    meanY /= static_cast<double>(rows.size());
    cv::Mat centered(static_cast<int>(rows.size()), 2, CV_64F);
    for (size_t i = 0; i < rows.size(); ++i) {
        centered.at<double>(static_cast<int>(i), 0) = rows[i][0] - meanX;
        centered.at<double>(static_cast<int>(i), 1) = rows[i][1] - meanY;
    }
    cv::Mat w;
    cv::SVD::compute(centered, w, cv::SVD::NO_UV);
    return w.rows >= 2 && w.at<double>(0) > 0.0 && w.at<double>(1) / w.at<double>(0) >= 1e-9;
}

QJsonObject readProjectFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取工程文件：%1").arg(path));
    const QJsonValue value = strictLoads(file.readAll());
    return requireObject(value, QStringLiteral("工程"));
}

} // namespace

Workbench::Workbench(const QString& root) : m_root(root)
{
    QDir().mkpath(m_root);
    QDir directory(m_root);
    const QFileInfoList files = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo& file : files) {
        try {
            QJsonObject project = readProjectFile(file.absoluteFilePath());
            require(project.value(QStringLiteral("id")).toString() == file.completeBaseName(),
                    QStringLiteral("project identity mismatch"));
            checkId(project.value(QStringLiteral("id")).toString());
            dumpsJson(project);
            m_projects.insert(project.value(QStringLiteral("id")).toString(), project);
        } catch (const std::exception& error) {
            m_loadErrors.append(QStringLiteral("%1: %2").arg(file.fileName(),
                                                              QString::fromUtf8(error.what())));
        }
    }
}

void Workbench::setHardwareConnected(bool connected)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_hardwareConnected = connected;
}

bool Workbench::hardwareConnected() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_hardwareConnected;
}

void Workbench::checkId(const QString& id) const
{
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{32}$"));
    require(pattern.match(id).hasMatch(), QStringLiteral("工程不存在"));
}

QJsonObject Workbench::projectCopy(const QString& id) const
{
    checkId(id);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_projects.contains(id)) fail(QStringLiteral("工程不存在"));
    return m_projects.value(id);
}

QJsonObject Workbench::state() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonArray projects;
    QVector<QJsonObject> briefs;
    for (auto it = m_projects.constBegin(); it != m_projects.constEnd(); ++it) {
        QJsonObject brief;
        brief.insert(QStringLiteral("id"), it.key());
        brief.insert(QStringLiteral("name"), it.value().value(QStringLiteral("name")));
        brief.insert(QStringLiteral("status"), it.value().value(QStringLiteral("status")));
        brief.insert(QStringLiteral("capture_count"),
                     static_cast<qint64>(it.value().value(QStringLiteral("captures")).toArray().size()));
        brief.insert(QStringLiteral("updated_at"), it.value().value(QStringLiteral("updated_at")));
        briefs.append(brief);
    }
    std::sort(briefs.begin(), briefs.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value(QStringLiteral("updated_at")).toString()
            > b.value(QStringLiteral("updated_at")).toString();
    });
    for (const QJsonObject& brief : briefs) projects.append(brief);

    QJsonArray loadErrors;
    for (const QString& error : m_loadErrors) loadErrors.append(error);

    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("sensor"), QStringLiteral("Spike"));
    capabilities.insert(QStringLiteral("mode"), QStringLiteral("offline"));
    capabilities.insert(QStringLiteral("device_sdk"), QStringLiteral("C++ ICameraDevice"));
    capabilities.insert(QStringLiteral("hardware_connected"), m_hardwareConnected);
    capabilities.insert(QStringLiteral("optics"),
                        QStringLiteral("shared lens / 1:1 splitter / Spike branch"));
    capabilities.insert(QStringLiteral("max_upload_bytes"), kMaxUploadBytes);
    capabilities.insert(QStringLiteral("inputs"),
                        QJsonArray{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("tif"),
                                   QStringLiteral("npy"), QStringLiteral("dat (explicit layout)"),
                                   QStringLiteral("observations.json")});

    QJsonObject result;
    result.insert(QStringLiteral("app"), QString::fromUtf8(kAppId));
    result.insert(QStringLiteral("version"), QString::fromUtf8(kVersion));
    result.insert(QStringLiteral("projects"), projects);
    result.insert(QStringLiteral("load_errors"), loadErrors);
    result.insert(QStringLiteral("capabilities"), capabilities);
    return result;
}

QJsonObject Workbench::save(QJsonObject project)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const QString id = project.value(QStringLiteral("id")).toString();
    checkId(id);
    const QString path = QDir(m_root).filePath(id + QStringLiteral(".json"));
    if (QFileInfo::exists(path)) {
        const QJsonObject stored = readProjectFile(path);
        if (stored.value(QStringLiteral("revision")).toInt()
            != project.value(QStringLiteral("revision")).toInt()) {
            m_projects.insert(id, stored);
            fail(QStringLiteral("该工程已在另一窗口修改，请刷新后重试"));
        }
    }
    project.insert(QStringLiteral("revision"), project.value(QStringLiteral("revision")).toInt() + 1);
    project.insert(QStringLiteral("updated_at"), nowIso());
    const QByteArray encoded = dumpsJson(project).toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法写入工程文件"));
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        fail(QStringLiteral("无法写入工程文件"));
    }
    m_projects.insert(id, project);
    return project;
}

void Workbench::invalidate(QJsonObject& project)
{
    project.insert(QStringLiteral("model"), QJsonValue());
    project.insert(QStringLiteral("validation"), QJsonValue());
    project.insert(QStringLiteral("turntable"), QJsonValue());
    project.insert(QStringLiteral("status"),
                   project.value(QStringLiteral("captures")).toArray().isEmpty()
                       ? QStringLiteral("empty") : QStringLiteral("collecting"));
}

QJsonObject Workbench::createProject(const QString& name)
{
    const QString trimmed = name.trimmed();
    require(!trimmed.isEmpty() && trimmed.size() <= 120,
            QStringLiteral("工程名称应为 1～120 个字符"));
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = emptyProjectCore();
    project.insert(QStringLiteral("id"), makeId());
    project.insert(QStringLiteral("name"), trimmed);
    project.insert(QStringLiteral("config"), defaultConfigJson());
    project.insert(QStringLiteral("revision"), 0);
    project.insert(QStringLiteral("status"), QStringLiteral("empty"));
    project.insert(QStringLiteral("updated_at"), nowIso());
    return save(project);
}

QJsonObject Workbench::getProject(const QString& id) const
{
    return projectCopy(id);
}

QJsonObject Workbench::updateConfig(const QString& id, const QJsonObject& body)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    const QJsonValue patchValue = body.contains(QStringLiteral("config"))
                                      ? body.value(QStringLiteral("config")) : QJsonValue(body);
    const QJsonObject patch = requireObject(patchValue, QStringLiteral("配置"));
    const QJsonObject defaults = defaultConfigJson();
    for (const QString& key : patch.keys()) {
        require(defaults.contains(key), QStringLiteral("配置字段不正确"));
    }
    QJsonObject config = project.value(QStringLiteral("config")).toObject();
    for (const QString& key : patch.keys()) {
        if (config.value(key).isObject() && patch.value(key).isObject()) {
            QJsonObject nested = config.value(key).toObject();
            const QJsonObject values = patch.value(key).toObject();
            for (const QString& sub : values.keys()) nested.insert(sub, values.value(sub));
            config.insert(key, nested);
        } else {
            config.insert(key, patch.value(key));
        }
    }
    const Config validated = validateConfig(config);
    const QJsonObject normalized = configToJson(validated);
    dumpsJson(normalized);
    if (normalized == project.value(QStringLiteral("config")).toObject()) return project;

    bool geometryChanged = false;
    for (const QString& key : {QStringLiteral("target"), QStringLiteral("orientation"),
                               QStringLiteral("optics")}) {
        if (normalized.value(key) != project.value(QStringLiteral("config"))
                                           .toObject().value(key)) {
            geometryChanged = true;
        }
    }
    project.insert(QStringLiteral("config"), normalized);
    if (geometryChanged) {
        project.insert(QStringLiteral("captures"), QJsonArray());
        invalidate(project);
    } else {
        project.insert(QStringLiteral("validation"), QJsonValue());
        project.insert(QStringLiteral("turntable"), QJsonValue());
        project.insert(QStringLiteral("status"),
                       !project.value(QStringLiteral("model")).isNull() ? QStringLiteral("solved")
                           : !project.value(QStringLiteral("captures")).toArray().isEmpty()
                               ? QStringLiteral("collecting") : QStringLiteral("empty"));
    }
    return save(project);
}

QJsonObject Workbench::loadDemo(const QString& id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    require(project.value(QStringLiteral("captures")).toArray().isEmpty(),
            QStringLiteral("请新建工程载入演示，以保留现有实采观测"));
    const Config config = validateConfig(project.value(QStringLiteral("config")).toObject());
    project.insert(QStringLiteral("captures"), capturesToJson(demoCaptures(config)));
    invalidate(project);
    return save(project);
}

void Workbench::checkCaptures(QJsonArray& captures, const QJsonArray& existing)
{
    require(!captures.isEmpty() && captures.size() <= 100,
            QStringLiteral("每批应导入 1～100 组观测"));
    std::set<QString> names;
    for (const QJsonValue& item : existing) {
        names.insert(item.toObject().value(QStringLiteral("name")).toString());
    }
    for (qsizetype i = 0; i < captures.size(); ++i) {
        QJsonObject capture = requireObject(captures.at(i), QStringLiteral("观测"));
        capture.insert(QStringLiteral("id"), makeId());
        const QString name = capture.value(QStringLiteral("name")).toString();
        require(!name.trimmed().isEmpty() && !names.count(name),
                QStringLiteral("观测文件名为空或重复，请使用不同文件名"));
        names.insert(name);
        const QString split = capture.value(QStringLiteral("split")).toString();
        require(split == QStringLiteral("train") || split == QStringLiteral("test"),
                QStringLiteral("需要明确训练/验证用途和数据来源"));
        const QString evidence = capture.value(QStringLiteral("evidence")).toString();
        require(evidence == QStringLiteral("hardware") || evidence == QStringLiteral("synthetic")
                    || evidence == QStringLiteral("public"),
                QStringLiteral("需要明确训练/验证用途和数据来源"));
        require(!capture.value(QStringLiteral("sequence_id")).toString().trimmed().isEmpty(),
                QStringLiteral("请填写真实采集序列标识"));
        if (!capture.contains(QStringLiteral("enabled"))) capture.insert(QStringLiteral("enabled"), true);
        if (!capture.contains(QStringLiteral("found"))) capture.insert(QStringLiteral("found"), true);
        require(capture.value(QStringLiteral("enabled")).isBool()
                    && capture.value(QStringLiteral("found")).isBool(),
                QStringLiteral("enabled/found 应为布尔值"));

        const QJsonArray size = requireArray(capture.value(QStringLiteral("image_size")),
                                             QStringLiteral("image_size"));
        require(size.size() == 2, QStringLiteral("image_size 应为有效 [宽,高]，上限 2500 万像素"));
        const int width = requireInt(size.at(0), QStringLiteral("图像宽度"));
        const int height = requireInt(size.at(1), QStringLiteral("图像高度"));
        require(width >= 2 && width <= 12000 && height >= 2 && height <= 12000
                    && static_cast<double>(width) * height <= 25000000.0,
                QStringLiteral("image_size 应为有效 [宽,高]，上限 2500 万像素"));
        if (capture.value(QStringLiteral("found")).toBool()) {
            const auto xy = numberMatrix(capture.value(QStringLiteral("image_points")),
                                         QStringLiteral("像点"), 2);
            const auto obj = numberMatrix(capture.value(QStringLiteral("object_points")),
                                          QStringLiteral("靶标三维点"), 3);
            require(xy.size() >= 6 && obj.size() == xy.size(),
                    QStringLiteral("每组有效观测至少需要 6 对二维/三维特征点"));
            for (const auto& point : xy) {
                require(point[0] >= 0.0 && point[1] >= 0.0
                            && point[0] < width && point[1] < height,
                        QStringLiteral("特征点超出图像范围"));
            }
            require(nonCollinear(capture.value(QStringLiteral("object_points"))),
                    QStringLiteral("标定点不能共线"));
        }
        QJsonObject source = capture.contains(QStringLiteral("source"))
                                 ? requireObject(capture.value(QStringLiteral("source")),
                                                 QStringLiteral("source"))
                                 : QJsonObject();
        const QString domain = capture.value(QStringLiteral("coordinate_domain"))
                                   .toString(QStringLiteral("transformed_pixels"));
        const QString sourceDomain = source.value(QStringLiteral("coordinate_domain"))
                                         .toString(QStringLiteral("transformed_pixels"));
        require(domain == QStringLiteral("transformed_pixels")
                    && sourceDomain == QStringLiteral("transformed_pixels"),
                QStringLiteral("观测坐标必须已应用当前旋转/镜像配置，使用 transformed_pixels"));
        if (!source.contains(QStringLiteral("coordinate_domain"))) {
            source.insert(QStringLiteral("coordinate_domain"), QStringLiteral("transformed_pixels"));
        }
        capture.insert(QStringLiteral("source"), source);

        const Capture normalized = validateCapture(capture);
        QJsonObject record = captureToJson(normalized);
        if (!record.contains(QStringLiteral("quality"))) {
            record.insert(QStringLiteral("quality"), QJsonObject());
        }
        const QString preview = record.value(QStringLiteral("preview_base64")).toString();
        require(preview.size() <= 24 * 1024 * 1024, QStringLiteral("预览图过大或格式错误"));
        if (!preview.isEmpty()) {
            const QByteArray binary = decodeBase64Strict(preview);
            const cv::Mat buffer(1, static_cast<int>(binary.size()), CV_8UC1,
                                 const_cast<char*>(binary.constData()));
            const cv::Mat image = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
            require(!image.empty() && image.cols == width && image.rows == height,
                    QStringLiteral("预览图无法解码或尺寸与 image_size 不一致"));
        }
        captures.replace(i, record);
    }
    const qsizetype total = existing.size() + captures.size();
    require(total <= 300, QStringLiteral("一个工程最多保留 300 组观测，请新建工程"));
    std::set<QString> trainSequences, testSequences;
    auto collect = [&](const QJsonArray& array) {
        for (const QJsonValue& item : array) {
            const QJsonObject capture = item.toObject();
            if (!capture.value(QStringLiteral("enabled")).toBool()) continue;
            (capture.value(QStringLiteral("split")).toString() == QStringLiteral("train")
                 ? trainSequences : testSequences).insert(capture.value(QStringLiteral("sequence_id")).toString());
        }
    };
    collect(existing);
    collect(captures);
    for (const QString& sequence : trainSequences) {
        require(!testSequences.count(sequence),
                QStringLiteral("求解与独立验证不能使用同一采集序列，请按完整序列划分"));
    }
}

QJsonObject Workbench::importFiles(const QString& id, const QJsonArray& files,
                                   const QString& split, const QString& sequenceId,
                                   const QString& evidence, const QJsonObject& options)
{
    require(!files.isEmpty() && files.size() <= 100, QStringLiteral("请选择 1～100 个文件"));
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    const Config config = validateConfig(project.value(QStringLiteral("config")).toObject());
    QJsonArray captures;
    for (const QJsonValue& item : files) {
        const FileRecord file = FileRecord::fromJson(requireObject(item, QStringLiteral("导入文件")));
        captures.append(captureToJson(importCapture(file, config, split, sequenceId, evidence, options)));
    }
    checkCaptures(captures, project.value(QStringLiteral("captures")).toArray());
    QJsonArray existing = project.value(QStringLiteral("captures")).toArray();
    for (const QJsonValue& item : captures) existing.append(item);
    project.insert(QStringLiteral("captures"), existing);
    invalidate(project);
    return save(project);
}

QJsonObject Workbench::importObservations(const QString& id, const QJsonArray& capturesIn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    QJsonArray captures = capturesIn;
    checkCaptures(captures, project.value(QStringLiteral("captures")).toArray());
    QJsonArray existing = project.value(QStringLiteral("captures")).toArray();
    for (const QJsonValue& item : captures) existing.append(item);
    project.insert(QStringLiteral("captures"), existing);
    invalidate(project);
    return save(project);
}

QJsonObject Workbench::updateCapture(const QString& id, const QString& captureId,
                                     const QJsonObject& body)
{
    require(!body.isEmpty(), QStringLiteral("只允许修改启用状态和用途"));
    for (const QString& key : body.keys()) {
        require(key == QStringLiteral("enabled") || key == QStringLiteral("split"),
                QStringLiteral("只允许修改启用状态和用途"));
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    QJsonArray captures = project.value(QStringLiteral("captures")).toArray();
    bool found = false;
    std::set<QString> trainSequences, testSequences;
    for (qsizetype i = 0; i < captures.size(); ++i) {
        QJsonObject capture = captures.at(i).toObject();
        if (capture.value(QStringLiteral("id")).toString() != captureId) {
            if (capture.value(QStringLiteral("enabled")).toBool()) {
                (capture.value(QStringLiteral("split")).toString() == QStringLiteral("train")
                     ? trainSequences : testSequences)
                    .insert(capture.value(QStringLiteral("sequence_id")).toString());
            }
            continue;
        }
        found = true;
        if (body.contains(QStringLiteral("enabled"))) {
            require(body.value(QStringLiteral("enabled")).isBool(),
                    QStringLiteral("enabled 应为布尔值"));
            capture.insert(QStringLiteral("enabled"), body.value(QStringLiteral("enabled")).toBool());
        }
        if (body.contains(QStringLiteral("split"))) {
            const QString split = body.value(QStringLiteral("split")).toString();
            require(split == QStringLiteral("train") || split == QStringLiteral("test"),
                    QStringLiteral("用途应为 train 或 test"));
            capture.insert(QStringLiteral("split"), split);
        }
        if (capture.value(QStringLiteral("enabled")).toBool()) {
            (capture.value(QStringLiteral("split")).toString() == QStringLiteral("train")
                 ? trainSequences : testSequences)
                .insert(capture.value(QStringLiteral("sequence_id")).toString());
        }
        captures.replace(i, capture);
    }
    require(found, QStringLiteral("观测不存在"));
    for (const QString& sequence : trainSequences) {
        require(!testSequences.count(sequence),
                QStringLiteral("同一采集序列不能同时用于求解和独立验证"));
    }
    project.insert(QStringLiteral("captures"), captures);
    invalidate(project);
    return save(project);
}

QJsonObject Workbench::solve(const QString& id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    const Config config = validateConfig(project.value(QStringLiteral("config")).toObject());
    const std::vector<Capture> captures =
        capturesFromJson(project.value(QStringLiteral("captures")).toArray());
    QJsonObject model = solveIntrinsics(captures, config);
    model.insert(QStringLiteral("config_fingerprint"),
                 configFingerprint(project.value(QStringLiteral("config")).toObject()));
    project.insert(QStringLiteral("model"), model);
    project.insert(QStringLiteral("validation"), QJsonValue());
    project.insert(QStringLiteral("turntable"), QJsonValue());
    project.insert(QStringLiteral("status"), QStringLiteral("solved"));
    return save(project);
}

QJsonObject Workbench::validate(const QString& id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    require(!project.value(QStringLiteral("model")).isNull(), QStringLiteral("请先完成内参求解"));
    const Config config = validateConfig(project.value(QStringLiteral("config")).toObject());
    const std::vector<Capture> captures =
        capturesFromJson(project.value(QStringLiteral("captures")).toArray());
    project.insert(QStringLiteral("validation"),
                   validateIntrinsics(project.value(QStringLiteral("model")).toObject(),
                                     captures, config));
    project.insert(QStringLiteral("status"), QStringLiteral("validated"));
    return save(project);
}

QJsonObject Workbench::calibrateTurntable(const QString& id, const QJsonArray& observations,
                                          const QJsonObject& reference)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    QJsonObject project = projectCopy(id);
    require(!project.value(QStringLiteral("model")).isNull(), QStringLiteral("请先完成内参求解"));
    const Config config = validateConfig(project.value(QStringLiteral("config")).toObject());
    project.insert(QStringLiteral("turntable"),
                   fitTurntable(project.value(QStringLiteral("model")).toObject(),
                                observations, reference, config));
    return save(project);
}

QJsonObject Workbench::query(const QString& id, const QJsonArray& points,
                             const QJsonValue& encoderDeg) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const QJsonObject project = projectCopy(id);
    require(!project.value(QStringLiteral("model")).isNull(), QStringLiteral("请先完成内参求解"));
    return queryRays(project.value(QStringLiteral("model")).toObject(), points,
                     project.value(QStringLiteral("turntable")), encoderDeg);
}

QByteArray Workbench::preview(const QString& id, const QString& captureId, bool undistort) const
{
    const QJsonObject project = projectCopy(id);
    QJsonObject capture;
    for (const QJsonValue& item : project.value(QStringLiteral("captures")).toArray()) {
        if (item.toObject().value(QStringLiteral("id")).toString() == captureId) {
            capture = item.toObject();
            break;
        }
    }
    require(!capture.isEmpty(), QStringLiteral("观测不存在"));
    cv::Mat image;
    const QString encoded = capture.value(QStringLiteral("preview_base64")).toString();
    if (!encoded.isEmpty()) {
        const QByteArray binary = decodeBase64Strict(encoded);
        const cv::Mat buffer(1, static_cast<int>(binary.size()), CV_8UC1,
                             const_cast<char*>(binary.constData()));
        image = cv::imdecode(buffer, cv::IMREAD_COLOR);
    }
    const QJsonArray size = capture.value(QStringLiteral("image_size")).toArray();
    if (image.empty()) {
        image = cv::Mat(size.at(1).toInt(), size.at(0).toInt(), CV_8UC3,
                        cv::Scalar(35, 35, 35));
    }
    std::vector<cv::Point2f> corners;
    for (const QJsonValue& item : capture.value(QStringLiteral("image_points")).toArray()) {
        const QJsonArray point = item.toArray();
        corners.emplace_back(static_cast<float>(point.at(0).toDouble()),
                             static_cast<float>(point.at(1).toDouble()));
    }
    if (undistort) {
        require(!project.value(QStringLiteral("model")).isNull(), QStringLiteral("请先完成内参求解"));
        const Camera camera = cameraFromModel(project.value(QStringLiteral("model")).toObject());
        const cv::Mat K = cv::Mat(camera.K).clone();
        std::vector<double> distortionValues = camera.distortion;
        const cv::Mat d(1, static_cast<int>(distortionValues.size()), CV_64F,
                        distortionValues.data());
        // OpenCV 5 refuses src == dst here, and the preview image may also be the
        // empty-canvas fallback, so map into a dedicated buffer.
        cv::Mat undistortedImage;
        cv::undistort(image, undistortedImage, K, d);
        image = undistortedImage;
        if (!corners.empty()) {
            const cv::Mat source(static_cast<int>(corners.size()), 1, CV_32FC2, corners.data());
            cv::Mat destination;
            cv::undistortPoints(source, destination, K, d, cv::noArray(), K);
            for (int i = 0; i < static_cast<int>(corners.size()); ++i) {
                const cv::Vec2f point = destination.at<cv::Vec2f>(i, 0);
                corners[static_cast<size_t>(i)] = cv::Point2f(point[0], point[1]);
            }
        }
    }
    for (size_t i = 0; i < corners.size(); ++i) {
        const cv::Scalar color = i == 0 ? cv::Scalar(70, 230, 70) : cv::Scalar(50, 210, 255);
        cv::circle(image, cv::Point(cvRound(corners[i].x), cvRound(corners[i].y)),
                   std::max(2, image.cols / 250), color, 1, cv::LINE_AA);
    }
    return encodePng(image);
}

QJsonObject Workbench::exportProject(const QString& id) const
{
    const QJsonObject project = projectCopy(id);
    std::set<QString> evidence;
    for (const QJsonValue& item : project.value(QStringLiteral("captures")).toArray()) {
        const QJsonObject capture = item.toObject();
        if (capture.value(QStringLiteral("enabled")).toBool()) {
            evidence.insert(capture.value(QStringLiteral("evidence")).toString());
        }
    }
    QJsonArray evidenceArray;
    for (const QString& value : evidence) evidenceArray.append(value);

    QJsonObject bundle;
    bundle.insert(QStringLiteral("format"), QStringLiteral("spike-calibration-bundle"));
    bundle.insert(QStringLiteral("format_version"), 1);
    bundle.insert(QStringLiteral("software_version"), QString::fromUtf8(kVersion));
    bundle.insert(QStringLiteral("exported_at"), nowIso());
    bundle.insert(QStringLiteral("sensor"), QStringLiteral("Spike"));
    bundle.insert(QStringLiteral("optical_configuration"),
                  QStringLiteral("shared_lens_1_to_1_splitter"));
    bundle.insert(QStringLiteral("coordinate_convention"),
                  QStringLiteral("pixels after configured rotation/flips; azimuth right-positive; elevation up-positive"));
    bundle.insert(QStringLiteral("evidence"), evidenceArray);
    bundle.insert(QStringLiteral("hardware_certified"), false);
    bundle.insert(QStringLiteral("limitations"),
                  QJsonArray{
                      QStringLiteral("标定仅绑定当前镜头、对焦、分光器安装及 Spike 图像坐标配置。"),
                      QStringLiteral("软件验证或合成结果不代表实机精度验收；阈值需依据实际要求填写。"),
                      QStringLiteral("单轴转台模型不估计完整六自由度；近距离模式依赖实测目标与旋转中心几何。")});
    bundle.insert(QStringLiteral("project"), project);
    return bundle;
}

QString Workbench::reportHtml(const QString& id) const
{
    const QJsonObject bundle = exportProject(id);
    const QJsonObject project = bundle.value(QStringLiteral("project")).toObject();
    QString sections;
    const std::vector<std::pair<QString, QString>> blocks = {
        {QStringLiteral("设备及标定配置"), QStringLiteral("config")},
        {QStringLiteral("内参和畸变"), QStringLiteral("model")},
        {QStringLiteral("独立验证"), QStringLiteral("validation")},
        {QStringLiteral("单轴转台"), QStringLiteral("turntable")}};
    for (const auto& [title, key] : blocks) {
        sections += QStringLiteral("<section><h2>%1</h2><pre>%2</pre></section>")
                        .arg(htmlEscape(title),
                             htmlEscape(dumpsJson(project.value(key))));
    }
    QString rows;
    for (const QJsonValue& item : project.value(QStringLiteral("captures")).toArray()) {
        const QJsonObject capture = item.toObject();
        rows += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td></tr>")
                    .arg(htmlEscape(capture.value(QStringLiteral("name")).toString()),
                         htmlEscape(capture.value(QStringLiteral("sequence_id")).toString()),
                         htmlEscape(capture.value(QStringLiteral("split")).toString()),
                         htmlEscape(capture.value(QStringLiteral("evidence")).toString()),
                         capture.value(QStringLiteral("enabled")).toBool()
                             ? QStringLiteral("启用") : QStringLiteral("排除"),
                         QString::number(capture.value(QStringLiteral("image_points")).toArray().size()));
    }
    QString limits;
    for (const QJsonValue& item : bundle.value(QStringLiteral("limitations")).toArray()) {
        limits += QStringLiteral("<li>%1</li>").arg(htmlEscape(item.toString()));
    }
    QString evidence;
    for (const QJsonValue& item : bundle.value(QStringLiteral("evidence")).toArray()) {
        if (!evidence.isEmpty()) evidence += QStringLiteral(", ");
        evidence += htmlEscape(item.toString());
    }
    const QString style = QStringLiteral(
        "body{max-width:1080px;margin:40px auto;padding:0 24px;font:15px/1.7 \"Microsoft YaHei\",sans-serif;color:#203047}"
        "h1{font-size:28px}h2{font-size:19px;margin-top:32px}"
        "pre{background:#f3f6fa;padding:18px;white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.6 Consolas,monospace}"
        "table{border-collapse:collapse;width:100%}th,td{padding:8px;text-align:left;border-bottom:1px solid #dae1e9}"
        ".note{border-left:3px solid #1668dc;background:#eef5ff;padding:14px}"
        "@media print{body{margin:0}pre{font-size:10px}h2{break-after:avoid}}");
    const QString name = htmlEscape(project.value(QStringLiteral("name")).toString());
    const QString version = htmlEscape(QString::fromUtf8(kVersion));
    const QString exported = htmlEscape(bundle.value(QStringLiteral("exported_at")).toString());
    const QString projectId = htmlEscape(project.value(QStringLiteral("id")).toString());
    const QString revision = QString::number(project.value(QStringLiteral("revision")).toInt());
    return QStringLiteral("<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\"><title>")
        + name + QStringLiteral(" · Spike 标定报告</title><style>") + style
        + QStringLiteral("</style><h1>") + name
        + QStringLiteral("</h1><p>Spike 单支路标定报告 · 软件 ") + version
        + QStringLiteral(" · ") + exported
        + QStringLiteral("</p><p class=\"note\">共用一个镜头，经 1:1 分光器进入 Spike 支路。"
                         "离线软件结果；未认证实机合格。数据来源：")
        + evidence + QStringLiteral("。</p>") + sections
        + QStringLiteral("<section><h2>观测来源与划分</h2><table><thead><tr><th>文件</th>"
                         "<th>采集序列</th><th>用途</th><th>来源</th><th>状态</th><th>特征点</th>"
                         "</tr></thead><tbody>")
        + rows + QStringLiteral("</tbody></table></section><h2>适用范围</h2><ul>")
        + limits + QStringLiteral("</ul><p>工程 ") + projectId
        + QStringLiteral(" · 修订 ") + revision + QStringLiteral("</p></html>");
}

} // namespace spike

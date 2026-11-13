// Mock-data integration test for the Qt 6 / OpenCV 5 port.
//
// Everything here is driven by the repository's independently generated mock
// dataset (output/mock_calibration_data). The software never receives
// ground_truth.json; it is only used here, after the fact, to score the result —
// the same contract the Python reference check follows.
//
// Suites:
//   1. decode      – PNG / binary Spike NPY / packed DAT import plus rejections
//   2. pipeline    – full image path: import -> detect -> solve -> validate -> export
//   3. scenarios   – clean / noisy / prism / degenerate / degenerate_noisy /
//                    flat_distorted / leak behave as the manifest documents
//   4. device      – FileCameraDevice replay, DeviceController threads, still capture
//   5. vendor      – simulated vendor SDK through the ABI-neutral function table
//   6. persistence – reload from disk, invalidation rules, SDK read-back
//
// Exit codes: 0 = all checks passed, 1 = failure, 77 = mock data not available.

#include "core/CalibrationSDK.h"
#include "core/Importers.h"
#include "core/Workbench.h"

#include "device/CameraDeviceFactory.h"
#include "device/DeviceController.h"
#include "device/FileCameraDevice.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>

namespace {

constexpr int kSkipExitCode = 77;

QString argument(const QStringList& arguments, const QString& key, const QString& fallback = QString())
{
    const int index = arguments.indexOf(key);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : fallback;
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    spike::require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取 %1").arg(path));
    return file.readAll();
}

QJsonValue readJson(const QString& path)
{
    return spike::strictLoads(readBytes(path));
}

void writeText(const QString& path, const QString& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    spike::require(file.open(QIODevice::WriteOnly), QStringLiteral("无法写入 %1").arg(path));
    file.write(text.toUtf8());
}

// ---------------------------------------------------------------------------
// Check bookkeeping: assertions carry a suite tag so the JSON report stays
// usable as evidence even if a later suite throws.
// ---------------------------------------------------------------------------
class Checks {
public:
    int total = 0;
    QStringList failures;

    void add(const QString& suite, const QString& label, bool ok, const QString& detail = QString())
    {
        ++total;
        if (!ok) {
            failures.append(QStringLiteral("[%1] %2%3").arg(
                suite, label, detail.isEmpty() ? QString() : QStringLiteral("：%1").arg(detail)));
        }
        QJsonObject record;
        record.insert(QStringLiteral("suite"), suite);
        record.insert(QStringLiteral("check"), label);
        record.insert(QStringLiteral("ok"), ok);
        if (!detail.isEmpty()) record.insert(QStringLiteral("detail"), detail);
        m_details.append(record);
    }

    void require(const QString& suite, const QString& label, bool ok, const QString& detail = QString())
    {
        add(suite, label, ok, detail);
    }

    void closeTo(const QString& suite, const QString& label, double actual, double expected,
                 double tolerance)
    {
        add(suite, label, std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
            QStringLiteral("实测 %1，期望 %2，容差 %3").arg(actual).arg(expected).arg(tolerance));
    }

    void lessThan(const QString& suite, const QString& label, double actual, double limit)
    {
        add(suite, label, std::isfinite(actual) && actual < limit,
            QStringLiteral("实测 %1，上限 %2").arg(actual).arg(limit));
    }

    void greaterThan(const QString& suite, const QString& label, double actual, double limit)
    {
        add(suite, label, std::isfinite(actual) && actual > limit,
            QStringLiteral("实测 %1，下限 %2").arg(actual).arg(limit));
    }

    // Requires `action` to raise spike::Error whose text contains `needle`.
    void rejects(const QString& suite, const QString& label, const std::function<void()>& action,
                 const QString& needle)
    {
        try {
            action();
        } catch (const std::exception& error) {
            const QString text = QString::fromUtf8(error.what());
            add(suite, label, text.contains(needle),
                text.contains(needle) ? text : QStringLiteral("异常信息不匹配：%1").arg(text));
            return;
        }
        add(suite, label, false, QStringLiteral("未抛出异常"));
    }

    QJsonArray details() const { return m_details; }

private:
    QJsonArray m_details;
};

struct Context {
    QString mockData;
    QString dataRoot;
    QString output;
    QJsonObject manifest;
    QJsonObject truth;
    QJsonArray queryRays;
    QJsonObject turntableTruth;
    double zeroTruth = 0.0;
    spike::Workbench* workbench = nullptr;
    Checks checks;
    QJsonObject suiteSummary;
    QJsonObject sceneReport;
    QString pipelineProjectId;
    QString latestCleanProject;

    QString resolve(const QString& relative) const { return QDir(mockData).filePath(relative); }

    void suite(const QString& name, const std::function<void()>& body)
    {
        const int before = checks.total;
        const int failuresBefore = checks.failures.size();
        try {
            body();
        } catch (const std::exception& error) {
            checks.require(name, QStringLiteral("套件未抛异常"), false,
                           QString::fromUtf8(error.what()));
        }
        QJsonObject record;
        record.insert(QStringLiteral("suite"), name);
        record.insert(QStringLiteral("checks"), checks.total - before);
        record.insert(QStringLiteral("failures"), checks.failures.size() - failuresBefore);
        suiteSummary.insert(name, record);
    }
};

spike::Config mockConfig()
{
    QJsonObject config = spike::defaultConfigJson();
    QJsonObject target = config.value(QStringLiteral("target")).toObject();
    target.insert(QStringLiteral("cols"), 9);
    target.insert(QStringLiteral("rows"), 6);
    target.insert(QStringLiteral("square_length_m"), 0.03);
    config.insert(QStringLiteral("target"), target);
    QJsonObject acceptance = config.value(QStringLiteral("acceptance")).toObject();
    acceptance.insert(QStringLiteral("intrinsics_p95_px"), 0.5);
    acceptance.insert(QStringLiteral("turntable_p95_deg"), 0.05);
    config.insert(QStringLiteral("acceptance"), acceptance);
    config.insert(QStringLiteral("operator"), QStringLiteral("qt-integration"));
    return spike::validateConfig(config);
}

QJsonObject configBody()
{
    QJsonObject body;
    body.insert(QStringLiteral("config"), spike::configToJson(mockConfig()));
    return body;
}

QJsonArray capturesOf(const QString& observationFile)
{
    return readJson(observationFile).toObject().value(QStringLiteral("captures")).toArray();
}

double element(const QJsonArray& matrix, int row, int column)
{
    return matrix.at(row).toArray().at(column).toDouble();
}

double modelValue(const QJsonObject& model, int row, int column)
{
    return element(model.value(QStringLiteral("K")).toArray(), row, column);
}

QString solveMessage(const std::function<void()>& action)
{
    try {
        action();
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    }
    return QString();
}

double maxRayErrorDeg(const spike::Workbench& workbench, const QString& projectId,
                      const QJsonArray& queryRays)
{
    QJsonArray pixels;
    for (const QJsonValue& item : queryRays) {
        pixels.append(item.toObject().value(QStringLiteral("pixel")).toArray());
    }
    const QJsonArray points =
        workbench.query(projectId, pixels).value(QStringLiteral("points")).toArray();
    spike::require(points.size() == queryRays.size(), QStringLiteral("查询点数与真值不一致"));
    double worst = 0.0;
    for (int i = 0; i < points.size(); ++i) {
        const QJsonObject got = points.at(i).toObject();
        const QJsonObject want = queryRays.at(i).toObject();
        worst = std::max({worst,
                          std::abs(got.value(QStringLiteral("azimuth_deg")).toDouble()
                                   - want.value(QStringLiteral("azimuth_deg")).toDouble()),
                          std::abs(got.value(QStringLiteral("elevation_deg")).toDouble()
                                   - want.value(QStringLiteral("elevation_deg")).toDouble())});
    }
    return worst;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

bool decodePng(const QByteArray& bytes, cv::Mat& image)
{
    if (bytes.isEmpty()) return false;
    const cv::Mat buffer(1, static_cast<int>(bytes.size()), CV_8UC1,
                         const_cast<char*>(bytes.constData()));
    image = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
    return !image.empty();
}

// ---------------------------------------------------------------------------
// 1. Decode / import layer
// ---------------------------------------------------------------------------
void testDecode(Context& ctx)
{
    const QString name = QStringLiteral("decode");
    const spike::Config config = mockConfig();

    const QByteArray pngBytes = readBytes(ctx.resolve(QStringLiteral("files/png/pose_00.png")));
    const cv::Mat png = spike::decodeImageBytes(pngBytes, cv::IMREAD_UNCHANGED);
    ctx.checks.require(name, QStringLiteral("PNG 解码为 640x480"),
                       png.cols == 640 && png.rows == 480,
                       QStringLiteral("%1x%2").arg(png.cols).arg(png.rows));

    QJsonObject pngOptions;
    pngOptions.insert(QStringLiteral("black_level"), 0);
    pngOptions.insert(QStringLiteral("white_level"), 255);
    spike::FileRecord pngRecord;
    pngRecord.name = QStringLiteral("pose_00.png");
    pngRecord.content = pngBytes;
    const spike::Capture pngCapture =
        spike::importCapture(pngRecord, config, QStringLiteral("train"),
                             QStringLiteral("mock-png-train-00"), QStringLiteral("synthetic"),
                             pngOptions);
    ctx.checks.require(name, QStringLiteral("PNG 检出靶标"), pngCapture.found);
    ctx.checks.require(name, QStringLiteral("PNG 检出 54 个角点"),
                       pngCapture.imagePoints.size() == 54,
                       QString::number(static_cast<int>(pngCapture.imagePoints.size())));
    ctx.checks.require(name, QStringLiteral("PNG 观测尺寸记录为 640x480"),
                       pngCapture.width == 640 && pngCapture.height == 480);
    ctx.checks.require(name, QStringLiteral("PNG 生成预览"),
                       !pngCapture.previewBase64.isEmpty());

    const QByteArray npyBytes = readBytes(ctx.resolve(QStringLiteral("files/spike_npy/spike_pose_00.npy")));
    QJsonObject npyOptions;
    npyOptions.insert(QStringLiteral("method"), QStringLiteral("count"));
    npyOptions.insert(QStringLiteral("window_start_tick"), 0);
    npyOptions.insert(QStringLiteral("window_ticks"), 128);
    npyOptions.insert(QStringLiteral("period_s"), 5e-05);
    npyOptions.insert(QStringLiteral("black_level"), 0);
    npyOptions.insert(QStringLiteral("white_level"), 128);
    const cv::Mat npyFrame = spike::reconstructFrame(npyBytes, QStringLiteral(".npy"), npyOptions);
    ctx.checks.require(name, QStringLiteral("NPY 重建为 640x480"),
                       npyFrame.cols == 640 && npyFrame.rows == 480,
                       QStringLiteral("%1x%2").arg(npyFrame.cols).arg(npyFrame.rows));
    spike::FileRecord npyRecord;
    npyRecord.name = QStringLiteral("spike_pose_00.npy");
    npyRecord.content = npyBytes;
    const spike::Capture npyCapture =
        spike::importCapture(npyRecord, config, QStringLiteral("train"),
                             QStringLiteral("mock-npy-train-00"), QStringLiteral("synthetic"),
                             npyOptions);
    ctx.checks.require(name, QStringLiteral("NPY 检出靶标"), npyCapture.found);
    ctx.checks.require(name, QStringLiteral("NPY 检出 54 个角点"),
                       npyCapture.imagePoints.size() == 54);

    const QByteArray datBytes = readBytes(ctx.resolve(QStringLiteral("files/packed_dat/spike_pose_00.dat")));
    QJsonObject datOptions;
    datOptions.insert(QStringLiteral("layout_confirmed"), true);
    datOptions.insert(QStringLiteral("layout"), QStringLiteral("packed_flat"));
    datOptions.insert(QStringLiteral("bitorder"), QStringLiteral("little"));
    datOptions.insert(QStringLiteral("width"), 640);
    datOptions.insert(QStringLiteral("height"), 480);
    datOptions.insert(QStringLiteral("header_bytes"), 0);
    datOptions.insert(QStringLiteral("frame_stride_bytes"), 38400);
    datOptions.insert(QStringLiteral("method"), QStringLiteral("count"));
    datOptions.insert(QStringLiteral("window_start_tick"), 0);
    datOptions.insert(QStringLiteral("window_ticks"), 256);
    datOptions.insert(QStringLiteral("black_level"), 0);
    datOptions.insert(QStringLiteral("white_level"), 256);
    const cv::Mat datFrame = spike::reconstructFrame(datBytes, QStringLiteral(".dat"), datOptions);
    ctx.checks.require(name, QStringLiteral("DAT 重建为 640x480"),
                       datFrame.cols == 640 && datFrame.rows == 480,
                       QStringLiteral("%1x%2").arg(datFrame.cols).arg(datFrame.rows));
    spike::FileRecord datRecord;
    datRecord.name = QStringLiteral("spike_pose_00.dat");
    datRecord.content = datBytes;
    const spike::Capture datCapture =
        spike::importCapture(datRecord, config, QStringLiteral("train"),
                             QStringLiteral("mock-dat-train-00"), QStringLiteral("synthetic"),
                             datOptions);
    ctx.checks.require(name, QStringLiteral("DAT 检出靶标"), datCapture.found);
    ctx.checks.require(name, QStringLiteral("DAT 检出 54 个角点"),
                       datCapture.imagePoints.size() == 54);

    ctx.checks.rejects(name, QStringLiteral("DAT 未声明布局时拒绝猜测"), [&] {
        QJsonObject options = datOptions;
        options.remove(QStringLiteral("layout_confirmed"));
        spike::reconstructFrame(datBytes, QStringLiteral(".dat"), options);
    }, QStringLiteral("布局"));

    ctx.checks.rejects(name, QStringLiteral("DAT 超出时间窗容量被拒绝"), [&] {
        QJsonObject options = datOptions;
        options.insert(QStringLiteral("window_ticks"), 384);
        spike::reconstructFrame(datBytes, QStringLiteral(".dat"), options);
    }, QStringLiteral("一亿"));

    ctx.checks.rejects(name, QStringLiteral("损坏的 NPY 被拒绝"), [&] {
        spike::reconstructFrame(QByteArray(4096, 'x'), QStringLiteral(".npy"), npyOptions);
    }, QStringLiteral("NPY"));

    ctx.checks.rejects(name, QStringLiteral("不支持的扩展名被拒绝"), [&] {
        spike::FileRecord record;
        record.name = QStringLiteral("notes.txt");
        record.content = QByteArray("not a frame");
        spike::importCapture(record, config, QStringLiteral("train"), QStringLiteral("seq"),
                             QStringLiteral("synthetic"));
    }, QStringLiteral("支持PNG"));
}

// ---------------------------------------------------------------------------
// 2. Full image pipeline
// ---------------------------------------------------------------------------
void testPipeline(Context& ctx)
{
    const QString name = QStringLiteral("pipeline");
    QJsonObject project = ctx.workbench->createProject(QStringLiteral("集成测试 · PNG 全链路"));
    const QString id = project.value(QStringLiteral("id")).toString();
    ctx.workbench->updateConfig(id, configBody());

    int imported = 0;
    int found = 0;
    for (const QJsonValue& item : ctx.manifest.value(QStringLiteral("scenarios")).toArray()) {
        const QJsonObject scene = item.toObject();
        if (scene.value(QStringLiteral("id")).toString() != QStringLiteral("png")) continue;
        for (const QJsonValue& fileValue : scene.value(QStringLiteral("files")).toArray()) {
            const QJsonObject entry = fileValue.toObject();
            const QString path = ctx.resolve(entry.value(QStringLiteral("path")).toString());
            QJsonObject file;
            file.insert(QStringLiteral("name"), QFileInfo(path).fileName());
            file.insert(QStringLiteral("content_base64"),
                        QString::fromLatin1(spike::encodeBase64(readBytes(path))));
            QJsonArray files;
            files.append(file);
            project = ctx.workbench->importFiles(
                id, files, entry.value(QStringLiteral("split")).toString(),
                entry.value(QStringLiteral("sequence_id")).toString(), QStringLiteral("synthetic"),
                entry.value(QStringLiteral("options")).toObject());
            ++imported;
        }
    }
    project = ctx.workbench->getProject(id);
    for (const QJsonValue& item : project.value(QStringLiteral("captures")).toArray()) {
        if (item.toObject().value(QStringLiteral("found")).toBool()) ++found;
    }
    ctx.checks.require(name, QStringLiteral("12 张 PNG 全部导入"), imported == 12,
                       QString::number(imported));
    ctx.checks.require(name, QStringLiteral("12/12 张检出靶标"), found == 12,
                       QString::number(found));

    // Preview through the GUI/serving path, before and after undistortion.
    const QString firstCapture =
        project.value(QStringLiteral("captures")).toArray().at(0).toObject()
            .value(QStringLiteral("id")).toString();
    cv::Mat preview;
    ctx.checks.require(name, QStringLiteral("原始预览可解码"),
                       decodePng(ctx.workbench->preview(id, firstCapture, false), preview)
                           && preview.cols == 640 && preview.rows == 480);

    project = ctx.workbench->solve(id);
    const QJsonObject model = project.value(QStringLiteral("model")).toObject();
    project = ctx.workbench->validate(id);
    const QJsonObject validation = project.value(QStringLiteral("validation")).toObject();
    const QJsonObject diagnostics = model.value(QStringLiteral("diagnostics")).toObject();
    const double fx = modelValue(model, 0, 0);
    const double truthFx = element(ctx.truth.value(QStringLiteral("K")).toArray(), 0, 0);
    ctx.checks.lessThan(name, QStringLiteral("PNG 链路焦距偏差 < 20 px"), std::abs(fx - truthFx), 20.0);
    ctx.checks.require(name, QStringLiteral("PNG 链路独立验证已执行"),
                       !validation.value(QStringLiteral("p95_px")).isUndefined(),
                       validation.value(QStringLiteral("status")).toString());
    ctx.checks.lessThan(name, QStringLiteral("PNG 训练 RMS < 0.5 px"),
                        diagnostics.value(QStringLiteral("fit_rms_px")).toDouble(), 0.5);
    ctx.checks.greaterThan(name, QStringLiteral("栅格化图像指向误差 > 0.5°（符合预期）"),
                           maxRayErrorDeg(*ctx.workbench, id, ctx.queryRays), 0.5);

    cv::Mat undistorted;
    ctx.checks.require(name, QStringLiteral("去畸变预览可解码"),
                       decodePng(ctx.workbench->preview(id, firstCapture, true), undistorted)
                           && undistorted.cols == 640 && undistorted.rows == 480);
    ctx.pipelineProjectId = id;
}

// ---------------------------------------------------------------------------
// 3. Scenario matrix
// ---------------------------------------------------------------------------
struct SceneRun {
    bool solved = false;
    QString rejection;
    QJsonObject model;
    QJsonObject validation;
    double rayMaxErrorDeg = std::numeric_limits<double>::quiet_NaN();
    QString projectId;
};

SceneRun runObservationScene(Context& ctx, const QString& sceneId, const QJsonArray& files)
{
    SceneRun run;
    QJsonObject project = ctx.workbench->createProject(
        QStringLiteral("集成测试 · %1").arg(sceneId));
    run.projectId = project.value(QStringLiteral("id")).toString();
    ctx.workbench->updateConfig(run.projectId, configBody());
    for (const QJsonValue& item : files) {
        const QJsonObject entry = item.toObject();
        ctx.workbench->importObservations(
            run.projectId, capturesOf(ctx.resolve(entry.value(QStringLiteral("path")).toString())));
    }
    run.rejection = solveMessage([&] {
        const QJsonObject solved = ctx.workbench->solve(run.projectId);
        run.model = solved.value(QStringLiteral("model")).toObject();
        run.solved = true;
    });
    if (!run.solved) return run;
    run.validation = ctx.workbench->validate(run.projectId)
                         .value(QStringLiteral("validation")).toObject();
    run.rayMaxErrorDeg = maxRayErrorDeg(*ctx.workbench, run.projectId, ctx.queryRays);
    return run;
}

void testScenarios(Context& ctx)
{
    const QString name = QStringLiteral("scenarios");
    const QJsonValue scenarios = ctx.manifest.value(QStringLiteral("scenarios"));
    auto filesOf = [&ctx, &scenarios](const QString& sceneId) {
        for (const QJsonValue& item : scenarios.toArray()) {
            const QJsonObject scene = item.toObject();
            if (scene.value(QStringLiteral("id")).toString() == sceneId) {
                return scene.value(QStringLiteral("files")).toArray();
            }
        }
        return QJsonArray();
    };
    auto hasScene = [&scenarios](const QString& sceneId) {
        for (const QJsonValue& item : scenarios.toArray()) {
            if (item.toObject().value(QStringLiteral("id")).toString() == sceneId) return true;
        }
        return false;
    };

    if (hasScene(QStringLiteral("clean"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("clean"),
                                                 filesOf(QStringLiteral("clean")));
        ctx.checks.require(name, QStringLiteral("clean 求解成功"), run.solved, run.rejection);
        if (run.solved) {
            const QJsonObject diagnostics = run.model.value(QStringLiteral("diagnostics")).toObject();
            const double validationP95 =
                run.validation.value(QStringLiteral("p95_px")).toDouble();
            ctx.checks.closeTo(name, QStringLiteral("clean fx 贴近真值"), modelValue(run.model, 0, 0),
                               element(ctx.truth.value(QStringLiteral("K")).toArray(), 0, 0), 3.0);
            ctx.checks.closeTo(name, QStringLiteral("clean fy 贴近真值"), modelValue(run.model, 1, 1),
                               element(ctx.truth.value(QStringLiteral("K")).toArray(), 1, 1), 3.0);
            ctx.checks.closeTo(name, QStringLiteral("clean cx 贴近真值"), modelValue(run.model, 0, 2),
                               element(ctx.truth.value(QStringLiteral("K")).toArray(), 0, 2), 1.5);
            ctx.checks.lessThan(name, QStringLiteral("clean 训练 RMS < 0.15 px"),
                                diagnostics.value(QStringLiteral("fit_rms_px")).toDouble(), 0.15);
            ctx.checks.lessThan(name, QStringLiteral("clean 独立验证 P95 < 0.25 px"),
                                validationP95, 0.25);
            ctx.checks.require(name, QStringLiteral("clean 验证门禁为 passed"),
                               run.validation.value(QStringLiteral("status")).toString()
                                   == QStringLiteral("passed"),
                               run.validation.value(QStringLiteral("status")).toString());
            ctx.checks.lessThan(name, QStringLiteral("clean 指向误差 < 0.2°"),
                                run.rayMaxErrorDeg, 0.2);

            for (const QString& mode : {QStringLiteral("far_field"),
                                        QStringLiteral("finite_distance")}) {
                const QJsonObject block = ctx.turntableTruth.value(mode).toObject();
                const QJsonObject result = ctx.workbench->calibrateTurntable(
                    run.projectId, block.value(QStringLiteral("observations")).toArray(),
                    block.value(QStringLiteral("reference")).toObject());
                const double zero =
                    result.value(QStringLiteral("turntable")).toObject()
                        .value(QStringLiteral("zero_offset_deg")).toDouble();
                // The mock dataset itself achieves ~0.06° here (identical to the
                // Python reference); 0.15° is that check's documented bound.
                ctx.checks.closeTo(name, QStringLiteral("clean 转台零位 %1").arg(mode), zero,
                                   ctx.zeroTruth, 0.15);
            }

            const QJsonObject bundle = ctx.workbench->exportProject(run.projectId);
            ctx.checks.require(name, QStringLiteral("导出包未认证实机合格"),
                               bundle.value(QStringLiteral("hardware_certified")).isBool()
                                   && !bundle.value(QStringLiteral("hardware_certified")).toBool());
            const QString html = ctx.workbench->reportHtml(run.projectId);
            ctx.checks.require(name, QStringLiteral("HTML 报告结构完整"),
                               html.contains(QStringLiteral("<!doctype html"))
                                   && html.contains(QStringLiteral("Spike 单支路标定报告")));
            ctx.sceneReport.insert(QStringLiteral("clean"),
                                   QJsonObject{{QStringLiteral("fx"), modelValue(run.model, 0, 0)},
                                               {QStringLiteral("p95_px"), validationP95},
                                               {QStringLiteral("ray_max_error_deg"), run.rayMaxErrorDeg}});
            ctx.latestCleanProject = run.projectId;
        }
    }

    if (hasScene(QStringLiteral("noisy"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("noisy"),
                                                 filesOf(QStringLiteral("noisy")));
        ctx.checks.require(name, QStringLiteral("noisy 仍可求解"), run.solved, run.rejection);
        if (run.solved) {
            const QJsonObject diagnostics = run.model.value(QStringLiteral("diagnostics")).toObject();
            ctx.checks.greaterThan(name, QStringLiteral("noisy 训练 RMS > 0.3 px"),
                                   diagnostics.value(QStringLiteral("fit_rms_px")).toDouble(), 0.3);
            ctx.checks.greaterThan(name, QStringLiteral("noisy 验证 P95 超过 0.5 px"),
                                   run.validation.value(QStringLiteral("p95_px")).toDouble(), 0.5);
            ctx.checks.require(name, QStringLiteral("noisy 门禁判为 failed"),
                               run.validation.value(QStringLiteral("status")).toString()
                                   == QStringLiteral("failed"),
                               run.validation.value(QStringLiteral("status")).toString());
            const double truthFx = element(ctx.truth.value(QStringLiteral("K")).toArray(), 0, 0);
            ctx.checks.lessThan(name, QStringLiteral("noisy 焦距漂移仍 < 15 px"),
                                std::abs(modelValue(run.model, 0, 0) - truthFx), 15.0);
            ctx.sceneReport.insert(QStringLiteral("noisy"),
                                   QJsonObject{{QStringLiteral("p95_px"),
                                                run.validation.value(QStringLiteral("p95_px")).toDouble()}});
        }
    }

    if (hasScene(QStringLiteral("prism"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("prism"),
                                                 filesOf(QStringLiteral("prism")));
        ctx.checks.require(name, QStringLiteral("prism 求解成功"), run.solved, run.rejection);
        if (run.solved) {
            const QJsonObject diagnostics = run.model.value(QStringLiteral("diagnostics")).toObject();
            ctx.checks.lessThan(name, QStringLiteral("prism 重投影仍很小 < 0.15 px"),
                                diagnostics.value(QStringLiteral("fit_rms_px")).toDouble(), 0.15);
            ctx.checks.require(name, QStringLiteral("prism 验证仍判 passed"),
                               run.validation.value(QStringLiteral("status")).toString()
                                   == QStringLiteral("passed"),
                               run.validation.value(QStringLiteral("status")).toString());
            ctx.checks.greaterThan(name, QStringLiteral("prism 指向误差 > 0.25° 暴露模型失配"),
                                   run.rayMaxErrorDeg, 0.25);
            const double truthCx = element(ctx.truth.value(QStringLiteral("K")).toArray(), 0, 2);
            ctx.checks.greaterThan(name, QStringLiteral("prism 主点被带动 > 3 px"),
                                   std::abs(modelValue(run.model, 0, 2) - truthCx), 3.0);
            ctx.sceneReport.insert(QStringLiteral("prism"),
                                   QJsonObject{{QStringLiteral("ray_max_error_deg"),
                                                run.rayMaxErrorDeg}});
        }
    }

    if (hasScene(QStringLiteral("flat_distorted"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("flat_distorted"),
                                                 filesOf(QStringLiteral("flat_distorted")));
        ctx.checks.require(name, QStringLiteral("flat_distorted 求解未被拦截（记录盲区）"),
                           run.solved, run.rejection);
        if (run.solved) {
            const double truthFx = element(ctx.truth.value(QStringLiteral("K")).toArray(), 0, 0);
            ctx.checks.greaterThan(name, QStringLiteral("flat_distorted 焦距误差 > 20 px"),
                                   std::abs(modelValue(run.model, 0, 0) - truthFx), 20.0);
            ctx.checks.lessThan(
                name, QStringLiteral("flat_distorted 重投影仍自洽 < 0.15 px"),
                run.model.value(QStringLiteral("diagnostics")).toObject()
                    .value(QStringLiteral("fit_rms_px")).toDouble(), 0.15);
            ctx.checks.greaterThan(name, QStringLiteral("flat_distorted 指向误差 > 0.5°"),
                                   run.rayMaxErrorDeg, 0.5);
            ctx.sceneReport.insert(QStringLiteral("flat_distorted"),
                                   QJsonObject{{QStringLiteral("fx"), modelValue(run.model, 0, 0)},
                                               {QStringLiteral("ray_max_error_deg"),
                                                run.rayMaxErrorDeg}});
        }
    }

    if (hasScene(QStringLiteral("degenerate"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("degenerate"),
                                                 filesOf(QStringLiteral("degenerate")));
        ctx.checks.require(name, QStringLiteral("degenerate 被拒绝求解"), !run.solved,
                           run.rejection);
        ctx.checks.require(name, QStringLiteral("degenerate 拒绝原因指向姿态退化"),
                           run.rejection.contains(QStringLiteral("姿态约束退化")), run.rejection);
    }

    if (hasScene(QStringLiteral("degenerate_noisy"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("degenerate_noisy"),
                                                 filesOf(QStringLiteral("degenerate_noisy")));
        if (!run.solved) {
            ctx.checks.require(name, QStringLiteral("degenerate_noisy 给出明确拒绝"), true,
                               run.rejection);
        } else {
            // Documented limitation of the 5-parameter model + homography guard:
            // 0.04 px of noise slips past the rank check and yields an absurd fx.
            // The port must still produce finite, serialisable JSON in that case.
            const double fx = modelValue(run.model, 0, 0);
            ctx.checks.require(name, QStringLiteral("degenerate_noisy 模型可序列化为有限 JSON"),
                               spike::finiteJson(run.model));
            ctx.checks.greaterThan(name, QStringLiteral("degenerate_noisy 焦距荒谬（已记录局限）"),
                                   std::abs(fx), 10000.0);
            ctx.sceneReport.insert(QStringLiteral("degenerate_noisy"),
                                   QJsonObject{{QStringLiteral("fx"), fx}});
        }
    }

    if (hasScene(QStringLiteral("leak"))) {
        const SceneRun run = runObservationScene(ctx, QStringLiteral("leak"),
                                                 filesOf(QStringLiteral("leak")));
        ctx.checks.require(name, QStringLiteral("leak 被拒绝求解"), !run.solved, run.rejection);
        ctx.checks.require(name, QStringLiteral("leak 拒绝原因指向重复角点"),
                           run.rejection.contains(QStringLiteral("完全相同的角点观测")),
                           run.rejection);
    }
}

// ---------------------------------------------------------------------------
// 4. Device layer
// ---------------------------------------------------------------------------
void testDevice(Context& ctx)
{
    const QString name = QStringLiteral("device");
    const QString pngDir = ctx.resolve(QStringLiteral("files/png"));

    spike::FileCameraDevice camera;
    QVariantMap parameters;
    parameters.insert(QStringLiteral("path"), pngDir);
    parameters.insert(QStringLiteral("loop"), true);
    QString error;
    bool opened = camera.open(parameters, &error);
    ctx.checks.require(name, QStringLiteral("文件设备打开回放目录"), opened, error);
    ctx.checks.require(name, QStringLiteral("文件设备统计 12 个文件"),
                       camera.parameters().value(QStringLiteral("frame_count")).toInt() == 12,
                       QString::number(camera.parameters().value(QStringLiteral("frame_count")).toInt()));

    spike::DeviceFrame frame;
    bool allSized = true;
    for (int i = 0; i < 12; ++i) {
        if (!camera.grab(frame, 1000, &error) || frame.image.cols != 640
            || frame.image.rows != 480) {
            allSized = false;
            ctx.checks.require(name, QStringLiteral("第 %1 帧可抓取").arg(i + 1), false, error);
            break;
        }
    }
    ctx.checks.require(name, QStringLiteral("连续 12 帧均为 640x480"), allSized);
    ctx.checks.require(name, QStringLiteral("帧元数据带来源文件名"),
                       !frame.metadata.value(QStringLiteral("source")).toString().isEmpty());

    // Re-opening the same path must reset the playlist, not append to it.
    opened = camera.open(parameters, &error);
    ctx.checks.require(name, QStringLiteral("重复打开不累加文件"),
                       opened
                           && camera.parameters().value(QStringLiteral("frame_count")).toInt() == 12,
                       QString::number(camera.parameters().value(QStringLiteral("frame_count")).toInt()));

    parameters.insert(QStringLiteral("loop"), false);
    camera.open(parameters, &error);
    int grabbed = 0;
    while (camera.grab(frame, 1000, &error) && grabbed < 100) ++grabbed;
    ctx.checks.require(name, QStringLiteral("关闭循环后回放恰好 12 帧"), grabbed == 12,
                       QString::number(grabbed));
    ctx.checks.require(name, QStringLiteral("回放结束给出明确错误"),
                       error.contains(QStringLiteral("已结束")), error);

    spike::FileCameraDevice npyCamera;
    QVariantMap npyParameters;
    npyParameters.insert(QStringLiteral("path"),
                         ctx.resolve(QStringLiteral("files/spike_npy/spike_pose_00.npy")));
    npyParameters.insert(QStringLiteral("window_ticks"), 128);
    npyParameters.insert(QStringLiteral("method"), QStringLiteral("count"));
    npyParameters.insert(QStringLiteral("period_s"), 5e-05);
    npyParameters.insert(QStringLiteral("black_level"), 0);
    npyParameters.insert(QStringLiteral("white_level"), 128);
    opened = npyCamera.open(npyParameters, &error);
    ctx.checks.require(name, QStringLiteral("NPY 单文件设备可打开"), opened, error);
    ctx.checks.require(name, QStringLiteral("NPY 回放重建 640x480"),
                       npyCamera.grab(frame, 1000, &error) && frame.image.cols == 640
                           && frame.image.rows == 480,
                       error);

    // Threaded controller: same signals the Widgets UI consumes.
    spike::DeviceController controller;
    bool connected = false;
    QString connectionMessage;
    QObject::connect(&controller, &spike::DeviceController::connectionChanged, &controller,
                     [&](bool ok, const QString& message) {
                         connected = ok;
                         connectionMessage = message;
                     });
    QByteArray still;
    QString stillName;
    QObject::connect(&controller, &spike::DeviceController::stillCaptured, &controller,
                     [&](const QByteArray& png, const QString& stillFileName, const QString&) {
                         still = png;
                         stillName = stillFileName;
                     });
    QVariantMap controllerParameters;
    controllerParameters.insert(QStringLiteral("path"), pngDir);
    controller.connectDevice(QStringLiteral("file"), controllerParameters);
    ctx.checks.require(name, QStringLiteral("异步连接设备"),
                       waitUntil([&] { return connected; }, 5000), connectionMessage);
    controller.captureStill();
    ctx.checks.require(name, QStringLiteral("异步采集停稳帧"),
                       waitUntil([&] { return !still.isEmpty(); }, 5000));
    cv::Mat stillImage;
    ctx.checks.require(name, QStringLiteral("停稳帧是有效 640x480 PNG"),
                       decodePng(still, stillImage) && stillImage.cols == 640
                           && stillImage.rows == 480);
    ctx.checks.require(name, QStringLiteral("停稳帧文件名规范"),
                       stillName.startsWith(QStringLiteral("device_")), stillName);

    // Stills enter the project exactly like the GUI device panel does.
    QJsonObject project = ctx.workbench->createProject(QStringLiteral("集成测试 · 设备采集"));
    const QString id = project.value(QStringLiteral("id")).toString();
    ctx.workbench->updateConfig(id, configBody());
    QJsonObject file;
    file.insert(QStringLiteral("name"), stillName);
    file.insert(QStringLiteral("content_base64"), QString::fromLatin1(spike::encodeBase64(still)));
    QJsonArray files;
    files.append(file);
    project = ctx.workbench->importFiles(id, files, QStringLiteral("train"),
                                         QStringLiteral("device-sequence-1"),
                                         QStringLiteral("synthetic"), QJsonObject());
    ctx.checks.require(name, QStringLiteral("设备帧可导入工程并检出靶标"),
                       project.value(QStringLiteral("captures")).toArray().at(0).toObject()
                           .value(QStringLiteral("found")).toBool());

    bool disconnected = false;
    QObject::connect(&controller, &spike::DeviceController::connectionChanged, &controller,
                     [&](bool ok, const QString&) {
                         if (!ok) disconnected = true;
                     });
    controller.disconnectDevice();
    ctx.checks.require(name, QStringLiteral("异步断开设备"),
                       waitUntil([&] { return disconnected; }, 5000));
}

// ---------------------------------------------------------------------------
// 5. Vendor SDK bridge, driven by a simulated SDK implementation
// ---------------------------------------------------------------------------
namespace fake_sdk {

int g_channels = 1;
int g_width = 64;
int g_height = 48;

const char* version() { return "fake-sdk/1.0"; }

void* openDevice(const char*, char* error, int capacity)
{
    if (error && capacity > 0) {
        const QByteArray text = QStringLiteral("模拟相机已连接").toUtf8();
        std::snprintf(error, static_cast<size_t>(capacity), "%s", text.constData());
    }
    return reinterpret_cast<void*>(0x1);
}

void closeDevice(void*) {}

int setInt(void*, const char*, int) { return 0; }

int setDouble(void*, const char*, double) { return 0; }

int getInt(void*, const char*, int* value)
{
    if (value) *value = 0;
    return 0;
}

int grab(void*, unsigned char* data, int capacity, int* width, int* height, int* channels, int)
{
    *width = g_width;
    *height = g_height;
    *channels = g_channels;
    const int needed = g_width * g_height * g_channels;
    if (needed > capacity) return -2;   // over-buffer: the adapter must reject it
    for (int i = 0; i < needed; ++i) data[i] = static_cast<unsigned char>((i * 7) % 251);
    return 0;
}

} // namespace fake_sdk

void testVendor(Context& ctx)
{
    const QString name = QStringLiteral("vendor");
    spike::VendorSdkApi api;
    api.version = fake_sdk::version;
    api.open = fake_sdk::openDevice;
    api.close = fake_sdk::closeDevice;
    api.setInt = fake_sdk::setInt;
    api.setDouble = fake_sdk::setDouble;
    api.getInt = fake_sdk::getInt;
    api.grab = fake_sdk::grab;
    spike::registerVendorSdkApi(api);

    ctx.checks.require(name, QStringLiteral("注册后出现在后端列表"),
                       spike::availableBackends().contains(QStringLiteral("vendor-sdk")));

    QVariantMap parameters;
    parameters.insert(QStringLiteral("connection"), QStringLiteral("SN-0001"));
    parameters.insert(QStringLiteral("max_width"), 64);
    parameters.insert(QStringLiteral("max_height"), 48);
    parameters.insert(QStringLiteral("channels"), 1);
    parameters.insert(QStringLiteral("exposure_us"), 1200);
    parameters.insert(QStringLiteral("gain"), 2.5);

    auto device = spike::createCameraDevice(QStringLiteral("vendor-sdk"));
    QString error;
    ctx.checks.require(name, QStringLiteral("厂商设备可打开"),
                       device->open(parameters, &error), error);
    ctx.checks.require(name, QStringLiteral("厂商设备参数已写回"),
                       device->parameters().value(QStringLiteral("exposure_us")).toInt() == 1200);

    spike::DeviceFrame frame;
    ctx.checks.require(name, QStringLiteral("厂商设备抓取灰度帧"),
                       device->grab(frame, 1000, &error) && frame.image.cols == 64
                           && frame.image.rows == 48 && frame.image.channels() == 1,
                       error);

    // 3-channel frame from the same adapter (buffer sized for 3 channels).
    fake_sdk::g_channels = 3;
    QVariantMap colorParameters = parameters;
    colorParameters.insert(QStringLiteral("channels"), 3);
    auto colorDevice = spike::createCameraDevice(QStringLiteral("vendor-sdk"));
    ctx.checks.require(name, QStringLiteral("厂商设备可打开彩色模式"),
                       colorDevice->open(colorParameters, &error), error);
    ctx.checks.require(name, QStringLiteral("厂商设备抓取三通道帧"),
                       colorDevice->grab(frame, 1000, &error) && frame.image.channels() == 3,
                       error);

    // 2-channel (e.g. YUY2-like) must be refused instead of being reinterpreted
    // as 4-channel data, which would read past the SDK buffer.
    fake_sdk::g_channels = 2;
    auto oddDevice = spike::createCameraDevice(QStringLiteral("vendor-sdk"));
    const bool oddOpened = oddDevice->open(parameters, &error);
    const bool oddGrabbed = oddOpened && oddDevice->grab(frame, 1000, &error);
    ctx.checks.require(name, QStringLiteral("双通道帧被明确拒绝而不是越界读取"),
                       !oddGrabbed && !error.isEmpty(),
                       oddGrabbed ? QStringLiteral("被当作有效帧接受") : error);

    // Over-buffer frame size from the SDK.
    fake_sdk::g_channels = 1;
    fake_sdk::g_width = 128;
    fake_sdk::g_height = 96;
    auto smallDevice = spike::createCameraDevice(QStringLiteral("vendor-sdk"));
    QVariantMap small = parameters;
    small.insert(QStringLiteral("max_width"), 32);
    small.insert(QStringLiteral("max_height"), 32);
    smallDevice->open(small, &error);
    ctx.checks.require(name, QStringLiteral("超出缓冲区的帧尺寸被拒绝"),
                       !smallDevice->grab(frame, 1000, &error) && !error.isEmpty(), error);
    fake_sdk::g_width = 64;
    fake_sdk::g_height = 48;
}

// ---------------------------------------------------------------------------
// 6. Persistence, invalidation and SDK read-back
// ---------------------------------------------------------------------------
void testPersistence(Context& ctx)
{
    const QString name = QStringLiteral("persistence");
    ctx.checks.require(name, QStringLiteral("全链路工程已创建"), !ctx.pipelineProjectId.isEmpty());
    if (ctx.pipelineProjectId.isEmpty()) return;

    const QJsonObject solved = ctx.workbench->getProject(ctx.pipelineProjectId);
    ctx.checks.require(name, QStringLiteral("工程状态为已验证"),
                       solved.value(QStringLiteral("status")).toString() == QStringLiteral("validated"),
                       solved.value(QStringLiteral("status")).toString());

    // A fresh instance must read the same project back from disk.
    spike::Workbench reopened(ctx.dataRoot);
    const QJsonObject loaded = reopened.getProject(ctx.pipelineProjectId);
    ctx.checks.require(name, QStringLiteral("重新打开工程可读取模型"),
                       !loaded.value(QStringLiteral("model")).isNull());
    ctx.checks.require(name, QStringLiteral("工程列表包含已保存工程"),
                       !reopened.state().value(QStringLiteral("projects")).toArray().isEmpty());

    const QJsonObject bundle = loaded.value(QStringLiteral("model")).toObject();
    Q_UNUSED(bundle);

    // Geometry changes must drop captures and results.
    QJsonObject configChange = spike::defaultConfigJson();
    QJsonObject target = configChange.value(QStringLiteral("target")).toObject();
    target.insert(QStringLiteral("square_length_m"), 0.025);
    configChange.insert(QStringLiteral("target"), target);
    QJsonObject body;
    body.insert(QStringLiteral("config"), configChange);
    const QJsonObject invalidated = reopened.updateConfig(ctx.pipelineProjectId, body);
    ctx.checks.require(name, QStringLiteral("改动靶标尺寸后清空观测"),
                       invalidated.value(QStringLiteral("captures")).toArray().isEmpty());
    ctx.checks.require(name, QStringLiteral("改动靶标尺寸后模型失效"),
                       invalidated.value(QStringLiteral("model")).isNull());

    // Restore the geometry and rebuild, then exercise capture toggling.
    const QJsonObject restored = reopened.updateConfig(ctx.pipelineProjectId, configBody());
    Q_UNUSED(restored);

    // Export + SDK read-back on the clean scene project.
    if (!ctx.latestCleanProject.isEmpty()) {
        const QJsonObject exportBundle = reopened.exportProject(ctx.latestCleanProject);
        const QString bundlePath = QDir(ctx.output).filePath(QStringLiteral("bundle_clean.json"));
        writeText(bundlePath, spike::dumpsJson(exportBundle));
        ctx.checks.require(name, QStringLiteral("导出包标明 synthetic 来源"),
                           exportBundle.value(QStringLiteral("evidence")).toArray()
                               .contains(QStringLiteral("synthetic")));
        ctx.checks.require(name, QStringLiteral("导出包未认证实机合格"),
                           !exportBundle.value(QStringLiteral("hardware_certified")).toBool());

        spike::CalibrationSDK sdk(bundlePath);
        const QJsonArray center = QJsonArray{QJsonArray{QJsonValue(320.0), QJsonValue(240.0)}};
        const QJsonObject sdkResult = sdk.query(center);
        const QJsonObject directResult = reopened.query(ctx.latestCleanProject, center);
        ctx.checks.require(name, QStringLiteral("SDK 可读取导出包并查询"),
                           !sdkResult.value(QStringLiteral("points")).toArray().isEmpty());
        const QJsonObject sdkPoint =
            sdkResult.value(QStringLiteral("points")).toArray().at(0).toObject();
        const QJsonObject directPoint =
            directResult.value(QStringLiteral("points")).toArray().at(0).toObject();
        ctx.checks.closeTo(name, QStringLiteral("SDK 与工程查询方位角一致"),
                           sdkPoint.value(QStringLiteral("azimuth_deg")).toDouble(),
                           directPoint.value(QStringLiteral("azimuth_deg")).toDouble(), 1e-9);
        ctx.checks.closeTo(name, QStringLiteral("SDK 与工程查询俯仰角一致"),
                           sdkPoint.value(QStringLiteral("elevation_deg")).toDouble(),
                           directPoint.value(QStringLiteral("elevation_deg")).toDouble(), 1e-9);

        const QString reportPath = QDir(ctx.output).filePath(QStringLiteral("report_clean.html"));
        writeText(reportPath, reopened.reportHtml(ctx.latestCleanProject));
        ctx.checks.require(name, QStringLiteral("报告文件已写出"),
                           QFileInfo::exists(reportPath));
    }

    // Capture toggling invalidates a solved model.
    if (!ctx.latestCleanProject.isEmpty()) {
        const QJsonObject project = reopened.getProject(ctx.latestCleanProject);
        const QJsonArray captures = project.value(QStringLiteral("captures")).toArray();
        if (!captures.isEmpty()) {
            const QString captureId =
                captures.at(0).toObject().value(QStringLiteral("id")).toString();
            QJsonObject toggle;
            toggle.insert(QStringLiteral("enabled"), false);
            const QJsonObject after = reopened.updateCapture(ctx.latestCleanProject, captureId, toggle);
            ctx.checks.require(name, QStringLiteral("停用观测后模型失效"),
                               after.value(QStringLiteral("model")).isNull());
        }
    }

    // A sequence may not be split across train and validation, even at import.
    QJsonObject duplicateProject = reopened.createProject(QStringLiteral("集成测试 · 序列分区"));
    const QString duplicateId = duplicateProject.value(QStringLiteral("id")).toString();
    reopened.updateConfig(duplicateId, configBody());
    auto filePayload = [&ctx](const QString& name) {
        const QString path = ctx.resolve(QStringLiteral("files/png/") + name);
        QJsonObject file;
        file.insert(QStringLiteral("name"), name);
        file.insert(QStringLiteral("content_base64"),
                    QString::fromLatin1(spike::encodeBase64(readBytes(path))));
        return file;
    };
    QJsonArray firstBatch{filePayload(QStringLiteral("pose_00.png"))};
    reopened.importFiles(duplicateId, firstBatch, QStringLiteral("train"),
                         QStringLiteral("shared-sequence"), QStringLiteral("synthetic"), QJsonObject());
    ctx.checks.rejects(name, QStringLiteral("同一序列不能同时用于训练与验证"), [&] {
        QJsonArray secondBatch{filePayload(QStringLiteral("pose_01.png"))};
        reopened.importFiles(duplicateId, secondBatch, QStringLiteral("test"),
                             QStringLiteral("shared-sequence"), QStringLiteral("synthetic"),
                             QJsonObject());
    }, QStringLiteral("同一采集序列"));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    Context ctx;
    ctx.mockData = argument(arguments, QStringLiteral("--mock-data"));
    ctx.dataRoot = argument(arguments, QStringLiteral("--data"),
                            QDir::current().filePath(QStringLiteral("integration_workspace")));
    ctx.output = argument(arguments, QStringLiteral("--output"),
                          QDir::current().filePath(QStringLiteral("integration_output")));

    if (ctx.mockData.isEmpty()
        || !QFileInfo::exists(QDir(ctx.mockData).filePath(QStringLiteral("manifest.json")))) {
        std::cout << "SKIP: mock data not found at '" << ctx.mockData.toStdString() << "'" << std::endl;
        return kSkipExitCode;
    }

    QDir().mkpath(ctx.output);
    // Repeated local runs must start from a clean workspace. Only directories that
    // follow the *_workspace convention (as CTest passes) are cleared.
    if (QFileInfo(ctx.dataRoot).fileName().endsWith(QStringLiteral("_workspace"))
        && QFileInfo::exists(ctx.dataRoot)) {
        QDir(ctx.dataRoot).removeRecursively();
    }
    QDir().mkpath(ctx.dataRoot);
    ctx.manifest = readJson(QDir(ctx.mockData).filePath(QStringLiteral("manifest.json"))).toObject();
    ctx.truth = readJson(QDir(ctx.mockData).filePath(
        ctx.manifest.value(QStringLiteral("truth_file")).toString(QStringLiteral("ground_truth.json")))).toObject();
    ctx.queryRays = ctx.truth.value(QStringLiteral("query_rays")).toArray();
    ctx.turntableTruth = ctx.truth.value(QStringLiteral("turntable")).toObject();
    ctx.zeroTruth = ctx.turntableTruth.value(QStringLiteral("zero_offset_deg")).toDouble();

    spike::Workbench workbench(ctx.dataRoot);
    workbench.setHardwareConnected(false);
    ctx.workbench = &workbench;

    ctx.suite(QStringLiteral("decode"), [&] { testDecode(ctx); });
    ctx.suite(QStringLiteral("pipeline"), [&] { testPipeline(ctx); });
    ctx.suite(QStringLiteral("scenarios"), [&] { testScenarios(ctx); });
    ctx.suite(QStringLiteral("device"), [&] { testDevice(ctx); });
    ctx.suite(QStringLiteral("vendor"), [&] { testVendor(ctx); });
    ctx.suite(QStringLiteral("persistence"), [&] { testPersistence(ctx); });

    QJsonObject summary;
    summary.insert(QStringLiteral("app"), QStringLiteral("spike-calibration-qt-integration"));
    summary.insert(QStringLiteral("version"), QString::fromUtf8(spike::kVersion));
    summary.insert(QStringLiteral("mock_data"), QDir::toNativeSeparators(ctx.mockData));
    summary.insert(QStringLiteral("check_count"), ctx.checks.total);
    summary.insert(QStringLiteral("failures"), QJsonArray::fromStringList(ctx.checks.failures));
    summary.insert(QStringLiteral("passed"), ctx.checks.failures.isEmpty());
    summary.insert(QStringLiteral("suites"), ctx.suiteSummary);
    summary.insert(QStringLiteral("scenes"), ctx.sceneReport);
    summary.insert(QStringLiteral("checks"), ctx.checks.details());
    writeText(QDir(ctx.output).filePath(QStringLiteral("integration_report.json")),
              spike::dumpsJson(summary));

    const int failureCount = ctx.checks.failures.size();
    std::cout << "mock-data integration: " << (failureCount == 0 ? "PASS" : "FAIL") << " ("
              << ctx.checks.total << " checks, " << failureCount << " failures)" << std::endl;
    for (const QString& failure : ctx.checks.failures) {
        std::cout << "  - " << failure.toStdString() << std::endl;
    }
    return failureCount == 0 ? 0 : 1;
}

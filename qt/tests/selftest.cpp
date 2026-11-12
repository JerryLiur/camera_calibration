#include "core/CalibrationSDK.h"
#include "core/Workbench.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <cmath>
#include <iostream>
#include <vector>

namespace {

QString argument(const QStringList& arguments, const QString& key, const QString& fallback = QString())
{
    const int index = arguments.indexOf(key);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : fallback;
}

QJsonValue loadJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw spike::Error(QStringLiteral("无法读取 %1").arg(path));
    }
    return spike::strictLoads(file.readAll());
}

void writeText(const QString& path, const QString& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw spike::Error(QStringLiteral("无法写入 %1").arg(path));
    }
    file.write(text.toUtf8());
}

struct Checks {
    QStringList failures;
    QJsonArray details;

    void closeTo(double actual, double expected, double tolerance, const QString& label)
    {
        const bool ok = std::abs(actual - expected) <= tolerance;
        if (!ok) {
            failures.append(QStringLiteral("%1：实测 %2，期望 %3（容差 %4）")
                                .arg(label).arg(actual).arg(expected).arg(tolerance));
        }
        QJsonObject record;
        record.insert(QStringLiteral("check"), label);
        record.insert(QStringLiteral("value"), actual);
        record.insert(QStringLiteral("expected"), expected);
        record.insert(QStringLiteral("ok"), ok);
        details.append(record);
    }

    void lessThan(double actual, double limit, const QString& label)
    {
        const bool ok = std::isfinite(actual) && actual < limit;
        if (!ok) failures.append(QStringLiteral("%1：实测 %2，上限 %3").arg(label).arg(actual).arg(limit));
        QJsonObject record;
        record.insert(QStringLiteral("check"), label);
        record.insert(QStringLiteral("value"), actual);
        record.insert(QStringLiteral("limit"), limit);
        record.insert(QStringLiteral("ok"), ok);
        details.append(record);
    }

    void require(bool condition, const QString& label)
    {
        if (!condition) failures.append(label);
        QJsonObject record;
        record.insert(QStringLiteral("check"), label);
        record.insert(QStringLiteral("ok"), condition);
        details.append(record);
    }
};

double element(const QJsonArray& matrix, int row, int column)
{
    return matrix.at(row).toArray().at(column).toDouble();
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    const QString dataRoot = argument(arguments, QStringLiteral("--data"),
                                      QDir::current().filePath(QStringLiteral("selftest_workspace")));
    const QString output = argument(arguments, QStringLiteral("--output"),
                                    QDir::current().filePath(QStringLiteral("selftest_output")));
    const QString observationsDir = argument(arguments, QStringLiteral("--observations"));
    const QString groundTruthPath = argument(arguments, QStringLiteral("--ground-truth"));

    Checks checks;
    QJsonObject summary;
    try {
        QDir().mkpath(output);
        spike::Workbench workbench(dataRoot);

        // 1. Deterministic synthetic pipeline (same fixture as the Python self-test).
        QJsonObject project = workbench.createProject(QStringLiteral("C++ 几何自检"));
        const QString demoId = project.value(QStringLiteral("id")).toString();
        workbench.loadDemo(demoId);
        project = workbench.solve(demoId);
        project = workbench.validate(demoId);
        const QJsonObject model = project.value(QStringLiteral("model")).toObject();
        const QJsonArray K = model.value(QStringLiteral("K")).toArray();
        const QJsonObject validation = project.value(QStringLiteral("validation")).toObject();
        checks.closeTo(element(K, 0, 0), 620.0, 3.0, QStringLiteral("合成夹具 fx"));
        checks.closeTo(element(K, 1, 1), 610.0, 3.0, QStringLiteral("合成夹具 fy"));
        checks.lessThan(model.value(QStringLiteral("diagnostics")).toObject()
                            .value(QStringLiteral("fit_rms_px")).toDouble(),
                        0.2, QStringLiteral("合成夹具拟合 RMS"));
        checks.lessThan(validation.value(QStringLiteral("p95_px")).toDouble(),
                        0.2, QStringLiteral("合成夹具独立验证 P95"));
        checks.require(validation.value(QStringLiteral("status")).toString() == QStringLiteral("measured"),
                       QStringLiteral("未设阈值时应为 measured"));
        const QJsonObject ray = workbench.query(
            demoId, QJsonArray{QJsonArray{QJsonValue(element(K, 0, 2)),
                                          QJsonValue(element(K, 1, 2))}});
        const QJsonObject first = ray.value(QStringLiteral("points")).toArray().at(0).toObject();
        checks.closeTo(first.value(QStringLiteral("azimuth_deg")).toDouble(), 0.0, 0.02,
                       QStringLiteral("主点方位角"));
        checks.closeTo(first.value(QStringLiteral("elevation_deg")).toDouble(), 0.0, 0.02,
                       QStringLiteral("主点俯仰角"));

        const QJsonObject bundle = workbench.exportProject(demoId);
        writeText(QDir(output).filePath(QStringLiteral("calibration.json")),
                  spike::dumpsJson(bundle));
        writeText(QDir(output).filePath(QStringLiteral("report.html")),
                  workbench.reportHtml(demoId));
        spike::CalibrationSDK sdk(QDir(output).filePath(QStringLiteral("calibration.json")));
        const QJsonObject sdkRay = sdk.query(
            QJsonArray{QJsonArray{QJsonValue(element(K, 0, 2)), QJsonValue(element(K, 1, 2))}});
        checks.require(!sdkRay.value(QStringLiteral("points")).toArray().isEmpty(),
                       QStringLiteral("C++ SDK 可读取导出包并查询视线"));

        // 2. Optional cross-check against the repository's independent mock ground truth.
        if (!observationsDir.isEmpty() && !groundTruthPath.isEmpty()) {
            const QJsonObject truth = loadJson(groundTruthPath).toObject();
            const QJsonArray truthK = truth.value(QStringLiteral("K")).toArray();
            const QJsonObject train = loadJson(QDir(observationsDir)
                                                   .filePath(QStringLiteral("observations_train.json"))).toObject();
            const QJsonObject test = loadJson(QDir(observationsDir)
                                                  .filePath(QStringLiteral("observations_test.json"))).toObject();
            QJsonArray captures = train.value(QStringLiteral("captures")).toArray();
            for (const QJsonValue& item : test.value(QStringLiteral("captures")).toArray()) {
                captures.append(item);
            }
            QJsonObject mockProject = workbench.createProject(QStringLiteral("C++ 模拟数据对照"));
            const QString mockId = mockProject.value(QStringLiteral("id")).toString();
            workbench.importObservations(mockId, captures);
            mockProject = workbench.solve(mockId);
            mockProject = workbench.validate(mockId);
            const QJsonObject mockModel = mockProject.value(QStringLiteral("model")).toObject();
            const QJsonArray mockK = mockModel.value(QStringLiteral("K")).toArray();
            checks.closeTo(element(mockK, 0, 0), element(truthK, 0, 0), 5.0, QStringLiteral("模拟数据 fx"));
            checks.closeTo(element(mockK, 1, 1), element(truthK, 1, 1), 5.0, QStringLiteral("模拟数据 fy"));
            checks.closeTo(element(mockK, 0, 2), element(truthK, 0, 2), 5.0, QStringLiteral("模拟数据 cx"));
            checks.closeTo(element(mockK, 1, 2), element(truthK, 1, 2), 5.0, QStringLiteral("模拟数据 cy"));
            checks.lessThan(mockProject.value(QStringLiteral("validation")).toObject()
                                .value(QStringLiteral("p95_px")).toDouble(),
                            0.5, QStringLiteral("模拟数据独立验证 P95"));
            const QJsonArray truthDistortion = truth.value(QStringLiteral("distortion")).toArray();
            const QJsonArray fittedDistortion = mockModel.value(QStringLiteral("distortion")).toArray();
            checks.require(truthDistortion.size() == fittedDistortion.size(),
                           QStringLiteral("畸变参数数量一致"));
            for (int i = 0; i < truthDistortion.size(); ++i) {
                checks.closeTo(fittedDistortion.at(i).toDouble(), truthDistortion.at(i).toDouble(),
                               0.03, QStringLiteral("模拟数据畸变 d%1").arg(i + 1));
            }

            for (const QJsonValue& item : truth.value(QStringLiteral("query_rays")).toArray()) {
                const QJsonObject expected = item.toObject();
                const QJsonArray pixel = expected.value(QStringLiteral("pixel")).toArray();
                const QJsonObject result = workbench.query(mockId, QJsonArray{pixel});
                const QJsonObject point = result.value(QStringLiteral("points")).toArray().at(0).toObject();
                checks.closeTo(point.value(QStringLiteral("azimuth_deg")).toDouble(),
                               expected.value(QStringLiteral("azimuth_deg")).toDouble(), 0.15,
                               QStringLiteral("视线方位角 [%1]").arg(pixel.at(0).toDouble()));
                checks.closeTo(point.value(QStringLiteral("elevation_deg")).toDouble(),
                               expected.value(QStringLiteral("elevation_deg")).toDouble(), 0.15,
                               QStringLiteral("视线俯仰角 [%1]").arg(pixel.at(0).toDouble()));
            }

            const QJsonObject turntableTruth = truth.value(QStringLiteral("turntable")).toObject();
            const double zeroTruth = turntableTruth.value(QStringLiteral("zero_offset_deg")).toDouble();
            for (const QString& mode : {QStringLiteral("far_field"), QStringLiteral("finite_distance")}) {
                const QJsonObject block = turntableTruth.value(mode).toObject();
                const QJsonObject result = workbench.calibrateTurntable(
                    mockId, block.value(QStringLiteral("observations")).toArray(),
                    block.value(QStringLiteral("reference")).toObject());
                checks.closeTo(result.value(QStringLiteral("turntable")).toObject()
                                   .value(QStringLiteral("zero_offset_deg")).toDouble(),
                               zeroTruth, 0.05, QStringLiteral("转台零位 %1").arg(mode));
            }
            writeText(QDir(output).filePath(QStringLiteral("model_mock.json")),
                      spike::dumpsJson(mockModel));
            writeText(QDir(output).filePath(QStringLiteral("validation_mock.json")),
                      spike::dumpsJson(mockProject.value(QStringLiteral("validation"))));
        }

        summary.insert(QStringLiteral("app"), QStringLiteral("spike-calibration-qt-selftest"));
        summary.insert(QStringLiteral("version"), QString::fromUtf8(spike::kVersion));
        summary.insert(QStringLiteral("ground_truth_cross_check"),
                       !observationsDir.isEmpty() && !groundTruthPath.isEmpty());
        summary.insert(QStringLiteral("check_count"), checks.details.size());
        summary.insert(QStringLiteral("passed"), checks.failures.isEmpty());
        QJsonArray failures;
        for (const QString& failure : checks.failures) failures.append(failure);
        summary.insert(QStringLiteral("failures"), failures);
        summary.insert(QStringLiteral("checks"), checks.details);
        writeText(QDir(output).filePath(QStringLiteral("self_test.json")), spike::dumpsJson(summary));
        std::cout << spike::dumpsJson(summary).toStdString() << std::endl;
        return checks.failures.isEmpty() ? 0 : 1;
    } catch (const std::exception& error) {
        QJsonObject failure;
        failure.insert(QStringLiteral("passed"), false);
        failure.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
        std::cerr << spike::dumpsJson(failure).toStdString() << std::endl;
        return 1;
    }
}

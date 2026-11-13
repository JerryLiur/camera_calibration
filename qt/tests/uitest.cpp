// Widgets smoke test: constructs the real MainWindow offscreen, drives refresh
// against a temporary workspace, and verifies it survives stale settings.
//
// This is the part a console self-test cannot cover: panel construction,
// signal wiring, and the "project id from a previous session no longer exists"
// path that a user hits after deleting or moving the workspace directory.

#include "core/Workbench.h"
#include "ui/MainWindow.h"
#include "ui/ConfigPanel.h"
#include "ui/DevicePanel.h"
#include "ui/IntrinsicsPanel.h"
#include "ui/TurntablePanel.h"
#include "ui/CapturePanel.h"
#include "ui/UiHelpers.h"

#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QGroupBox>
#include <QKeyEvent>
#include <QPixmap>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextStream>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>

#include <iostream>

namespace {

int g_total = 0;
QStringList g_failures;

void check(const QString& label, bool ok, const QString& detail = QString())
{
    ++g_total;
    if (!ok) {
        g_failures.append(detail.isEmpty() ? label : QStringLiteral("%1：%2").arg(label, detail));
    }
}

void pump(int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void saveScreenshot(QWidget* window, const QString& directory, const QString& name)
{
    if (directory.isEmpty()) return;
    const QString path = QDir(directory).filePath(name + QStringLiteral(".png"));
    check(QStringLiteral("保存界面截图 %1").arg(name), window->grab().save(path), path);
}

void layoutChecks(spike::ui::MainWindow* window, const QString& screenshotDirectory)
{
    auto* tabs = window->findChild<QTabWidget*>();
    if (!tabs) return;
    const int previousTab = tabs->currentIndex();
    const QSize previousSize = window->size();
    const QStringList names = {QStringLiteral("capture"), QStringLiteral("intrinsics"),
                               QStringLiteral("turntable"), QStringLiteral("config"),
                               QStringLiteral("device")};
    for (const QSize size : {QSize(1440, 940), QSize(1100, 720)}) {
        window->resize(size);
        pump();
        check(QStringLiteral("窗口可缩放至 %1x%2").arg(size.width()).arg(size.height()),
              window->size() == size);
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            pump();
            auto* scroll = qobject_cast<QScrollArea*>(tabs->widget(i));
            if (scroll) {
                scroll->verticalScrollBar()->setValue(0);
                pump();
                check(QStringLiteral("%1x%2 页签 %3 无横向溢出")
                          .arg(size.width()).arg(size.height()).arg(i + 1),
                      scroll->horizontalScrollBar()->maximum() == 0,
                      QStringLiteral("horizontal range=%1, panel=%2, viewport=%3")
                          .arg(scroll->horizontalScrollBar()->maximum())
                          .arg(scroll->widget()->width()).arg(scroll->viewport()->width()));
            }
            if (i == 1) {
                auto* intrinsics = window->findChild<spike::ui::IntrinsicsPanel*>();
                QTableWidget* matrix = nullptr;
                if (intrinsics) {
                    for (auto* table : intrinsics->findChildren<QTableWidget*>()) {
                        if (table->rowCount() == 3 && table->columnCount() == 3) matrix = table;
                    }
                }
                bool readable = matrix && matrix->horizontalScrollBar()->maximum() == 0;
                QString detail;
                if (matrix) {
                    for (int row = 0; row < 3; ++row) {
                        for (int column = 0; column < 3; ++column) {
                            const auto* item = matrix->item(row, column);
                            if (!item) { readable = false; continue; }
                            const int needed = matrix->fontMetrics().horizontalAdvance(item->text()) + 12;
                            if (matrix->columnWidth(column) < needed) {
                                readable = false;
                                detail = QStringLiteral("column %1 width %2 < text %3")
                                             .arg(column).arg(matrix->columnWidth(column)).arg(needed);
                            }
                        }
                    }
                }
                check(QStringLiteral("%1x%2 相机矩阵三列完整可读")
                          .arg(size.width()).arg(size.height()), readable, detail);
            }
            saveScreenshot(window, screenshotDirectory,
                           QStringLiteral("%1-%2x%3")
                               .arg(names.value(i, QString::number(i))).arg(size.width()).arg(size.height()));
        }
    }

    tabs->setCurrentIndex(0);
    pump();
    auto* toggle = window->findChild<QToolButton*>(QStringLiteral("captureAdvancedToggle"));
    auto* advanced = window->findChild<QGroupBox*>(QStringLiteral("captureAdvancedGroup"));
    check(QStringLiteral("采集高级参数提供可聚焦的展开按钮"),
          toggle && advanced && toggle->isCheckable() && toggle->focusPolicy() != Qt::NoFocus);
    if (toggle && advanced) {
        check(QStringLiteral("采集高级参数默认折叠"), !toggle->isChecked() && advanced->isHidden());
        auto* settingsScroll = window->findChild<QScrollArea*>(QStringLiteral("captureSettingsScroll"));
        if (settingsScroll) settingsScroll->ensureWidgetVisible(toggle);
        toggle->setFocus(Qt::TabFocusReason);
        pump();
        const auto pressSpace = [toggle] {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(toggle, &press);
            QApplication::sendEvent(toggle, &release);
            pump();
        };
        pressSpace();
        check(QStringLiteral("键盘空格可展开采集高级参数"), toggle->isChecked() && advanced->isVisible());
        if (settingsScroll) {
            check(QStringLiteral("高级参数展开仍无横向溢出"),
                  settingsScroll->horizontalScrollBar()->maximum() == 0);
        }
        if (settingsScroll) {
            settingsScroll->verticalScrollBar()->setValue(settingsScroll->verticalScrollBar()->maximum());
            pump();
        }
        saveScreenshot(window, screenshotDirectory, QStringLiteral("capture-advanced-1100x720"));
        pressSpace();
        check(QStringLiteral("键盘空格可折叠采集高级参数"), !toggle->isChecked() && advanced->isHidden());
    }
    tabs->setCurrentIndex(previousTab);
    window->resize(previousSize);
    pump();
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        pump();
        QThread::msleep(5);
    }
    return predicate();
}

void regressionChecks(spike::Workbench& workbench, const QJsonObject& solvedProject)
{
    using namespace spike::ui;
    class BackgroundProbe : public ProjectPanel {
    public:
        explicit BackgroundProbe(spike::Workbench* wb) : ProjectPanel(wb) {}
        void setProject(const QJsonObject& project) override { m_project = project; }
        using ProjectPanel::runBackground;
    } probe(&workbench);
    QTimer heartbeat;
    int ticks = 0;
    bool workerOffUi = false;
    bool nestedRan = false;
    bool nestedRejected = false;
    bool busyCloseRejected = false;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        ++ticks;
        if (ticks == 1) {
            if (auto* modal = QApplication::activeModalWidget()) {
                modal->close();
                busyCloseRejected = modal->isVisible();
            }
            nestedRejected = !probe.runBackground(QStringLiteral("重复操作"), [&] { nestedRan = true; }, QString());
        }
    });
    heartbeat.start(5);
    check(QStringLiteral("后台任务完成"), probe.runBackground(QStringLiteral("后台响应回归"), [&] {
        workerOffUi = QThread::currentThread() != qApp->thread();
        QThread::msleep(100);
    }, QStringLiteral("完成")));
    heartbeat.stop();
    check(QStringLiteral("后台任务不占用UI线程且保持事件响应"), workerOffUi && ticks >= 2);
    check(QStringLiteral("后台任务拒绝重入"), nestedRejected && !nestedRan);
    check(QStringLiteral("计算未完成不能关闭进度对话框遗留后台线程"), busyCloseRejected);
    bool backgroundError = false;
    QObject::connect(&probe, &ProjectPanel::notify, [&backgroundError](const QString&, bool error) { backgroundError |= error; });
    check(QStringLiteral("后台异常报告并恢复忙状态"),
          !probe.runBackground(QStringLiteral("异常回归"), [] { throw std::runtime_error("expected"); }, QString())
          && backgroundError && !probe.property("spikeBusy").toBool());

    QLineEdit number;
    for (const QString& input : {QStringLiteral("2147483648"), QStringLiteral("-2147483649"),
                               QStringLiteral("1e100"), QStringLiteral("1.5"), QStringLiteral("nan")}) {
        number.setText(input);
        bool rejected = false;
        try { lineToInt(&number, QStringLiteral("测试整数")); } catch (...) { rejected = true; }
        check(QStringLiteral("拒绝整数越界/非整数 %1").arg(input), rejected);
    }
    number.setText(QStringLiteral("2147483647"));
    check(QStringLiteral("接受 int 最大值"), lineToInt(&number, QStringLiteral("整数")) == 2147483647);

    QTemporaryDir exportDir;
    const QString path = exportDir.filePath(QStringLiteral("标定导出.json"));
    saveFileAtomically(path, QByteArray("first"));
    saveFileAtomically(path, QByteArray("second"));
    QFile saved(path);
    check(QStringLiteral("原子导出支持中文路径及替换"), saved.open(QIODevice::ReadOnly) && saved.readAll() == QByteArray("second"));
    bool failedSave = false;
    try { saveFileAtomically(exportDir.path(), QByteArray("data")); } catch (...) { failedSave = true; }
    check(QStringLiteral("导出写入失败必须报告"), failedSave);

    QJsonObject precisionProject = workbench.createProject(QStringLiteral("精确配置回归"));
    QJsonObject config = precisionProject.value(QStringLiteral("config")).toObject();
    QJsonObject target = config.value(QStringLiteral("target")).toObject();
    target.insert(QStringLiteral("square_length_m"), 0.025123456789);
    config.insert(QStringLiteral("target"), target);
    QJsonObject acceptance = config.value(QStringLiteral("acceptance")).toObject();
    acceptance.insert(QStringLiteral("intrinsics_p95_px"), 0.0000123456789);
    config.insert(QStringLiteral("acceptance"), acceptance);
    const QString preciseId = precisionProject.value(QStringLiteral("id")).toString();
    workbench.updateConfig(preciseId, QJsonObject{{QStringLiteral("config"), config}});
    precisionProject = workbench.loadDemo(preciseId);
    ConfigPanel configPanel(&workbench);
    configPanel.setProject(precisionProject);
    auto* spacing = configPanel.findChild<QLineEdit*>(QStringLiteral("targetSpacing"));
    auto* threshold = configPanel.findChild<QLineEdit*>(QStringLiteral("intrinsicsThreshold"));
    check(QStringLiteral("配置间距浮点精确往返"), spacing && spacing->text().toDouble() == 0.025123456789);
    check(QStringLiteral("配置小阈值精确往返"), threshold && threshold->text().toDouble() == 0.0000123456789);
    bool geometricPrompt = false;
    QTimer::singleShot(0, [&geometricPrompt] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            geometricPrompt = true;
            box->done(QMessageBox::No);
        }
    });
    bool savedConfig = false;
    QObject::connect(&configPanel, &ProjectPanel::projectChanged, [&savedConfig] { savedConfig = true; });
    QMetaObject::invokeMethod(&configPanel, "onSave");
    pump();
    check(QStringLiteral("未修改配置保存不会误报几何变化"), savedConfig && !geometricPrompt);
    check(QStringLiteral("未修改配置保存保留观测"), workbench.getProject(preciseId).value(QStringLiteral("captures"))
          == precisionProject.value(QStringLiteral("captures")));
    spacing->setText(QStringLiteral("0.031"));
    configPanel.setProject(workbench.getProject(preciseId));
    check(QStringLiteral("同工程刷新保留未保存配置"), spacing->text() == QStringLiteral("0.031"));

    QJsonObject splitProject = workbench.createProject(QStringLiteral("用途拒绝回归"));
    QJsonArray sharedSequence;
    for (int i = 0; i < 2; ++i) {
        QJsonObject capture = solvedProject.value(QStringLiteral("captures")).toArray().at(i).toObject();
        capture.insert(QStringLiteral("sequence_id"), QStringLiteral("same-acquisition"));
        sharedSequence.append(capture);
    }
    splitProject = workbench.importObservations(splitProject.value(QStringLiteral("id")).toString(), sharedSequence);
    CapturePanel capturePanel(&workbench);
    capturePanel.setProject(splitProject);
    bool splitRejected = false;
    QObject::connect(&capturePanel, &ProjectPanel::notify, [&splitRejected](const QString&, bool error) { splitRejected |= error; });
    auto* table = capturePanel.findChild<QTableWidget*>();
    auto* split = qobject_cast<QComboBox*>(table->cellWidget(0, 2));
    split->setCurrentIndex(1);
    check(QStringLiteral("同序列跨用途被拒绝"), splitRejected);
    check(QStringLiteral("拒绝后用途下拉恢复实际值"), qobject_cast<QComboBox*>(table->cellWidget(0, 2))->currentData().toString() == QStringLiteral("train"));
    check(QStringLiteral("观测表保留可检查的显示高度"), table->minimumHeight() >= 200);

    IntrinsicsPanel intrinsics(&workbench);
    intrinsics.setProject(solvedProject);
    QMetaObject::invokeMethod(&intrinsics, "onQuery");
    bool queryShown = false;
    for (auto* label : intrinsics.findChildren<QLabel*>()) queryShown |= label->text().startsWith(QStringLiteral("方位角 "));
    check(QStringLiteral("内参页可查询视线"), queryShown);
    intrinsics.findChild<QLineEdit*>(QStringLiteral("queryU"))->setText(QStringLiteral("-1"));
    bool rayAfterEdit = false;
    for (auto* label : intrinsics.findChildren<QLabel*>()) rayAfterEdit |= label->text().startsWith(QStringLiteral("方位角 "));
    check(QStringLiteral("编辑查询参数立即清除旧角度"), !rayAfterEdit);
    bool invalidQueryRejected = false;
    QObject::connect(&intrinsics, &ProjectPanel::notify, [&invalidQueryRejected](const QString&, bool error) { invalidQueryRejected |= error; });
    QMetaObject::invokeMethod(&intrinsics, "onQuery");
    bool rayAfterError = false;
    for (auto* label : intrinsics.findChildren<QLabel*>()) rayAfterError |= label->text().startsWith(QStringLiteral("方位角 "));
    check(QStringLiteral("越界查询拒绝且无旧角度残留"), invalidQueryRejected && !rayAfterError);

    CapturePanel rayPanel(&workbench);
    rayPanel.setProject(solvedProject);
    QMetaObject::invokeMethod(&rayPanel, "onPixelClicked", Q_ARG(QPointF, QPointF(300, 240)));
    bool clickedRay = false;
    for (auto* label : rayPanel.findChildren<QLabel*>()) clickedRay |= label->text().startsWith(QStringLiteral("方位角 "));
    check(QStringLiteral("图像点击可生成视线"), clickedRay);
    QMetaObject::invokeMethod(&rayPanel, "onPixelClicked", Q_ARG(QPointF, QPointF(-1, 240)));
    bool staleClickedRay = false;
    for (auto* label : rayPanel.findChildren<QLabel*>()) staleClickedRay |= label->text().startsWith(QStringLiteral("方位角 "));
    check(QStringLiteral("图像点击失败清除旧视线"), !staleClickedRay);
    QJsonObject other = workbench.createProject(QStringLiteral("隔离工程"));
    intrinsics.setProject(other);
    bool staleQuery = false;
    for (auto* label : intrinsics.findChildren<QLabel*>()) staleQuery |= label->text().startsWith(QStringLiteral("方位角 "));
    check(QStringLiteral("切换工程清除旧视线结果"), !staleQuery);
    for (auto* button : intrinsics.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("计算视线")) check(QStringLiteral("未求解工程禁用查询"), !button->isEnabled());
    }

    TurntablePanel turntable(&workbench);
    QJsonObject fitted = solvedProject;
    QJsonObject reference{{QStringLiteral("mode"), QStringLiteral("finite_distance")},
                         {QStringLiteral("target_position_m"), QJsonArray{1.23456789, 2.0, 3.0}},
                         {QStringLiteral("camera_offset_m"), QJsonArray{0.1, 0.2, 0.3}}};
    QJsonObject fit{{QStringLiteral("reference"), reference}, {QStringLiteral("sign"), 1}};
    fit.insert(QStringLiteral("observations"), QJsonArray{QJsonObject{{QStringLiteral("sequence_id"), QStringLiteral("retained")}}});
    fitted.insert(QStringLiteral("turntable"), fit);
    turntable.setProject(fitted);
    bool restoredFinite = false, restoredObservations = false;
    for (auto* field : turntable.findChildren<QLineEdit*>()) restoredFinite |= field->text().toDouble() == 1.23456789;
    for (auto* field : turntable.findChildren<QPlainTextEdit*>()) {
        if (!field->isReadOnly()) restoredObservations = field->toPlainText().contains(QStringLiteral("retained"));
    }
    check(QStringLiteral("重开恢复有限距离几何"), restoredFinite);
    check(QStringLiteral("重开恢复转台观测"), restoredObservations);
    turntable.setProject(other);
    for (auto* field : turntable.findChildren<QPlainTextEdit*>()) {
        if (!field->isReadOnly()) check(QStringLiteral("切换工程清空上个工程转台观测"), field->toPlainText().isEmpty());
    }

    QTemporaryDir replayDir;
    QImage blank(320, 240, QImage::Format_Grayscale8);
    blank.fill(128);
    blank.save(replayDir.filePath(QStringLiteral("frame.png")));
    spike::DeviceController controller;
    DevicePanel device(&workbench, &controller);
    device.setProject(other);
    device.findChild<QLineEdit*>(QStringLiteral("deviceConnection"))->setText(replayDir.path());
    auto* exposure = device.findChild<QLineEdit*>(QStringLiteral("exposureUs"));
    int errors = 0;
    QObject::connect(&device, &ProjectPanel::notify, [&errors](const QString&, bool error) { if (error) ++errors; });
    exposure->setText(QStringLiteral("invalid"));
    QMetaObject::invokeMethod(&device, "onConnect");
    check(QStringLiteral("非法曝光在连接前拒绝"), errors == 1);
    exposure->clear();
    bool connected = false;
    QObject::connect(&controller, &spike::DeviceController::connectionChanged,
                     [&connected](bool state, const QString&) { connected = state; });
    QMetaObject::invokeMethod(&device, "onConnect");
    check(QStringLiteral("回放设备可连接"), waitFor([&] { return connected; }));
    auto* evidence = device.findChild<QComboBox*>(QStringLiteral("deviceEvidence"));
    check(QStringLiteral("离线回放可明确标记为合成"), evidence->currentData().toString() == QStringLiteral("synthetic"));
    auto* sequence = device.findChild<QLineEdit*>(QStringLiteral("deviceSequence"));
    sequence->setText(QStringLiteral("requested-sequence"));
    const QString originalId = other.value(QStringLiteral("id")).toString();
    const QString switchedId = solvedProject.value(QStringLiteral("id")).toString();
    const int switchedCount = workbench.getProject(switchedId).value(QStringLiteral("captures")).toArray().size();
    QMetaObject::invokeMethod(&device, "onCaptureStill");
    device.setProject(solvedProject);
    sequence->setText(QStringLiteral("changed-after-request"));
    check(QStringLiteral("异步采集写入请求时工程"), waitFor([&] {
        return workbench.getProject(originalId).value(QStringLiteral("captures")).toArray().size() == 1;
    }));
    const QJsonArray captures = workbench.getProject(originalId).value(QStringLiteral("captures")).toArray();
    check(QStringLiteral("异步采集保留请求时序列与来源"), !captures.isEmpty()
        && captures.at(0).toObject().value(QStringLiteral("sequence_id")).toString() == QStringLiteral("requested-sequence")
        && captures.at(0).toObject().value(QStringLiteral("evidence")).toString() == QStringLiteral("synthetic"));
    check(QStringLiteral("异步采集不污染切换后的工程"), workbench.getProject(switchedId).value(QStringLiteral("captures")).toArray().size() == switchedCount);
    device.setProject(workbench.getProject(originalId));
    const int previousErrors = errors;
    QMetaObject::invokeMethod(&device, "onCaptureStill");
    device.setProperty("spikeBusy", true);
    check(QStringLiteral("计算繁忙时迟到采集明确提示重新采集"), waitFor([&] { return errors > previousErrors; }));
    device.setProperty("spikeBusy", false);
    check(QStringLiteral("计算繁忙时迟到采集不写入工程"), workbench.getProject(originalId).value(QStringLiteral("captures")).toArray().size() == 1);
    QMetaObject::invokeMethod(&device, "onDisconnect");
    check(QStringLiteral("回放设备可断开"), waitFor([&] { return !connected; }));

    const QString datPath = replayDir.filePath(QStringLiteral("declared_layout.dat"));
    QFile dat(datPath);
    check(QStringLiteral("生成位打包 DAT 回放夹具"), dat.open(QIODevice::WriteOnly) && dat.write(QByteArray(32, char(0x55))) == 32);
    dat.close();
    device.findChild<QLineEdit*>(QStringLiteral("deviceConnection"))->setText(datPath);
    const int beforeDatError = errors;
    QMetaObject::invokeMethod(&device, "onConnect");
    check(QStringLiteral("DAT 回放未声明布局在连接前拒绝"), errors == beforeDatError + 1);
    device.setReplayOptionsProvider([] {
        return QJsonObject{{QStringLiteral("layout_confirmed"), true},
                           {QStringLiteral("layout"), QStringLiteral("row_packed")},
                           {QStringLiteral("bitorder"), QStringLiteral("little")},
                           {QStringLiteral("width"), 8}, {QStringLiteral("height"), 8},
                           {QStringLiteral("window_ticks"), 4}};
    });
    device.findChild<QCheckBox*>(QStringLiteral("useReplayOptions"))->setChecked(true);
    QMetaObject::invokeMethod(&device, "onConnect");
    check(QStringLiteral("DAT 显式配置后可连接"), waitFor([&] { return connected; }));
    bool datFrame = false;
    QObject::connect(&controller, &spike::DeviceController::frameReady,
                     [&datFrame](const QImage& frame, quint64, const QString&) { datFrame |= frame.size() == QSize(8, 8); });
    QMetaObject::invokeMethod(&device, "onStartPreview");
    check(QStringLiteral("DAT 显式配置后生成8x8预览帧"), waitFor([&] { return datFrame; }));
    QMetaObject::invokeMethod(&device, "onDisconnect");
    check(QStringLiteral("DAT 回放线程可停止与断开"), waitFor([&] { return !connected; }));
}

QString settingsValue()
{
    QSettings settings(QStringLiteral("SpikeCalibration"), QStringLiteral("QtWorkbench"));
    return settings.value(QStringLiteral("lastProject")).toString();
}

void setSettingsValue(const QString& value)
{
    QSettings settings(QStringLiteral("SpikeCalibration"), QStringLiteral("QtWorkbench"));
    if (value.isEmpty()) settings.remove(QStringLiteral("lastProject"));
    else settings.setValue(QStringLiteral("lastProject"), value);
    settings.sync();
}

} // namespace

int main(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    }
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setApplicationName(QStringLiteral("SpikeCalibrationUiTest"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("screenshots"), QStringLiteral("保存真实界面离屏截图"),
                      QStringLiteral("directory")});
    parser.process(app);
    const QString screenshotDirectory = parser.value(QStringLiteral("screenshots"));
    if (!screenshotDirectory.isEmpty() && !QDir().mkpath(screenshotDirectory)) {
        std::cerr << "Cannot create screenshot directory: " << screenshotDirectory.toStdString() << std::endl;
        return 1;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    const QString root = workspace.path();

    const QString previousSetting = settingsValue();

    // 1. A stale project id from a previous session must not abort startup.
    setSettingsValue(QStringLiteral("project-that-no-longer-exists"));
    spike::Workbench workbench(root);
    bool constructed = false;
    QString constructionError;
    std::unique_ptr<spike::ui::MainWindow> window;
    try {
        window = std::make_unique<spike::ui::MainWindow>(&workbench);
        constructed = true;
    } catch (const std::exception& error) {
        constructionError = QString::fromUtf8(error.what());
    }
    check(QStringLiteral("残留工程 ID 时窗口仍可构造"), constructed, constructionError);
    if (!constructed) {
        setSettingsValue(previousSetting);
        std::cout << "ui smoke: FAIL (1/" << g_total << ")" << std::endl;
        return 1;
    }

    window->show();
    pump();
    check(QStringLiteral("主窗口可见"), window->isVisible());

    auto* tabs = window->findChild<QTabWidget*>();
    check(QStringLiteral("找到五个功能页"), tabs != nullptr && tabs->count() == 5,
          tabs ? QString::number(tabs->count()) : QStringLiteral("缺少 QTabWidget"));
    if (tabs) {
        const QStringList expected = {QStringLiteral("01 采集与检查"),
                                      QStringLiteral("02 内参与畸变"),
                                      QStringLiteral("03 转台关系"),
                                      QStringLiteral("04 工程配置"),
                                      QStringLiteral("05 设备连接与控制")};
        for (int i = 0; i < expected.size() && i < tabs->count(); ++i) {
            check(QStringLiteral("页签 %1 标题正确").arg(i + 1),
                  tabs->tabText(i) == expected.at(i), tabs->tabText(i));
            check(QStringLiteral("页签 %1 可滚动防止输入框裁切").arg(i + 1),
                  qobject_cast<QScrollArea*>(tabs->widget(i)) != nullptr);
        }
    }

    auto* combo = window->findChild<QComboBox*>();
    check(QStringLiteral("找到工程下拉框"), combo != nullptr);
    check(QStringLiteral("无工程时显示占位项"),
          combo && combo->count() == 1
              && combo->itemText(0) == QStringLiteral("尚未创建工程"),
          combo ? combo->itemText(0) : QStringLiteral("缺少 QComboBox"));
    check(QStringLiteral("状态栏存在"), window->statusBar() != nullptr);

    auto* exportButton = window->findChild<QPushButton*>(QStringLiteral("exportCalibration"));
    auto* reportButton = window->findChild<QPushButton*>(QStringLiteral("reportCalibration"));
    check(QStringLiteral("无工程时禁用导出标定包"), exportButton && !exportButton->isEnabled());
    check(QStringLiteral("无工程时禁用报告导出"), reportButton && !reportButton->isEnabled());
    for (const QSize size : {QSize(1440, 940), QSize(1100, 720)}) {
        window->resize(size);
        pump();
        saveScreenshot(window.get(), screenshotDirectory,
                       QStringLiteral("empty-%1x%2").arg(size.width()).arg(size.height()));
    }

    // 2. Creating a project and refreshing lists it and selects it.
    QJsonObject project = workbench.createProject(QStringLiteral("UI 冒烟工程"));
    project = workbench.loadDemo(project.value(QStringLiteral("id")).toString());
    project = workbench.solve(project.value(QStringLiteral("id")).toString());
    project = workbench.validate(project.value(QStringLiteral("id")).toString());
    const QString projectId = project.value(QStringLiteral("id")).toString();
    QMetaObject::invokeMethod(window.get(), "refresh");
    pump();
    // The placeholder only exists while the workspace has no project, so one
    // project means exactly one combo entry, already selected.
    check(QStringLiteral("刷新后列出工程"), combo && combo->count() == 1,
          combo ? QString::number(combo->count()) : QString());
    check(QStringLiteral("工程名显示在标题栏选择器"),
          combo && combo->currentText() == QStringLiteral("UI 冒烟工程"),
          combo ? combo->currentText() : QString());
    check(QStringLiteral("当前工程自动选中"), combo && combo->currentData().toString() == projectId,
          combo ? combo->currentData().toString() : QString());

    check(QStringLiteral("已求解工程启用标定包与报告导出"),
          exportButton && reportButton && exportButton->isEnabled() && reportButton->isEnabled());
    layoutChecks(window.get(), screenshotDirectory);

    // 3. Startup with a valid setting restores that project.
    setSettingsValue(projectId);
    spike::ui::MainWindow restored(&workbench);
    restored.show();
    pump();
    auto* restoredCombo = restored.findChild<QComboBox*>();
    check(QStringLiteral("启动时恢复上次工程"),
          restoredCombo && restoredCombo->currentData().toString() == projectId,
          restoredCombo ? restoredCombo->currentData().toString() : QString());
    restored.close();

    regressionChecks(workbench, project);

    // 4. Closing the window tears the acquisition thread down cleanly.
    window->close();
    window.reset();
    pump();
    check(QStringLiteral("窗口销毁无异常"), true);

    setSettingsValue(previousSetting);

    std::cout << "ui smoke: " << (g_failures.isEmpty() ? "PASS" : "FAIL") << " (" << g_total
              << " checks, " << g_failures.size() << " failures)" << std::endl;
    for (const QString& failure : g_failures) {
        std::cout << "  - " << failure.toStdString() << std::endl;
    }
    return g_failures.isEmpty() ? 0 : 1;
}

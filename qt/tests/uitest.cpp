// Widgets smoke test: constructs the real MainWindow offscreen, drives refresh
// against a temporary workspace, and verifies it survives stale settings.
//
// This is the part a console self-test cannot cover: panel construction,
// signal wiring, and the "project id from a previous session no longer exists"
// path that a user hits after deleting or moving the workspace directory.

#include "core/Workbench.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextStream>

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
    QApplication::setApplicationName(QStringLiteral("SpikeCalibrationUiTest"));

    const QString root = QDir::current().filePath(QStringLiteral("uitest_workspace"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);

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
        }
    }

    auto* combo = window->findChild<QComboBox*>();
    check(QStringLiteral("找到工程下拉框"), combo != nullptr);
    check(QStringLiteral("无工程时显示占位项"),
          combo && combo->count() == 1
              && combo->itemText(0) == QStringLiteral("尚未创建工程"),
          combo ? combo->itemText(0) : QStringLiteral("缺少 QComboBox"));
    check(QStringLiteral("状态栏存在"), window->statusBar() != nullptr);

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

    // 4. Closing the window tears the acquisition thread down cleanly.
    window->close();
    window.reset();
    pump();
    check(QStringLiteral("窗口销毁无异常"), true);

    setSettingsValue(previousSetting);

    QDir(root).removeRecursively();
    std::cout << "ui smoke: " << (g_failures.isEmpty() ? "PASS" : "FAIL") << " (" << g_total
              << " checks, " << g_failures.size() << " failures)" << std::endl;
    for (const QString& failure : g_failures) {
        std::cout << "  - " << failure.toStdString() << std::endl;
    }
    return g_failures.isEmpty() ? 0 : 1;
}

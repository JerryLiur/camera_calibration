#include "MainWindow.h"

#include "CapturePanel.h"
#include "ConfigPanel.h"
#include "DevicePanel.h"
#include "IntrinsicsPanel.h"
#include "TurntablePanel.h"
#include "UiHelpers.h"

#include "core/Workbench.h"
#include "device/DeviceController.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace spike {
namespace ui {

MainWindow::MainWindow(Workbench* workbench, QWidget* parent)
    : QMainWindow(parent), m_workbench(workbench)
{
    setWindowTitle(QStringLiteral("Spike 标定工具 · Qt 6 / C++ / OpenCV"));
    resize(1440, 940);
    setMinimumSize(1100, 720);
    m_controller = new DeviceController(this);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Spike 标定工具"), central);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:650;color:#1256c0;"));
    auto* subtitle = new QLabel(QStringLiteral("共用镜头 ／ 1:1 分光 ／ Spike 单支路"), central);
    subtitle->setStyleSheet(QStringLiteral("color:#536177;"));
    header->addWidget(title);
    header->addWidget(subtitle);
    header->addStretch(1);
    root->addLayout(header);

    auto* bar = new QHBoxLayout();
    m_projectSelect = new QComboBox(central);
    m_projectSelect->setMinimumWidth(280);
    auto* newProject = makeButton(QStringLiteral("新建工程"), central, true);
    auto* refreshButton = makeButton(QStringLiteral("刷新"), central);
    auto* exportButton = makeButton(QStringLiteral("导出标定包"), central);
    auto* reportButton = makeButton(QStringLiteral("查看报告"), central);
    bar->addWidget(new QLabel(QStringLiteral("当前工程"), central));
    bar->addWidget(m_projectSelect, 1);
    bar->addWidget(newProject);
    bar->addWidget(refreshButton);
    bar->addWidget(exportButton);
    bar->addWidget(reportButton);
    root->addLayout(bar);

    m_tabs = new QTabWidget(central);
    m_capture = new CapturePanel(m_workbench, m_tabs);
    m_intrinsics = new IntrinsicsPanel(m_workbench, m_tabs);
    m_turntable = new TurntablePanel(m_workbench, m_tabs);
    m_config = new ConfigPanel(m_workbench, m_tabs);
    m_device = new DevicePanel(m_workbench, m_controller, m_tabs);
    m_tabs->addTab(m_capture, QStringLiteral("01 采集与检查"));
    m_tabs->addTab(m_intrinsics, QStringLiteral("02 内参与畸变"));
    m_tabs->addTab(m_turntable, QStringLiteral("03 转台关系"));
    m_tabs->addTab(m_config, QStringLiteral("04 工程配置"));
    m_tabs->addTab(m_device, QStringLiteral("05 设备连接与控制"));
    root->addWidget(m_tabs, 1);
    setCentralWidget(central);

    m_status = new QLabel(QStringLiteral("本机工程已就绪"), this);
    m_revision = new QLabel(QStringLiteral("原始 Spike 保持不变"), this);
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_revision);

    const QList<ProjectPanel*> panels = {m_capture, m_intrinsics, m_turntable, m_config, m_device};
    for (ProjectPanel* panel : panels) {
        connect(panel, &ProjectPanel::projectChanged, this, &MainWindow::refresh);
        connect(panel, &ProjectPanel::notify, this, &MainWindow::onNotify);
    }
    connect(m_device, &ProjectPanel::deviceConnectionChanged,
            this, &MainWindow::onDeviceConnection);
    connect(newProject, &QPushButton::clicked, this, &MainWindow::onNewProject);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refresh);
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::onExport);
    connect(reportButton, &QPushButton::clicked, this, &MainWindow::onReport);
    connect(m_projectSelect, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProjectSelected);

    setStyleSheet(QStringLiteral(
        "QMainWindow,QWidget{background:#f3f5f8;color:#202b3c;font-family:'Microsoft YaHei','Segoe UI',sans-serif;}"
        "QGroupBox,QTabWidget::pane{background:#ffffff;border:1px solid #d8e0ea;}"
        "QTabBar::tab{background:#eef2f7;padding:10px 18px;border:1px solid #d8e0ea;}"
        "QTabBar::tab:selected{background:#ffffff;color:#1256c0;font-weight:600;}"
        "QLineEdit,QComboBox,QPlainTextEdit,QTableWidget{background:#f7f9fc;border:1px solid #9caabd;"
        "border-radius:4px;padding:5px;}"
        "QPushButton{background:#ffffff;border:1px solid #9caabd;border-radius:4px;padding:6px 12px;}"
        "QPushButton:hover{border-color:#1256c0;color:#1256c0;}"
        "QPushButton:disabled{color:#8794a5;background:#e9edf3;}"));

    QSettings settings(QStringLiteral("SpikeCalibration"), QStringLiteral("QtWorkbench"));
    m_currentId = settings.value(QStringLiteral("lastProject")).toString();
    refresh();
}

void MainWindow::applyProject(const QJsonObject& project)
{
    m_project = project;
    m_capture->setProject(project);
    m_intrinsics->setProject(project);
    m_turntable->setProject(project);
    m_config->setProject(project);
    m_device->setProject(project);
    if (project.isEmpty()) {
        m_status->setText(QStringLiteral("本机服务已连接 · 新建工程开始标定"));
        m_revision->setText(QStringLiteral("原始 Spike 保持不变"));
        return;
    }
    m_currentId = project.value(QStringLiteral("id")).toString();
    QSettings settings(QStringLiteral("SpikeCalibration"), QStringLiteral("QtWorkbench"));
    settings.setValue(QStringLiteral("lastProject"), m_currentId);
    m_status->setText(QStringLiteral("%1 · %2")
                          .arg(project.value(QStringLiteral("name")).toString(),
                               statusText(project.value(QStringLiteral("status")).toString())));
    m_revision->setText(QStringLiteral("修订 %1 · 本机保存")
                            .arg(project.value(QStringLiteral("revision")).toInt()));
}

void MainWindow::refresh()
{
    const QJsonObject state = m_workbench->state();
    const QJsonArray projects = state.value(QStringLiteral("projects")).toArray();
    QSignalBlocker blocker(m_projectSelect);
    m_projectSelect->clear();
    if (projects.isEmpty()) m_projectSelect->addItem(QStringLiteral("尚未创建工程"), QString());
    for (const QJsonValue& item : projects) {
        const QJsonObject brief = item.toObject();
        m_projectSelect->addItem(brief.value(QStringLiteral("name")).toString(),
                                 brief.value(QStringLiteral("id")).toString());
    }
    QString chosen = m_currentId;
    if (chosen.isEmpty()) chosen = projects.isEmpty() ? QString()
        : projects.at(0).toObject().value(QStringLiteral("id")).toString();
    const int index = m_projectSelect->findData(chosen);
    m_projectSelect->setCurrentIndex(index >= 0 ? index : 0);
    if (chosen.isEmpty()) {
        applyProject(QJsonObject());
    } else {
        try {
            applyProject(m_workbench->getProject(chosen));
        } catch (const std::exception& error) {
            // The id remembered by QSettings can outlive its file (workspace moved
            // or cleaned). Falling back keeps startup working instead of throwing.
            m_currentId.clear();
            const QString fallback = projects.isEmpty()
                                         ? QString()
                                         : projects.at(0).toObject()
                                               .value(QStringLiteral("id")).toString();
            if (fallback.isEmpty() || fallback == chosen) {
                applyProject(QJsonObject());
            } else {
                m_currentId = fallback;
                const int fallbackIndex = m_projectSelect->findData(fallback);
                if (fallbackIndex >= 0) m_projectSelect->setCurrentIndex(fallbackIndex);
                applyProject(m_workbench->getProject(fallback));
            }
            onNotify(QStringLiteral("上次打开的工程不可用，已重新选择：%1")
                         .arg(QString::fromUtf8(error.what())));
        }
    }
    for (const QJsonValue& item : state.value(QStringLiteral("load_errors")).toArray()) {
        onNotify(QStringLiteral("工程读取提示：%1").arg(item.toString()), true);
    }
}

void MainWindow::onProjectSelected(int index)
{
    const QString id = m_projectSelect->itemData(index).toString();
    if (id.isEmpty() || id == m_currentId) return;
    m_currentId = id;
    try {
        applyProject(m_workbench->getProject(id));
    } catch (const std::exception& error) {
        onNotify(QString::fromUtf8(error.what()), true);
    }
}

void MainWindow::onNewProject()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建标定工程"),
                                               QStringLiteral("工程名称"), QLineEdit::Normal,
                                               QStringLiteral("Spike 标定工程"), &accepted);
    if (!accepted || name.trimmed().isEmpty()) return;
    try {
        const QJsonObject project = m_workbench->createProject(name);
        m_currentId = project.value(QStringLiteral("id")).toString();
        refresh();
        m_tabs->setCurrentWidget(m_config);
        onNotify(QStringLiteral("工程已创建。请核验标定板尺寸、方向和光路配置。"));
    } catch (const std::exception& error) {
        onNotify(QString::fromUtf8(error.what()), true);
    }
}

void MainWindow::onExport()
{
    if (m_currentId.isEmpty()) { onNotify(QStringLiteral("请先新建工程。"), true); return; }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出标定包"),
                                                      QStringLiteral("spike-calibration.json"),
                                                      QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    try {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法写入文件"));
        file.write(dumpsJson(m_workbench->exportProject(m_currentId)).toUtf8());
        onNotify(QStringLiteral("标定包已保存：%1").arg(path));
    } catch (const std::exception& error) {
        onNotify(QString::fromUtf8(error.what()), true);
    }
}

void MainWindow::onReport()
{
    if (m_currentId.isEmpty()) { onNotify(QStringLiteral("请先新建工程。"), true); return; }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存标定报告"),
                                                      QStringLiteral("spike-report.html"),
                                                      QStringLiteral("HTML (*.html)"));
    if (path.isEmpty()) return;
    try {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法写入文件"));
        file.write(m_workbench->reportHtml(m_currentId).toUtf8());
        onNotify(QStringLiteral("报告已保存：%1，可用浏览器打开或打印。").arg(path));
    } catch (const std::exception& error) {
        onNotify(QString::fromUtf8(error.what()), true);
    }
}

void MainWindow::onNotify(const QString& text, bool error)
{
    m_status->setText(text);
    statusBar()->showMessage(text, 8000);
    if (error) QMessageBox::warning(this, QStringLiteral("操作未完成"), text);
}

void MainWindow::onDeviceConnection(bool connected)
{
    m_workbench->setHardwareConnected(connected);
    m_revision->setText(connected ? QStringLiteral("设备已连接 · C++ SDK")
                                  : QStringLiteral("离线数据模式"));
}

} // namespace ui
} // namespace spike

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
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QShortcut>
#include <QIcon>
#include <QFormLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>

namespace spike {
namespace ui {

MainWindow::MainWindow(Workbench* workbench, QWidget* parent)
    : QMainWindow(parent), m_workbench(workbench)
{
    setWindowTitle(QStringLiteral("Spike 标定工作台"));
    resize(1440, 940);
    setMinimumSize(1100, 720);
    m_controller = new DeviceController(this);

    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("workbenchRoot"));
    QPalette palette = central->palette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#f2f5fa")));
    setPalette(palette);
    central->setPalette(palette);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* headerWidget = new QWidget(central);
    headerWidget->setObjectName(QStringLiteral("workspaceHeader"));
    auto* headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(24, 16, 24, 16);
    headerLayout->setSpacing(16);
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    auto* mark = new QLabel(headerWidget);
    mark->setFixedSize(44, 44);
    mark->setPixmap(QIcon(QStringLiteral(":/app/icon.png")).pixmap(44, 44));
    auto* brand = new QVBoxLayout();
    brand->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("Spike 标定工作台"), headerWidget);
    title->setObjectName(QStringLiteral("brandTitle"));
    auto* subtitle = new QLabel(QStringLiteral("共用镜头 · 1:1 分光 · 单支路成像标定"), headerWidget);
    subtitle->setObjectName(QStringLiteral("brandSubtitle"));
    brand->addWidget(title);
    brand->addWidget(subtitle);
    header->addWidget(mark);
    header->addLayout(brand);
    header->addStretch(1);
    m_projectState = new QLabel(QStringLiteral("尚无工程"), headerWidget);
    m_projectState->setObjectName(QStringLiteral("projectState"));
    m_connectionState = new QLabel(QStringLiteral("离线数据模式"), headerWidget);
    m_connectionState->setObjectName(QStringLiteral("connectionState"));
    header->addWidget(m_projectState);
    header->addWidget(m_connectionState);
    headerLayout->addLayout(header);

    auto* bar = new QHBoxLayout();
    m_projectSelect = new QComboBox(central);
    m_projectSelect->setObjectName(QStringLiteral("projectSelect"));
    m_projectSelect->setAccessibleName(QStringLiteral("当前工程"));
    m_projectSelect->setMinimumWidth(220);
    m_projectSelect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_projectSelect->setMinimumContentsLength(14);
    auto* newProject = makeButton(QStringLiteral("新建工程"), central);
    newProject->setShortcut(QKeySequence::New);
    newProject->setToolTip(QStringLiteral("新建标定工程（Ctrl+N）"));
    auto* refreshButton = makeButton(QStringLiteral("刷新"), central);
    refreshButton->setShortcut(QKeySequence::Refresh);
    refreshButton->setToolTip(QStringLiteral("重新载入工程（F5）"));
    m_exportButton = makeButton(QStringLiteral("导出标定包"), central);
    m_exportButton->setObjectName(QStringLiteral("exportCalibration"));
    m_reportButton = makeButton(QStringLiteral("导出报告"), central);
    m_reportButton->setObjectName(QStringLiteral("reportCalibration"));
    bar->setSpacing(8);
    bar->addWidget(new QLabel(QStringLiteral("当前工程"), central));
    bar->addWidget(m_projectSelect, 1);
    bar->addWidget(newProject);
    bar->addWidget(refreshButton);
    bar->addSpacing(12);
    bar->addWidget(m_exportButton);
    bar->addWidget(m_reportButton);
    headerLayout->addLayout(bar);
    root->addWidget(headerWidget);

    m_tabs = new QTabWidget(central);
    m_tabs->setObjectName(QStringLiteral("workflowTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setExpanding(false);
    m_capture = new CapturePanel(m_workbench, m_tabs);
    m_intrinsics = new IntrinsicsPanel(m_workbench, m_tabs);
    m_turntable = new TurntablePanel(m_workbench, m_tabs);
    m_config = new ConfigPanel(m_workbench, m_tabs);
    m_device = new DevicePanel(m_workbench, m_controller, m_tabs);
    m_device->setReplayOptionsProvider([this] { return m_capture->replayOptions(); });
    const auto addScrollableTab = [this](QWidget* panel, const QString& label) {
        panel->layout()->setContentsMargins(16, 12, 16, 16);
        panel->layout()->setSpacing(16);
        auto* scroll = new QScrollArea(m_tabs);
        QPalette panelPalette = panel->palette();
        panelPalette.setColor(QPalette::Window, QColor(QStringLiteral("#f2f5fa")));
        panel->setPalette(panelPalette);
        scroll->setPalette(panelPalette);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(panel);
        m_tabs->addTab(scroll, label);
        const int index = m_tabs->count() - 1;
        m_tabs->setTabToolTip(index, label + QStringLiteral("（Alt+%1）").arg(index + 1));
        auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(index + 1)), this);
        connect(shortcut, &QShortcut::activated, this, [this, index] { m_tabs->setCurrentIndex(index); });
    };
    addScrollableTab(m_capture, QStringLiteral("01 采集与检查"));
    addScrollableTab(m_intrinsics, QStringLiteral("02 内参与畸变"));
    addScrollableTab(m_turntable, QStringLiteral("03 转台关系"));
    addScrollableTab(m_config, QStringLiteral("04 工程配置"));
    addScrollableTab(m_device, QStringLiteral("05 设备连接与控制"));
    auto* content = new QVBoxLayout();
    content->setContentsMargins(8, 4, 8, 0);
    content->addWidget(m_tabs);
    root->addLayout(content, 1);
    setCentralWidget(central);

    m_status = new QLabel(QStringLiteral("本机工程已就绪"), this);
    m_status->setMinimumWidth(0);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::onExport);
    connect(m_reportButton, &QPushButton::clicked, this, &MainWindow::onReport);
    connect(m_projectSelect, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProjectSelected);

    setStyleSheet(workbenchStyleSheet());
    for (auto* table : findChildren<QTableWidget*>()) {
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
    }

    QSettings settings(QStringLiteral("SpikeCalibration"), QStringLiteral("QtWorkbench"));
    m_currentId = settings.value(QStringLiteral("lastProject")).toString();
    refresh();
}

void MainWindow::applyProject(const QJsonObject& project)
{
    m_project = project;
    statusBar()->clearMessage();
    m_capture->setProject(project);
    m_intrinsics->setProject(project);
    m_turntable->setProject(project);
    m_config->setProject(project);
    m_device->setProject(project);
    m_exportButton->setEnabled(!project.isEmpty());
    m_reportButton->setEnabled(!project.isEmpty());
    m_projectState->setText(project.isEmpty() ? QStringLiteral("尚无工程")
        : statusText(project.value(QStringLiteral("status")).toString()));
    m_projectSelect->setToolTip(project.value(QStringLiteral("name")).toString());
    if (project.isEmpty()) {
        m_status->setText(QStringLiteral("新建工程，核验标定板配置后开始采集"));
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
        m_tabs->setCurrentIndex(3);
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
        const QByteArray data = dumpsJson(m_workbench->exportProject(m_currentId)).toUtf8();
        saveFileAtomically(path, data);
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
        const QByteArray data = m_workbench->reportHtml(m_currentId).toUtf8();
        saveFileAtomically(path, data);
        onNotify(QStringLiteral("报告已保存：%1，可用浏览器打开或打印。").arg(path));
    } catch (const std::exception& error) {
        onNotify(QString::fromUtf8(error.what()), true);
    }
}

void MainWindow::onNotify(const QString& text, bool error)
{
    m_status->setText(text);
    m_status->setToolTip(text);
    if (error) QMessageBox::warning(this, QStringLiteral("操作未完成"), text);
}

void MainWindow::onDeviceConnection(bool connected)
{
    m_workbench->setHardwareConnected(connected);
    m_connectionState->setText(connected ? QStringLiteral("设备已连接")
                                  : QStringLiteral("离线数据模式"));
}

} // namespace ui
} // namespace spike

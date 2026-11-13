#include "DevicePanel.h"

#include "device/CameraDeviceFactory.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace spike {
namespace ui {

DevicePanel::DevicePanel(Workbench* workbench, DeviceController* controller, QWidget* parent)
    : ProjectPanel(workbench, parent), m_controller(controller)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(16);
    auto* settings = new QWidget(this);
    auto* left = new QVBoxLayout(settings);
    left->setContentsMargins(0, 0, 8, 0);
    left->setSpacing(16);
    auto* connectGroup = makeGroup(QStringLiteral("连接设备"), settings);
    auto* form = new QFormLayout(connectGroup);
    form->setContentsMargins(16, 20, 16, 16);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_backend = new QComboBox(connectGroup);
    for (const QString& backend : availableBackends()) {
        m_backend->addItem(backend == QStringLiteral("file")
                               ? QStringLiteral("离线文件回放（无需 SDK）")
                               : QStringLiteral("相机厂商 C++ SDK"), backend);
    }
    if (!hasVendorSdk()) {
        m_backend->addItem(QStringLiteral("厂商 SDK（未注册适配器）"), QStringLiteral("vendor-sdk"));
    }
    m_connection = new QLineEdit(connectGroup);
    m_connection->setObjectName(QStringLiteral("deviceConnection"));
    m_connection->setAccessibleName(QStringLiteral("连接串或回放目录路径"));
    m_connection->setPlaceholderText(QStringLiteral("序列号 / 设备地址，或回放目录路径"));
    auto* pathRow = new QHBoxLayout();
    pathRow->addWidget(m_connection, 1);
    auto* browse = makeButton(QStringLiteral("浏览…"), connectGroup);
    pathRow->addWidget(browse);
    auto* parameters = new QWidget(connectGroup);
    auto* parameterForm = new QFormLayout(parameters);
    parameterForm->setContentsMargins(0, 8, 0, 0);
    parameterForm->setHorizontalSpacing(12);
    parameterForm->setVerticalSpacing(10);
    parameterForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_maxWidth = new QLineEdit(QStringLiteral("4096"), parameters);
    m_maxHeight = new QLineEdit(QStringLiteral("3000"), parameters);
    m_channels = new QLineEdit(QStringLiteral("1"), parameters);
    m_exposure = new QLineEdit(parameters);
    m_exposure->setObjectName(QStringLiteral("exposureUs"));
    m_exposure->setPlaceholderText(QStringLiteral("留空使用设备默认值"));
    m_gain = new QLineEdit(parameters);
    m_gain->setObjectName(QStringLiteral("deviceGain"));
    m_trigger = new QLineEdit(parameters);
    addRow(form, QStringLiteral("设备后端"), m_backend);
    auto* connectionLabel = new QLabel(QStringLiteral("连接串 / 回放路径"), connectGroup);
    connectionLabel->setBuddy(m_connection);
    form->addRow(connectionLabel);
    form->addRow(pathRow);
    m_useReplayOptions = new QCheckBox(QStringLiteral("使用采集页的 Spike / DAT 参数"), connectGroup);
    m_useReplayOptions->setObjectName(QStringLiteral("useReplayOptions"));
    form->addRow(m_useReplayOptions);
    auto* replayHelp = new QLabel(QStringLiteral("DAT/BIN：先到“采集与检查”的高级参数中核验位布局及宽高，再勾选此项。NPY 可复用同一重建窗口。"), connectGroup);
    replayHelp->setWordWrap(true);
    replayHelp->setStyleSheet(QStringLiteral("color:#536177;"));
    form->addRow(replayHelp);
    addRow(parameterForm, QStringLiteral("最大帧宽 / px"), m_maxWidth);
    addRow(parameterForm, QStringLiteral("最大帧高 / px"), m_maxHeight);
    addRow(parameterForm, QStringLiteral("通道数"), m_channels);
    addRow(parameterForm, QStringLiteral("曝光 / μs"), m_exposure);
    addRow(parameterForm, QStringLiteral("增益"), m_gain);
    addRow(parameterForm, QStringLiteral("触发模式"), m_trigger);
    m_trigger->setToolTip(QStringLiteral("填写厂商 SDK 定义的整数模式编号；留空保留设备默认值。"));
    m_apply = makeButton(QStringLiteral("下发采集参数"), parameters);
    parameterForm->addRow(m_apply);
    auto* parameterToggle = new QToolButton(connectGroup);
    parameterToggle->setObjectName(QStringLiteral("deviceParametersToggle"));
    parameterToggle->setText(QStringLiteral("帧格式与采集参数"));
    parameterToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    parameterToggle->setArrowType(Qt::RightArrow);
    parameterToggle->setCheckable(true);
    parameterToggle->setFocusPolicy(Qt::StrongFocus);
    parameterToggle->setMinimumHeight(36);
    parameterToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(parameterToggle);
    form->addRow(parameters);
    parameters->hide();
    connect(parameterToggle, &QToolButton::toggled, this, [parameters, parameterToggle](bool expanded) {
        parameters->setVisible(expanded);
        parameterToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    auto* buttons = new QHBoxLayout();
    m_connect = makeButton(QStringLiteral("连接设备"), connectGroup, true);
    m_disconnect = makeButton(QStringLiteral("断开"), connectGroup);
    buttons->addWidget(m_connect);
    buttons->addWidget(m_disconnect);
    form->addRow(buttons);
    m_status = new QLabel(QStringLiteral("离线 · 未连接设备"), connectGroup);
    m_status->setWordWrap(true);
    form->addRow(m_status);
    left->addWidget(connectGroup);

    auto* captureGroup = makeGroup(QStringLiteral("保存为标定观测"), settings);
    auto* captureForm = new QFormLayout(captureGroup);
    captureForm->setContentsMargins(16, 20, 16, 16);
    captureForm->setHorizontalSpacing(12);
    captureForm->setVerticalSpacing(10);
    captureForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_sequence = new QLineEdit(captureGroup);
    m_sequence->setObjectName(QStringLiteral("deviceSequence"));
    m_sequence->setPlaceholderText(QStringLiteral("例如 hardware-capture-01"));
    m_split = new QComboBox(captureGroup);
    m_split->addItem(QStringLiteral("训练 · 求解内参"), QStringLiteral("train"));
    m_split->addItem(QStringLiteral("验证 · 不参与求解"), QStringLiteral("test"));
    m_evidence = new QComboBox(captureGroup);
    m_evidence->setObjectName(QStringLiteral("deviceEvidence"));
    m_evidence->addItem(QStringLiteral("本机相机实采"), QStringLiteral("hardware"));
    m_evidence->addItem(QStringLiteral("公开数据"), QStringLiteral("public"));
    m_evidence->addItem(QStringLiteral("合成测试数据"), QStringLiteral("synthetic"));
    m_evidence->setCurrentIndex(2);
    addRow(captureForm, QStringLiteral("采集序列 ID"), m_sequence);
    addRow(captureForm, QStringLiteral("数据用途"), m_split);
    addRow(captureForm, QStringLiteral("数据来源"), m_evidence);
    m_capture = makeButton(QStringLiteral("采集当前帧并加入工程"), captureGroup, true);
    captureForm->addRow(m_capture);
    auto* captureHelp = new QLabel(QStringLiteral(
        "按当前工程的旋转、镜像和标定板参数检测角点，原始脉冲保持不变。"), captureGroup);
    captureHelp->setWordWrap(true);
    captureHelp->setStyleSheet(QStringLiteral("color:#536177;"));
    captureForm->addRow(captureHelp);
    left->addWidget(captureGroup);
    left->addStretch(1);
    auto* settingsScroll = new QScrollArea(this);
    settingsScroll->setObjectName(QStringLiteral("deviceSettingsScroll"));
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setMinimumWidth(390);
    settingsScroll->setWidget(settings);
    root->addWidget(settingsScroll, 2);

    auto* right = new QVBoxLayout();
    auto* previewGroup = makeGroup(QStringLiteral("实时预览"), this);
    auto* previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(16, 20, 16, 16);
    previewLayout->setSpacing(12);
    auto* previewActions = new QHBoxLayout();
    previewActions->addStretch(1);
    m_start = makeButton(QStringLiteral("开始预览"), previewGroup);
    m_stop = makeButton(QStringLiteral("停止预览"), previewGroup);
    previewActions->addWidget(m_start);
    previewActions->addWidget(m_stop);
    previewLayout->addLayout(previewActions);
    m_preview = new ImageViewer(previewGroup);
    m_preview->setObjectName(QStringLiteral("devicePreview"));
    m_preview->setAccessibleName(QStringLiteral("设备实时图像预览"));
    m_preview->setEmptyText(QStringLiteral("等待设备图像"), QStringLiteral("连接设备后，点击“开始预览”"));
    previewLayout->addWidget(m_preview, 1);
    m_frameInfo = new QLabel(QStringLiteral("无帧数据"), previewGroup);
    m_frameInfo->setWordWrap(true);
    m_frameInfo->setMinimumWidth(0);
    m_frameInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_frameInfo->setStyleSheet(QStringLiteral("color:#536177;"));
    previewLayout->addWidget(m_frameInfo);
    right->addWidget(previewGroup, 1);
    root->addLayout(right, 3);

    connect(browse, &QPushButton::clicked, this, &DevicePanel::onBrowse);
    connect(m_connect, &QPushButton::clicked, this, &DevicePanel::onConnect);
    connect(m_disconnect, &QPushButton::clicked, this, &DevicePanel::onDisconnect);
    connect(m_start, &QPushButton::clicked, this, &DevicePanel::onStartPreview);
    connect(m_stop, &QPushButton::clicked, this, &DevicePanel::onStopPreview);
    connect(m_apply, &QPushButton::clicked, this, &DevicePanel::onApplyParameters);
    connect(m_backend, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        m_evidence->setCurrentIndex(m_backend->currentData().toString() == QStringLiteral("file") ? 2 : 0);
    });
    connect(m_capture, &QPushButton::clicked, this, &DevicePanel::onCaptureStill);
    connect(m_controller, &DeviceController::frameReady, this, &DevicePanel::onFrame);
    connect(m_controller, &DeviceController::connectionChanged, this, &DevicePanel::onConnection);
    connect(m_controller, &DeviceController::stillCaptured, this, &DevicePanel::onStill);
    connect(m_controller, &DeviceController::errorOccurred, this, &DevicePanel::onError);
    onConnection(false, QStringLiteral("离线 · 未连接设备"));
}

QVariantMap DevicePanel::connectionParameters() const
{
    QVariantMap parameters;
    const QString connection = m_connection->text().trimmed();
    parameters.insert(QStringLiteral("connection"), connection);
    parameters.insert(QStringLiteral("path"), connection);
    parameters.insert(QStringLiteral("loop"), true);
    if (m_backend->currentData().toString() == QStringLiteral("file")) {
        if (m_useReplayOptions->isChecked()) {
            if (!m_replayOptionsProvider) fail(QStringLiteral("未找到 Spike 回放参数，请在工作台中配置。"));
            parameters.insert(m_replayOptionsProvider().toVariantMap());
        }
        const QFileInfo file(connection);
        const bool hasDat = file.suffix().compare(QStringLiteral("dat"), Qt::CaseInsensitive) == 0
            || file.suffix().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0
            || (file.isDir() && !QDir(connection).entryList({QStringLiteral("*.dat"), QStringLiteral("*.bin")}, QDir::Files).isEmpty());
        if (hasDat && !parameters.value(QStringLiteral("layout_confirmed")).toBool()) {
            fail(QStringLiteral("DAT/BIN 回放需要在 01 页核验真实位布局，并勾选使用该页重建 / DAT 参数。"));
        }
    }
    parameters.insert(QStringLiteral("max_width"), lineToInt(m_maxWidth, QStringLiteral("最大帧宽")));
    parameters.insert(QStringLiteral("max_height"), lineToInt(m_maxHeight, QStringLiteral("最大帧高")));
    parameters.insert(QStringLiteral("channels"), lineToInt(m_channels, QStringLiteral("通道数")));
    if (parameters.value(QStringLiteral("max_width")).toInt() <= 0
        || parameters.value(QStringLiteral("max_height")).toInt() <= 0
        || !QList<int>{1, 3, 4}.contains(parameters.value(QStringLiteral("channels")).toInt())) {
        fail(QStringLiteral("帧宽高必须为正整数，通道数必须为 1、3 或 4。"));
    }
    parameters.insert(acquisitionParameters());
    return parameters;
}

QVariantMap DevicePanel::acquisitionParameters() const
{
    QVariantMap parameters;
    for (const QPair<QLineEdit*, QString>& pair : {
             qMakePair(m_exposure, QStringLiteral("exposure_us")),
             qMakePair(m_gain, QStringLiteral("gain")),
             qMakePair(m_trigger, QStringLiteral("trigger_mode"))}) {
        if (!pair.first->text().trimmed().isEmpty()) {
            if (pair.first == m_trigger) {
                const int value = lineToInt(pair.first, pair.second);
                parameters.insert(pair.second, value);
            } else {
                const double value = lineToNumber(pair.first, pair.second);
                if (pair.first == m_exposure && value <= 0) {
                    fail(QStringLiteral("曝光必须为正数。"));
                }
                parameters.insert(pair.second, value);
            }
        }
    }
    return parameters;
}

void DevicePanel::onBrowse()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择离线回放目录"));
    if (!path.isEmpty()) m_connection->setText(path);
}

void DevicePanel::onConnect()
{
    if (m_backend->currentData().toString() == QStringLiteral("vendor-sdk") && !hasVendorSdk()) {
        emit notify(QStringLiteral("厂商 SDK 适配器尚未注册，请先在启动时调用 registerVendorSdkApi()。"), true);
        return;
    }
    try {
        const QVariantMap parameters = connectionParameters();
        m_connect->setEnabled(false);
        m_controller->connectDevice(m_backend->currentData().toString(), parameters);
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
    }
}

void DevicePanel::onDisconnect() { m_controller->disconnectDevice(); }

void DevicePanel::onStartPreview() { m_controller->startPreview(100); }

void DevicePanel::onStopPreview() { m_controller->stopPreview(); }

void DevicePanel::onApplyParameters()
{
    if (!m_connected) { emit notify(QStringLiteral("请先连接设备。"), true); return; }
    try {
        const QVariantMap parameters = acquisitionParameters();
        if (parameters.isEmpty()) { emit notify(QStringLiteral("请填写要下发的参数。"), true); return; }
        for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) {
            m_controller->setParameter(it.key(), it.value());
        }
        emit notify(QStringLiteral("参数请求已发送，设备拒绝时将报告原因。"));
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
    }
}

void DevicePanel::onCaptureStill()
{
    if (!m_connected) { emit notify(QStringLiteral("请先连接设备。"), true); return; }
    if (!hasProject()) { emit notify(QStringLiteral("请先新建工程。"), true); return; }
    if (!m_pendingProject.isEmpty()) return;
    if (m_sequence->text().trimmed().isEmpty()) {
        emit notify(QStringLiteral("请填写采集序列 ID。"), true);
        return;
    }
    m_pendingProject = projectId();
    m_pendingConfig = m_project.value(QStringLiteral("config")).toObject();
    m_pendingSequence = m_sequence->text().trimmed();
    m_pendingSplit = m_split->currentData().toString();
    m_pendingEvidence = m_evidence->currentData().toString();
    m_capture->setEnabled(false);
    m_controller->captureStill();
}

void DevicePanel::onFrame(const QImage& image, quint64 index, const QString& source)
{
    m_lastFrame = image;
    m_preview->setImage(image);
    m_preview->setPoints({}, image.size());
    m_frameInfo->setText(QStringLiteral("帧 %1 · %2×%3 · %4")
                             .arg(index).arg(image.width()).arg(image.height()).arg(source));
}

void DevicePanel::onConnection(bool connected, const QString& message)
{
    m_connected = connected;
    m_status->setText(message);
    m_disconnect->setEnabled(connected);
    m_start->setEnabled(connected);
    m_stop->setEnabled(connected);
    if (!connected) {
        m_pendingProject.clear();
        m_lastFrame = QImage();
        m_preview->clear();
        m_frameInfo->setText(QStringLiteral("无帧数据"));
    }
    m_capture->setEnabled(connected && hasProject() && m_pendingProject.isEmpty());
    m_apply->setEnabled(connected);
    m_backend->setEnabled(!connected);
    m_useReplayOptions->setEnabled(!connected);
    m_connection->setEnabled(!connected);
    m_connect->setEnabled(!connected);
    emit deviceConnectionChanged(connected && m_backend->currentData().toString() == QStringLiteral("vendor-sdk"));
}

void DevicePanel::onStill(const QByteArray& png, const QString& name, const QString&)
{
    if (m_pendingProject.isEmpty()) return;
    if (window()->property("spikeBusy").toBool()) {
        m_pendingProject.clear();
        m_capture->setEnabled(m_connected && hasProject());
        emit notify(QStringLiteral("当前计算尚未完成，本次采集帧未导入；请等待计算完成后重新采集。"), true);
        return;
    }
    const QString requestedProject = m_pendingProject;
    m_pendingProject.clear();
    m_capture->setEnabled(m_connected && hasProject());
    runBackground(QStringLiteral("正在导入采集帧并检测角点…"),
                  [workbench = m_workbench, png, name, requestedProject, config = m_pendingConfig,
                   split = m_pendingSplit, sequence = m_pendingSequence, evidence = m_pendingEvidence] {
        if (workbench->getProject(requestedProject).value(QStringLiteral("config")).toObject() != config) {
            fail(QStringLiteral("采集期间工程配置已改变，请重新采集。"));
        }
        QJsonObject record;
        record.insert(QStringLiteral("name"), name);
        record.insert(QStringLiteral("content_base64"), QString::fromLatin1(png.toBase64()));
        workbench->importFiles(requestedProject, QJsonArray{record}, split, sequence, evidence, QJsonObject());
    }, QStringLiteral("已把采集帧加入请求时工程：%1").arg(name));
}

void DevicePanel::onError(const QString& message)
{
    m_pendingProject.clear();
    m_capture->setEnabled(m_connected && hasProject());
    emit notify(message, true);
}

void DevicePanel::setProject(const QJsonObject& project)
{
    m_project = project;
    m_capture->setEnabled(m_connected && hasProject() && m_pendingProject.isEmpty());
}

} // namespace ui
} // namespace spike

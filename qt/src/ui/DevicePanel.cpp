#include "DevicePanel.h"

#include "device/CameraDeviceFactory.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPixmap>
#include <QVBoxLayout>

namespace spike {
namespace ui {

DevicePanel::DevicePanel(Workbench* workbench, DeviceController* controller, QWidget* parent)
    : ProjectPanel(workbench, parent), m_controller(controller)
{
    auto* root = new QHBoxLayout(this);
    auto* left = new QVBoxLayout();
    auto* connectGroup = makeGroup(QStringLiteral("设备连接与控制"), this);
    auto* form = new QFormLayout(connectGroup);
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
    m_connection->setPlaceholderText(QStringLiteral("序列号 / 设备地址，或回放目录路径"));
    auto* pathRow = new QHBoxLayout();
    pathRow->addWidget(m_connection, 1);
    auto* browse = makeButton(QStringLiteral("浏览…"), connectGroup);
    pathRow->addWidget(browse);
    m_maxWidth = new QLineEdit(QStringLiteral("4096"), connectGroup);
    m_maxHeight = new QLineEdit(QStringLiteral("3000"), connectGroup);
    m_channels = new QLineEdit(QStringLiteral("1"), connectGroup);
    m_exposure = new QLineEdit(connectGroup);
    m_exposure->setPlaceholderText(QStringLiteral("曝光 / μs，SDK 未实现时忽略"));
    m_gain = new QLineEdit(connectGroup);
    m_trigger = new QLineEdit(connectGroup);
    addRow(form, QStringLiteral("设备后端"), m_backend);
    form->addRow(QStringLiteral("连接串 / 回放路径"), pathRow);
    addRow(form, QStringLiteral("最大帧宽"), m_maxWidth);
    addRow(form, QStringLiteral("最大帧高"), m_maxHeight);
    addRow(form, QStringLiteral("通道数"), m_channels);
    addRow(form, QStringLiteral("exposure_us"), m_exposure);
    addRow(form, QStringLiteral("gain"), m_gain);
    addRow(form, QStringLiteral("trigger_mode"), m_trigger);
    auto* buttons = new QHBoxLayout();
    m_connect = makeButton(QStringLiteral("连接设备"), connectGroup, true);
    m_disconnect = makeButton(QStringLiteral("断开"), connectGroup);
    m_start = makeButton(QStringLiteral("开始预览"), connectGroup);
    m_stop = makeButton(QStringLiteral("停止预览"), connectGroup);
    buttons->addWidget(m_connect);
    buttons->addWidget(m_disconnect);
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    form->addRow(buttons);
    m_status = new QLabel(QStringLiteral("离线 · 未连接设备"), connectGroup);
    m_status->setWordWrap(true);
    form->addRow(m_status);
    left->addWidget(connectGroup);

    auto* captureGroup = makeGroup(QStringLiteral("在线采集为标定观测"), this);
    auto* captureForm = new QFormLayout(captureGroup);
    m_sequence = new QLineEdit(captureGroup);
    m_sequence->setPlaceholderText(QStringLiteral("例如 hardware-capture-01"));
    m_split = new QComboBox(captureGroup);
    m_split->addItem(QStringLiteral("训练 · 求解内参"), QStringLiteral("train"));
    m_split->addItem(QStringLiteral("验证 · 不参与求解"), QStringLiteral("test"));
    m_evidence = new QComboBox(captureGroup);
    m_evidence->addItem(QStringLiteral("本机相机实采"), QStringLiteral("hardware"));
    m_evidence->addItem(QStringLiteral("公开数据"), QStringLiteral("public"));
    addRow(captureForm, QStringLiteral("采集序列 ID"), m_sequence);
    addRow(captureForm, QStringLiteral("数据用途"), m_split);
    addRow(captureForm, QStringLiteral("数据来源"), m_evidence);
    auto* actions = new QHBoxLayout();
    auto* apply = makeButton(QStringLiteral("下发采集参数"), captureGroup);
    m_capture = makeButton(QStringLiteral("采集当前帧并加入工程"), captureGroup, true);
    actions->addWidget(apply);
    actions->addWidget(m_capture);
    captureForm->addRow(actions);
    captureForm->addRow(new QLabel(QStringLiteral(
        "采集帧按当前工程的旋转 / 镜像与标定板参数检测角点，原始脉冲不会被改写。"), captureGroup));
    left->addWidget(captureGroup);
    left->addStretch(1);
    root->addLayout(left, 2);

    auto* right = new QVBoxLayout();
    auto* previewGroup = makeGroup(QStringLiteral("实时预览"), this);
    auto* previewLayout = new QVBoxLayout(previewGroup);
    m_preview = new QLabel(previewGroup);
    m_preview->setMinimumSize(640, 420);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("background:#1c2431;color:#9fb0c6;"));
    m_preview->setText(QStringLiteral("连接设备后开始预览"));
    previewLayout->addWidget(m_preview, 1);
    m_frameInfo = new QLabel(QStringLiteral("无帧数据"), previewGroup);
    previewLayout->addWidget(m_frameInfo);
    right->addWidget(previewGroup, 1);
    root->addLayout(right, 3);

    connect(browse, &QPushButton::clicked, this, &DevicePanel::onBrowse);
    connect(m_connect, &QPushButton::clicked, this, &DevicePanel::onConnect);
    connect(m_disconnect, &QPushButton::clicked, this, &DevicePanel::onDisconnect);
    connect(m_start, &QPushButton::clicked, this, &DevicePanel::onStartPreview);
    connect(m_stop, &QPushButton::clicked, this, &DevicePanel::onStopPreview);
    connect(apply, &QPushButton::clicked, this, &DevicePanel::onApplyParameters);
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
    parameters.insert(QStringLiteral("max_width"), lineToInt(m_maxWidth, QStringLiteral("最大帧宽")));
    parameters.insert(QStringLiteral("max_height"), lineToInt(m_maxHeight, QStringLiteral("最大帧高")));
    parameters.insert(QStringLiteral("channels"), lineToInt(m_channels, QStringLiteral("通道数")));
    for (const QPair<QLineEdit*, QString>& pair : {
             qMakePair(m_exposure, QStringLiteral("exposure_us")),
             qMakePair(m_gain, QStringLiteral("gain")),
             qMakePair(m_trigger, QStringLiteral("trigger_mode"))}) {
        if (!pair.first->text().trimmed().isEmpty()) {
            parameters.insert(pair.second, pair.first->text().trimmed().toInt());
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
        m_controller->connectDevice(m_backend->currentData().toString(), connectionParameters());
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
    }
}

void DevicePanel::onDisconnect() { m_controller->disconnectDevice(); }

void DevicePanel::onStartPreview() { m_controller->startPreview(100); }

void DevicePanel::onStopPreview() { m_controller->stopPreview(); }

void DevicePanel::onApplyParameters()
{
    for (const QPair<QLineEdit*, QString>& pair : {
             qMakePair(m_exposure, QStringLiteral("exposure_us")),
             qMakePair(m_gain, QStringLiteral("gain")),
             qMakePair(m_trigger, QStringLiteral("trigger_mode"))}) {
        if (!pair.first->text().trimmed().isEmpty()) {
            m_controller->setParameter(pair.second, pair.first->text().trimmed().toInt());
        }
    }
    emit notify(QStringLiteral("采集参数已下发（由厂商 SDK 回调实现）。"));
}

void DevicePanel::onCaptureStill()
{
    if (!m_connected) { emit notify(QStringLiteral("请先连接设备。"), true); return; }
    if (!hasProject()) { emit notify(QStringLiteral("请先新建工程。"), true); return; }
    if (m_sequence->text().trimmed().isEmpty()) {
        emit notify(QStringLiteral("请填写采集序列 ID。"), true);
        return;
    }
    m_controller->captureStill();
}

void DevicePanel::onFrame(const QImage& image, quint64 index, const QString& source)
{
    m_lastFrame = image;
    const QSize size = m_preview->size() - QSize(12, 12);
    m_preview->setPixmap(QPixmap::fromImage(image).scaled(size, Qt::KeepAspectRatio,
                                                          Qt::SmoothTransformation));
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
    m_capture->setEnabled(connected && hasProject());
    m_connect->setEnabled(!connected);
    emit deviceConnectionChanged(connected);
}

void DevicePanel::onStill(const QByteArray& png, const QString& name, const QString&)
{
    run(QStringLiteral("正在导入采集帧并检测角点…"), [this, png, name] {
        QJsonObject record;
        record.insert(QStringLiteral("name"), name);
        record.insert(QStringLiteral("content_base64"), QString::fromLatin1(png.toBase64()));
        m_workbench->importFiles(projectId(), QJsonArray{record},
                                 m_split->currentData().toString(),
                                 m_sequence->text().trimmed(),
                                 m_evidence->currentData().toString(), QJsonObject());
        emit notify(QStringLiteral("已把采集帧加入工程：%1").arg(name));
    });
}

void DevicePanel::onError(const QString& message) { emit notify(message, true); }

void DevicePanel::setProject(const QJsonObject& project)
{
    m_project = project;
    m_capture->setEnabled(m_connected && hasProject());
}

} // namespace ui
} // namespace spike

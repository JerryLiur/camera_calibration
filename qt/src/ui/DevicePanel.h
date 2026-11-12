#pragma once

#include "UiHelpers.h"

#include <QByteArray>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "device/DeviceController.h"

namespace spike {
namespace ui {

class DevicePanel : public ProjectPanel {
    Q_OBJECT
public:
    explicit DevicePanel(Workbench* workbench, DeviceController* controller,
                         QWidget* parent = nullptr);
    void setProject(const QJsonObject& project) override;

private slots:
    void onConnect();
    void onDisconnect();
    void onStartPreview();
    void onStopPreview();
    void onBrowse();
    void onApplyParameters();
    void onCaptureStill();
    void onFrame(const QImage& image, quint64 index, const QString& source);
    void onConnection(bool connected, const QString& message);
    void onStill(const QByteArray& png, const QString& name, const QString& message);
    void onError(const QString& message);

private:
    QVariantMap connectionParameters() const;

    DeviceController* m_controller = nullptr;
    QComboBox* m_backend = nullptr;
    QLineEdit* m_connection = nullptr;
    QLineEdit* m_maxWidth = nullptr;
    QLineEdit* m_maxHeight = nullptr;
    QLineEdit* m_channels = nullptr;
    QLineEdit* m_exposure = nullptr;
    QLineEdit* m_gain = nullptr;
    QLineEdit* m_trigger = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_frameInfo = nullptr;
    QLineEdit* m_sequence = nullptr;
    QComboBox* m_split = nullptr;
    QComboBox* m_evidence = nullptr;
    QPushButton* m_connect = nullptr;
    QPushButton* m_disconnect = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_capture = nullptr;
    bool m_connected = false;
    QImage m_lastFrame;
};

} // namespace ui
} // namespace spike

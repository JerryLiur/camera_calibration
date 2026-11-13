#pragma once

#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>

class QComboBox;
class QTabWidget;
class QPushButton;

namespace spike {

class Workbench;
class DeviceController;

namespace ui {

class CapturePanel;
class ConfigPanel;
class DevicePanel;
class IntrinsicsPanel;
class TurntablePanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Workbench* workbench, QWidget* parent = nullptr);

private slots:
    void refresh();
    void onNewProject();
    void onProjectSelected(int index);
    void onExport();
    void onReport();
    void onNotify(const QString& text, bool error = false);
    void onDeviceConnection(bool connected);

private:
    void applyProject(const QJsonObject& project);

    Workbench* m_workbench = nullptr;
    DeviceController* m_controller = nullptr;
    QComboBox* m_projectSelect = nullptr;
    QTabWidget* m_tabs = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_revision = nullptr;
    QLabel* m_projectState = nullptr;
    QLabel* m_connectionState = nullptr;
    QPushButton* m_exportButton = nullptr;
    QPushButton* m_reportButton = nullptr;
    CapturePanel* m_capture = nullptr;
    IntrinsicsPanel* m_intrinsics = nullptr;
    TurntablePanel* m_turntable = nullptr;
    ConfigPanel* m_config = nullptr;
    DevicePanel* m_device = nullptr;
    QJsonObject m_project;
    QString m_currentId;
};

} // namespace ui
} // namespace spike

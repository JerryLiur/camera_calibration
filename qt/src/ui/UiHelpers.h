#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>

#include "core/Workbench.h"

class QFormLayout;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;

namespace spike {
namespace ui {

QString fmt(double value, int precision = 4);
QString pretty(const QJsonValue& value);
QString statusText(const QString& status);
QString evidenceText(const QString& evidence);
QString workbenchStyleSheet();
QPushButton* makeButton(const QString& text, QWidget* parent, bool primary = false);
QGroupBox* makeGroup(const QString& title, QWidget* parent);
void addRow(QFormLayout* layout, const QString& label, QWidget* field);
QJsonValue lineToOptionalNumber(QLineEdit* field, const QString& name);
double lineToNumber(QLineEdit* field, const QString& name);
int lineToInt(QLineEdit* field, const QString& name);
void saveFileAtomically(const QString& path, const QByteArray& data);

class ProjectPanel : public QWidget {
    Q_OBJECT
public:
    ProjectPanel(spike::Workbench* workbench, QWidget* parent = nullptr)
        : QWidget(parent), m_workbench(workbench) {}

    virtual void setProject(const QJsonObject& project) = 0;

signals:
    void projectChanged();
    void notify(const QString& text, bool error = false);
    void deviceConnectionChanged(bool connected);

protected:
    bool run(const QString& busyMessage, const std::function<void()>& action);
    bool runBackground(const QString& busyMessage, const std::function<void()>& action,
                       const QString& completedMessage);
    QString projectId() const { return m_project.value(QStringLiteral("id")).toString(); }
    bool hasProject() const { return !projectId().isEmpty(); }

    spike::Workbench* m_workbench = nullptr;
    QJsonObject m_project;
};

} // namespace ui
} // namespace spike

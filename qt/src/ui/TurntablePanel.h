#pragma once

#include "UiHelpers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>

namespace spike {
namespace ui {

class TurntablePanel : public ProjectPanel {
    Q_OBJECT
public:
    explicit TurntablePanel(Workbench* workbench, QWidget* parent = nullptr);
    void setProject(const QJsonObject& project) override;

private slots:
    void onModeChanged();
    void onLoadObservations();
    void onFit();

private:
    QJsonArray parseObservations(const QString& text) const;

    QComboBox* m_mode = nullptr;
    QComboBox* m_sign = nullptr;
    QLineEdit* m_referenceAngle = nullptr;
    QCheckBox* m_farConfirmed = nullptr;
    QWidget* m_farWidget = nullptr;
    QWidget* m_finiteWidget = nullptr;
    QLineEdit* m_targetX = nullptr;
    QLineEdit* m_targetY = nullptr;
    QLineEdit* m_targetZ = nullptr;
    QLineEdit* m_offsetX = nullptr;
    QLineEdit* m_offsetY = nullptr;
    QLineEdit* m_offsetZ = nullptr;
    QPlainTextEdit* m_observations = nullptr;
    QLabel* m_result = nullptr;
    QPlainTextEdit* m_detail = nullptr;
    QPushButton* m_fitButton = nullptr;
};

} // namespace ui
} // namespace spike

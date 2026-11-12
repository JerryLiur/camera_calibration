#pragma once

#include "UiHelpers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

namespace spike {
namespace ui {

class ConfigPanel : public ProjectPanel {
    Q_OBJECT
public:
    explicit ConfigPanel(Workbench* workbench, QWidget* parent = nullptr);
    void setProject(const QJsonObject& project) override;

private slots:
    void onSave();

private:
    QJsonObject collectConfig() const;

    QComboBox* m_kind = nullptr;
    QLineEdit* m_spacing = nullptr;
    QLineEdit* m_cols = nullptr;
    QLineEdit* m_rows = nullptr;
    QComboBox* m_rotation = nullptr;
    QCheckBox* m_flipX = nullptr;
    QCheckBox* m_flipY = nullptr;
    QLineEdit* m_lens = nullptr;
    QLineEdit* m_focus = nullptr;
    QLineEdit* m_splitter = nullptr;
    QLineEdit* m_intrinsicsThreshold = nullptr;
    QLineEdit* m_turntableThreshold = nullptr;
    QLineEdit* m_operator = nullptr;
    QPushButton* m_save = nullptr;
};

} // namespace ui
} // namespace spike

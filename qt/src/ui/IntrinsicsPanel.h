#pragma once

#include "UiHelpers.h"

#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTableWidget>

namespace spike {
namespace ui {

class IntrinsicsPanel : public ProjectPanel {
    Q_OBJECT
public:
    explicit IntrinsicsPanel(Workbench* workbench, QWidget* parent = nullptr);
    void setProject(const QJsonObject& project) override;

private slots:
    void onSolve();
    void onValidate();
    void onQuery();

private:
    void refreshModel();
    void refreshValidation();

    QPushButton* m_solveButton = nullptr;
    QPushButton* m_validateButton = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_synthetic = nullptr;
    QTableWidget* m_matrix = nullptr;
    QTableWidget* m_distortion = nullptr;
    QTableWidget* m_perView = nullptr;
    QLabel* m_warnings = nullptr;
    QLabel* m_validationStatus = nullptr;
    QLabel* m_validationStats = nullptr;
    QLabel* m_validationNote = nullptr;
    QPlainTextEdit* m_modelJson = nullptr;
    QLineEdit* m_queryU = nullptr;
    QLineEdit* m_queryV = nullptr;
    QLineEdit* m_queryEncoder = nullptr;
    QLabel* m_queryResult = nullptr;
};

} // namespace ui
} // namespace spike

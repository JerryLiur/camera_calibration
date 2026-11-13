#pragma once

#include "ImageViewer.h"
#include "UiHelpers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>

namespace spike {
namespace ui {

class CapturePanel : public ProjectPanel {
    Q_OBJECT
public:
    explicit CapturePanel(Workbench* workbench, QWidget* parent = nullptr);
    void setProject(const QJsonObject& project) override;
    QJsonObject replayOptions() const { return options(); }

private slots:
    void onDemo();
    void onSolve();
    void onChooseFiles();
    void onImport();
    void onCellChanged(int row, int column);
    void onSplitChanged(int index);
    void onSelectCapture(int row, int column);
    void onUndistortChanged();
    void onPixelHovered(const QPointF& pixel);
    void onPixelClicked(const QPointF& pixel);

private:
    void refreshTable();
    void refreshViewer();
    void refreshCoverage();
    QJsonObject options() const;

    ImageViewer* m_viewer = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_filename = nullptr;
    QLabel* m_pointer = nullptr;
    QLabel* m_ray = nullptr;
    QLabel* m_counts = nullptr;
    QLabel* m_coverageLabel = nullptr;
    QWidget* m_coverageGrid = nullptr;
    QPushButton* m_solveButton = nullptr;
    QCheckBox* m_undistort = nullptr;

    QLineEdit* m_sequence = nullptr;
    QComboBox* m_split = nullptr;
    QComboBox* m_evidence = nullptr;
    QLabel* m_fileSummary = nullptr;
    QStringList m_files;

    QComboBox* m_method = nullptr;
    QLineEdit* m_windowStart = nullptr;
    QLineEdit* m_windowTicks = nullptr;
    QLineEdit* m_period = nullptr;
    QCheckBox* m_layoutConfirmed = nullptr;
    QComboBox* m_layout = nullptr;
    QComboBox* m_bitorder = nullptr;
    QLineEdit* m_datWidth = nullptr;
    QLineEdit* m_datHeight = nullptr;
    QLineEdit* m_datHeader = nullptr;
    QLineEdit* m_datStride = nullptr;

    QString m_selectedId;
    bool m_updating = false;
};

} // namespace ui
} // namespace spike

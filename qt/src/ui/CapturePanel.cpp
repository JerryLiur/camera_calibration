#include "CapturePanel.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace spike {
namespace ui {

CapturePanel::CapturePanel(Workbench* workbench, QWidget* parent)
    : ProjectPanel(workbench, parent)
{
    auto* root = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    auto* left = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(left);
    m_filename = new QLabel(QStringLiteral("图像与角点"), left);
    m_filename->setStyleSheet(QStringLiteral("font-weight:600;"));
    auto* controls = new QHBoxLayout();
    m_undistort = new QCheckBox(QStringLiteral("去畸变预览"), left);
    controls->addWidget(m_filename);
    controls->addStretch(1);
    controls->addWidget(m_undistort);
    leftLayout->addLayout(controls);
    m_viewer = new ImageViewer(left);
    leftLayout->addWidget(m_viewer, 1);
    m_pointer = new QLabel(QStringLiteral("u → 右 ／ v → 下"), left);
    m_ray = new QLabel(QStringLiteral("内参求解后可点击图像查询视线"), left);
    m_pointer->setStyleSheet(QStringLiteral("color:#536177;"));
    m_ray->setStyleSheet(QStringLiteral("color:#536177;"));
    leftLayout->addWidget(m_pointer);
    leftLayout->addWidget(m_ray);
    splitter->addWidget(left);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    auto* heading = new QHBoxLayout();
    m_counts = new QLabel(QStringLiteral("0 训练 · 0 验证"), right);
    m_counts->setStyleSheet(QStringLiteral("font-weight:600;"));
    m_solveButton = makeButton(QStringLiteral("求解内参"), right, true);
    auto* demoButton = makeButton(QStringLiteral("加载合成示例"), right);
    heading->addWidget(m_counts);
    heading->addStretch(1);
    heading->addWidget(demoButton);
    heading->addWidget(m_solveButton);
    rightLayout->addLayout(heading);

    m_table = new QTableWidget(0, 4, right);
    m_table->setHorizontalHeaderLabels({QStringLiteral("参与"), QStringLiteral("图像 / 采集序列"),
                                        QStringLiteral("用途"), QStringLiteral("角点")});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMaximumHeight(260);
    rightLayout->addWidget(m_table);

    auto* coverage = new QHBoxLayout();
    m_coverageLabel = new QLabel(QStringLiteral("训练视场覆盖 0%"), right);
    m_coverageGrid = new QWidget(right);
    auto* grid = new QGridLayout(m_coverageGrid);
    grid->setSpacing(2);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) {
            auto* cell = new QFrame(m_coverageGrid);
            cell->setFixedSize(14, 10);
            cell->setStyleSheet(QStringLiteral("background:#eef2f7;border:1px solid #d8e0ea;"));
            grid->addWidget(cell, y, x);
        }
    }
    coverage->addWidget(m_coverageLabel);
    coverage->addStretch(1);
    coverage->addWidget(m_coverageGrid);
    rightLayout->addLayout(coverage);
    rightLayout->addWidget(new QLabel(QStringLiteral("验证必须来自独立采集序列，同一序列不能同时用于训练与验证。"), right));

    auto* importGroup = makeGroup(QStringLiteral("导入采集数据"), right);
    auto* form = new QFormLayout(importGroup);
    m_sequence = new QLineEdit(importGroup);
    m_sequence->setPlaceholderText(QStringLiteral("例如 capture-fit-01"));
    m_split = new QComboBox(importGroup);
    m_split->addItem(QStringLiteral("训练 · 求解内参"), QStringLiteral("train"));
    m_split->addItem(QStringLiteral("验证 · 不参与求解"), QStringLiteral("test"));
    m_evidence = new QComboBox(importGroup);
    m_evidence->addItem(QStringLiteral("本机相机实采"), QStringLiteral("hardware"));
    m_evidence->addItem(QStringLiteral("公开数据"), QStringLiteral("public"));
    auto* fileRow = new QHBoxLayout();
    auto* choose = makeButton(QStringLiteral("选择文件…"), importGroup);
    m_fileSummary = new QLabel(QStringLiteral("尚未选择文件"), importGroup);
    fileRow->addWidget(choose);
    fileRow->addWidget(m_fileSummary, 1);
    addRow(form, QStringLiteral("采集序列 ID"), m_sequence);
    addRow(form, QStringLiteral("数据用途"), m_split);
    addRow(form, QStringLiteral("数据来源"), m_evidence);
    form->addRow(QStringLiteral("文件"), fileRow);
    auto* importButton = makeButton(QStringLiteral("导入所选文件 / 角点 JSON"), importGroup, true);
    form->addRow(QString(), importButton);

    auto* spikeGroup = makeGroup(QStringLiteral("Spike 重建窗口与 DAT 布局"), right);
    auto* spikeForm = new QFormLayout(spikeGroup);
    m_method = new QComboBox(spikeGroup);
    m_method->addItem(QStringLiteral("窗口计数 / TFP"), QStringLiteral("count"));
    m_method->addItem(QStringLiteral("脉冲间隔统计"), QStringLiteral("interval"));
    m_method->addItem(QStringLiteral("发放率（需采样周期）"), QStringLiteral("rate"));
    m_windowStart = new QLineEdit(QStringLiteral("0"), spikeGroup);
    m_windowTicks = new QLineEdit(QStringLiteral("256"), spikeGroup);
    m_period = new QLineEdit(spikeGroup);
    m_period->setPlaceholderText(QStringLiteral("未知时留空"));
    addRow(spikeForm, QStringLiteral("重建方式"), m_method);
    addRow(spikeForm, QStringLiteral("起始 tick"), m_windowStart);
    addRow(spikeForm, QStringLiteral("窗口长度 / tick"), m_windowTicks);
    addRow(spikeForm, QStringLiteral("采样周期 / s"), m_period);
    m_layoutConfirmed = new QCheckBox(QStringLiteral("已核验厂商真实位布局"), spikeGroup);
    m_layout = new QComboBox(spikeGroup);
    m_layout->addItem(QStringLiteral("packed_flat（逐帧连续位）"), QStringLiteral("packed_flat"));
    m_layout->addItem(QStringLiteral("row_packed（每行字节对齐）"), QStringLiteral("row_packed"));
    m_bitorder = new QComboBox(spikeGroup);
    m_bitorder->addItem(QStringLiteral("little"), QStringLiteral("little"));
    m_bitorder->addItem(QStringLiteral("big"), QStringLiteral("big"));
    m_datWidth = new QLineEdit(spikeGroup);
    m_datHeight = new QLineEdit(spikeGroup);
    m_datHeader = new QLineEdit(QStringLiteral("0"), spikeGroup);
    m_datStride = new QLineEdit(spikeGroup);
    m_datStride->setPlaceholderText(QStringLiteral("留空按最小步长"));
    spikeForm->addRow(QString(), m_layoutConfirmed);
    addRow(spikeForm, QStringLiteral("DAT 布局"), m_layout);
    addRow(spikeForm, QStringLiteral("位序"), m_bitorder);
    addRow(spikeForm, QStringLiteral("DAT 宽度"), m_datWidth);
    addRow(spikeForm, QStringLiteral("DAT 高度"), m_datHeight);
    addRow(spikeForm, QStringLiteral("文件头字节"), m_datHeader);
    addRow(spikeForm, QStringLiteral("帧步长字节"), m_datStride);
    rightLayout->addWidget(importGroup);
    rightLayout->addWidget(spikeGroup);
    rightLayout->addStretch(1);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(demoButton, &QPushButton::clicked, this, &CapturePanel::onDemo);
    connect(m_solveButton, &QPushButton::clicked, this, &CapturePanel::onSolve);
    connect(choose, &QPushButton::clicked, this, &CapturePanel::onChooseFiles);
    connect(importButton, &QPushButton::clicked, this, &CapturePanel::onImport);
    connect(m_table, &QTableWidget::cellChanged, this, &CapturePanel::onCellChanged);
    connect(m_table, &QTableWidget::cellClicked, this, &CapturePanel::onSelectCapture);
    connect(m_undistort, &QCheckBox::toggled, this, &CapturePanel::onUndistortChanged);
    connect(m_viewer, &ImageViewer::pixelHovered, this, &CapturePanel::onPixelHovered);
    connect(m_viewer, &ImageViewer::pixelClicked, this, &CapturePanel::onPixelClicked);
}

QJsonObject CapturePanel::options() const
{
    QJsonObject options;
    options.insert(QStringLiteral("method"), m_method->currentData().toString());
    if (!m_windowStart->text().trimmed().isEmpty()) {
        options.insert(QStringLiteral("window_start_tick"), lineToInt(m_windowStart, QStringLiteral("起始 tick")));
    }
    if (!m_windowTicks->text().trimmed().isEmpty()) {
        options.insert(QStringLiteral("window_ticks"), lineToInt(m_windowTicks, QStringLiteral("窗口长度")));
    }
    const QJsonValue period = lineToOptionalNumber(m_period, QStringLiteral("采样周期"));
    if (!period.isNull() && !period.isUndefined()) options.insert(QStringLiteral("period_s"), period);
    if (m_layoutConfirmed->isChecked()) {
        options.insert(QStringLiteral("layout_confirmed"), true);
        options.insert(QStringLiteral("layout"), m_layout->currentData().toString());
        options.insert(QStringLiteral("bitorder"), m_bitorder->currentData().toString());
        options.insert(QStringLiteral("width"), lineToInt(m_datWidth, QStringLiteral("DAT 宽度")));
        options.insert(QStringLiteral("height"), lineToInt(m_datHeight, QStringLiteral("DAT 高度")));
        if (!m_datHeader->text().trimmed().isEmpty()) {
            options.insert(QStringLiteral("header_bytes"), lineToInt(m_datHeader, QStringLiteral("文件头字节")));
        }
        if (!m_datStride->text().trimmed().isEmpty()) {
            options.insert(QStringLiteral("frame_stride_bytes"),
                           lineToInt(m_datStride, QStringLiteral("帧步长字节")));
        }
    }
    return options;
}

void CapturePanel::setProject(const QJsonObject& project)
{
    m_project = project;
    refreshTable();
    refreshViewer();
    refreshCoverage();
}

void CapturePanel::refreshTable()
{
    m_updating = true;
    const QJsonArray captures = m_project.value(QStringLiteral("captures")).toArray();
    int train = 0, test = 0, failed = 0;
    for (const QJsonValue& item : captures) {
        const QJsonObject capture = item.toObject();
        if (!capture.value(QStringLiteral("found")).toBool()) { ++failed; continue; }
        if (!capture.value(QStringLiteral("enabled")).toBool()) continue;
        if (capture.value(QStringLiteral("split")).toString() == QStringLiteral("train")) ++train;
        else ++test;
    }
    m_counts->setText(QStringLiteral("%1 训练 · %2 验证 · %3 未检出").arg(train).arg(test).arg(failed));
    m_solveButton->setEnabled(hasProject() && train >= 6);
    m_undistort->setEnabled(!m_project.value(QStringLiteral("model")).isNull());

    m_table->blockSignals(true);
    m_table->setRowCount(static_cast<int>(captures.size()));
    for (int row = 0; row < captures.size(); ++row) {
        const QJsonObject capture = captures.at(row).toObject();
        const QString id = capture.value(QStringLiteral("id")).toString();
        auto* enabled = new QTableWidgetItem();
        enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enabled->setCheckState(capture.value(QStringLiteral("enabled")).toBool()
                                   ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, 0, enabled);
        auto* name = new QTableWidgetItem(capture.value(QStringLiteral("name")).toString()
                                          + QStringLiteral("\n")
                                          + capture.value(QStringLiteral("sequence_id")).toString());
        name->setData(Qt::UserRole, id);
        m_table->setItem(row, 1, name);
        auto* split = new QComboBox(m_table);
        split->addItem(QStringLiteral("训练"), QStringLiteral("train"));
        split->addItem(QStringLiteral("验证"), QStringLiteral("test"));
        split->setCurrentIndex(capture.value(QStringLiteral("split")).toString() == QStringLiteral("test") ? 1 : 0);
        split->setProperty("captureId", id);
        connect(split, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &CapturePanel::onSplitChanged, Qt::UniqueConnection);
        m_table->setCellWidget(row, 2, split);
        auto* points = new QTableWidgetItem(capture.value(QStringLiteral("found")).toBool()
                                                ? QString::number(capture.value(QStringLiteral("image_points"))
                                                                      .toArray().size())
                                                : QStringLiteral("未检出"));
        m_table->setItem(row, 3, points);
    }
    m_table->blockSignals(false);
    if (m_selectedId.isEmpty() && !captures.isEmpty()) {
        m_selectedId = captures.at(0).toObject().value(QStringLiteral("id")).toString();
    }
    m_updating = false;
}

void CapturePanel::refreshViewer()
{
    if (!hasProject()) { m_viewer->clear(); return; }
    const QJsonArray captures = m_project.value(QStringLiteral("captures")).toArray();
    QJsonObject selected;
    for (const QJsonValue& item : captures) {
        const QJsonObject capture = item.toObject();
        if (capture.value(QStringLiteral("id")).toString() == m_selectedId) { selected = capture; break; }
    }
    if (selected.isEmpty() && !captures.isEmpty()) {
        selected = captures.at(0).toObject();
        m_selectedId = selected.value(QStringLiteral("id")).toString();
    }
    if (selected.isEmpty()) {
        m_viewer->clear();
        m_filename->setText(QStringLiteral("图像与角点"));
        return;
    }
    const bool undistort = m_undistort->isChecked()
        && !m_project.value(QStringLiteral("model")).isNull();
    try {
        const QByteArray png = m_workbench->preview(projectId(), m_selectedId, undistort);
        QImage image;
        image.loadFromData(png, "PNG");
        m_viewer->setImage(image);
        QVector<QPointF> points;
        for (const QJsonValue& item : selected.value(QStringLiteral("image_points")).toArray()) {
            const QJsonArray point = item.toArray();
            points.append(QPointF(point.at(0).toDouble(), point.at(1).toDouble()));
        }
        const QJsonArray size = selected.value(QStringLiteral("image_size")).toArray();
        m_viewer->setPoints(points, QSize(size.at(0).toInt(), size.at(1).toInt()));
        m_filename->setText(QStringLiteral("%1 · %2 · %3")
                                .arg(selected.value(QStringLiteral("name")).toString(),
                                     selected.value(QStringLiteral("sequence_id")).toString(),
                                     evidenceText(selected.value(QStringLiteral("evidence")).toString())));
    } catch (const std::exception& error) {
        m_viewer->clear();
        m_filename->setText(QString::fromUtf8(error.what()));
    }
}

void CapturePanel::refreshCoverage()
{
    bool cells[4][6]{};
    int filled = 0;
    for (const QJsonValue& item : m_project.value(QStringLiteral("captures")).toArray()) {
        const QJsonObject capture = item.toObject();
        if (!capture.value(QStringLiteral("enabled")).toBool()
            || !capture.value(QStringLiteral("found")).toBool()
            || capture.value(QStringLiteral("split")).toString() != QStringLiteral("train")) continue;
        const QJsonArray size = capture.value(QStringLiteral("image_size")).toArray();
        const double width = size.at(0).toDouble(), height = size.at(1).toDouble();
        if (width <= 0 || height <= 0) continue;
        for (const QJsonValue& pointValue : capture.value(QStringLiteral("image_points")).toArray()) {
            const QJsonArray point = pointValue.toArray();
            const double u = point.at(0).toDouble(), v = point.at(1).toDouble();
            if (u < 0 || v < 0 || u >= width || v >= height) continue;
            const int x = std::clamp(static_cast<int>(u / width * 6), 0, 5);
            const int y = std::clamp(static_cast<int>(v / height * 4), 0, 3);
            cells[y][x] = true;
        }
    }
    auto* grid = qobject_cast<QGridLayout*>(m_coverageGrid->layout());
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) {
            auto* cell = qobject_cast<QFrame*>(grid->itemAtPosition(y, x)->widget());
            if (cells[y][x]) {
                ++filled;
                cell->setStyleSheet(QStringLiteral("background:#1256c0;border:1px solid #0b449d;"));
            } else {
                cell->setStyleSheet(QStringLiteral("background:#eef2f7;border:1px solid #d8e0ea;"));
            }
        }
    }
    m_coverageLabel->setText(QStringLiteral("训练视场覆盖 %1%").arg(qRound(filled / 24.0 * 100)));
}

void CapturePanel::onDemo()
{
    if (!m_project.value(QStringLiteral("captures")).toArray().isEmpty()) {
        if (QMessageBox::question(this, QStringLiteral("加载合成示例"),
                                  QStringLiteral("建议在独立工程中体验。当前工程的采集与结果会失效，继续？"))
            != QMessageBox::Yes) return;
    }
    run(QStringLiteral("正在生成合成多姿态示例…"), [this] {
        m_workbench->loadDemo(projectId());
        m_selectedId.clear();
        emit notify(QStringLiteral("已加载合成多姿态示例，仅用于软件与数值流程验证。"));
    });
}

void CapturePanel::onSolve()
{
    run(QStringLiteral("正在求解 Spike 内参…"), [this] {
        m_workbench->solve(projectId());
        emit notify(QStringLiteral("Spike 内参已求解，请继续使用独立采集序列验证。"));
    });
}

void CapturePanel::onChooseFiles()
{
    m_files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择采集文件"), QString(),
        QStringLiteral("采集数据 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.npy *.dat *.bin *.json);;所有文件 (*)"));
    m_fileSummary->setText(m_files.isEmpty()
                               ? QStringLiteral("尚未选择文件")
                               : QStringLiteral("已选择 %1 个文件").arg(m_files.size()));
}

void CapturePanel::onImport()
{
    if (m_files.isEmpty()) { emit notify(QStringLiteral("请先选择文件。"), true); return; }
    if (!hasProject()) { emit notify(QStringLiteral("请先新建工程。"), true); return; }
    run(QStringLiteral("正在读取数据并检测角点…"), [this] {
        const QString sequence = m_sequence->text().trimmed();
        if (sequence.isEmpty()) fail(QStringLiteral("请填写采集序列 ID。"));
        const QString split = m_split->currentData().toString();
        const QString evidence = m_evidence->currentData().toString();
        bool hasJson = false, hasBinary = false;
        for (const QString& path : m_files) {
            if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) hasJson = true;
            else hasBinary = true;
        }
        if (hasJson && hasBinary) {
            fail(QStringLiteral("角点 JSON 与图像 / Spike 文件请分批导入。"));
        }
        if (hasJson) {
            QJsonArray captures;
            for (const QString& path : m_files) {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取 %1").arg(path));
                const QJsonValue value = strictLoads(file.readAll());
                const QJsonArray list = value.isArray() ? value.toArray()
                    : value.toObject().value(QStringLiteral("captures")).toArray();
                if (list.isEmpty()) fail(QStringLiteral("%1 需要包含非空 captures 数组。").arg(path));
                for (const QJsonValue& item : list) {
                    QJsonObject capture = item.toObject();
                    capture.insert(QStringLiteral("sequence_id"),
                                   capture.value(QStringLiteral("sequence_id")).toString().isEmpty()
                                       ? sequence : capture.value(QStringLiteral("sequence_id")).toString());
                    capture.insert(QStringLiteral("split"),
                                   capture.value(QStringLiteral("split")).toString().isEmpty()
                                       ? split : capture.value(QStringLiteral("split")).toString());
                    capture.insert(QStringLiteral("evidence"),
                                   capture.value(QStringLiteral("evidence")).toString().isEmpty()
                                       ? evidence : capture.value(QStringLiteral("evidence")).toString());
                    capture.insert(QStringLiteral("name"),
                                   capture.value(QStringLiteral("name")).toString().isEmpty()
                                       ? QFileInfo(path).fileName()
                                       : capture.value(QStringLiteral("name")).toString());
                    captures.append(capture);
                }
            }
            m_workbench->importObservations(projectId(), captures);
        } else {
            QJsonArray files;
            for (const QString& path : m_files) {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取 %1").arg(path));
                QJsonObject record;
                record.insert(QStringLiteral("name"), QFileInfo(path).fileName());
                record.insert(QStringLiteral("content_base64"),
                              QString::fromLatin1(file.readAll().toBase64()));
                files.append(record);
            }
            m_workbench->importFiles(projectId(), files, split, sequence, evidence, options());
        }
        m_files.clear();
        m_fileSummary->setText(QStringLiteral("尚未选择文件"));
        m_selectedId.clear();
        emit notify(QStringLiteral("数据已导入，请检查角点、用途划分与视场覆盖。"));
    });
}

void CapturePanel::onCellChanged(int row, int column)
{
    if (m_updating || column != 0) return;
    const QTableWidgetItem* name = m_table->item(row, 1);
    if (!name) return;
    auto* check = m_table->item(row, 0);
    run(QStringLiteral("正在更新观测用途…"), [this, id = name->data(Qt::UserRole).toString(),
                                              enabled = check->checkState() == Qt::Checked] {
        QJsonObject body;
        body.insert(QStringLiteral("enabled"), enabled);
        m_workbench->updateCapture(projectId(), id, body);
    });
}

void CapturePanel::onSplitChanged(int index)
{
    if (m_updating) return;
    auto* combo = qobject_cast<QComboBox*>(sender());
    if (!combo) return;
    const QString id = combo->property("captureId").toString();
    const QString split = combo->itemData(index).toString();
    run(QStringLiteral("正在更新观测用途…"), [this, id, split] {
        QJsonObject body;
        body.insert(QStringLiteral("split"), split);
        m_workbench->updateCapture(projectId(), id, body);
    });
}

void CapturePanel::onSelectCapture(int row, int)
{
    const QTableWidgetItem* item = m_table->item(row, 1);
    if (!item) return;
    m_selectedId = item->data(Qt::UserRole).toString();
    refreshViewer();
}

void CapturePanel::onUndistortChanged() { refreshViewer(); }

void CapturePanel::onPixelHovered(const QPointF& pixel)
{
    m_pointer->setText(QStringLiteral("u %1 ／ v %2").arg(fmt(pixel.x(), 1), fmt(pixel.y(), 1)));
}

void CapturePanel::onPixelClicked(const QPointF& pixel)
{
    if (m_project.value(QStringLiteral("model")).isNull()) {
        emit notify(QStringLiteral("请先完成内参求解，再点击图像查询视线。"), true);
        return;
    }
    if (m_undistort->isChecked()) {
        emit notify(QStringLiteral("请关闭去畸变预览后查询，避免重复去畸变。"), true);
        return;
    }
    try {
        const QJsonObject result = m_workbench->query(
            projectId(), QJsonArray{QJsonArray{QJsonValue(pixel.x()), QJsonValue(pixel.y())}});
        const QJsonObject point = result.value(QStringLiteral("points")).toArray().at(0).toObject();
        m_ray->setText(QStringLiteral("方位角 %1° · 俯仰角 %2° · u %3 v %4")
                           .arg(fmt(point.value(QStringLiteral("azimuth_deg")).toDouble()),
                                fmt(point.value(QStringLiteral("elevation_deg")).toDouble()),
                                fmt(pixel.x(), 2), fmt(pixel.y(), 2)));
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
    }
}

} // namespace ui
} // namespace spike

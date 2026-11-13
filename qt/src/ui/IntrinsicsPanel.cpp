#include "IntrinsicsPanel.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace spike {
namespace ui {

IntrinsicsPanel::IntrinsicsPanel(Workbench* workbench, QWidget* parent)
    : ProjectPanel(workbench, parent)
{
    auto* root = new QVBoxLayout(this);
    auto* heading = new QHBoxLayout();
    m_status = new QLabel(QStringLiteral("内参尚未求解"), this);
    m_status->setStyleSheet(QStringLiteral("font-size:16px;font-weight:600;"));
    m_synthetic = new QLabel(this);
    heading->addWidget(m_status);
    heading->addWidget(m_synthetic);
    heading->addStretch(1);
    m_validateButton = makeButton(QStringLiteral("独立验证"), this);
    m_solveButton = makeButton(QStringLiteral("求解内参"), this, true);
    heading->addWidget(m_validateButton);
    heading->addWidget(m_solveButton);
    root->addLayout(heading);

    auto* columns = new QHBoxLayout();
    columns->setSpacing(16);
    auto* left = new QVBoxLayout();
    left->setSpacing(16);
    auto* modelGroup = makeGroup(QStringLiteral("Spike 成像模型"), this);
    auto* modelLayout = new QVBoxLayout(modelGroup);
    m_matrix = new QTableWidget(3, 3, modelGroup);
    m_matrix->horizontalHeader()->hide();
    m_matrix->verticalHeader()->hide();
    m_matrix->setMaximumHeight(120);
    m_matrix->setMinimumHeight(110);
    m_matrix->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_matrix->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_matrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelLayout->addWidget(new QLabel(QStringLiteral("相机内参 K"), modelGroup));
    modelLayout->addWidget(m_matrix);
    m_distortion = new QTableWidget(0, 2, modelGroup);
    m_distortion->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("值")});
    m_distortion->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_distortion->setMinimumHeight(200);
    m_distortion->setMaximumHeight(210);
    m_distortion->verticalHeader()->hide();
    m_distortion->verticalHeader()->setDefaultSectionSize(32);
    m_distortion->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelLayout->addWidget(new QLabel(QStringLiteral("完整支路畸变参数"), modelGroup));
    modelLayout->addWidget(m_distortion);
    m_warnings = new QLabel(modelGroup);
    m_warnings->setWordWrap(true);
    m_warnings->setStyleSheet(QStringLiteral("color:#87520b;"));
    modelLayout->addWidget(m_warnings);
    left->addWidget(modelGroup);

    auto* perViewGroup = makeGroup(QStringLiteral("训练姿态残差"), this);
    auto* perViewLayout = new QVBoxLayout(perViewGroup);
    m_perView = new QTableWidget(0, 2, perViewGroup);
    m_perView->setHorizontalHeaderLabels({QStringLiteral("姿态"), QStringLiteral("RMS / px")});
    m_perView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_perView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_perView->setMaximumHeight(220);
    m_perView->setMinimumHeight(150);
    m_perView->verticalHeader()->hide();
    m_perView->verticalHeader()->setDefaultSectionSize(32);
    perViewLayout->addWidget(m_perView);
    left->addWidget(perViewGroup);
    columns->addLayout(left, 3);

    auto* right = new QVBoxLayout();
    right->setSpacing(16);
    auto* validationGroup = makeGroup(QStringLiteral("独立验证（冻结内参）"), this);
    auto* validationLayout = new QVBoxLayout(validationGroup);
    m_validationStatus = new QLabel(QStringLiteral("等待独立验证"), validationGroup);
    m_validationStatus->setStyleSheet(QStringLiteral("font-weight:600;"));
    m_validationStats = new QLabel(QStringLiteral("—"), validationGroup);
    m_validationStats->setWordWrap(true);
    m_validationNote = new QLabel(
        QStringLiteral("使用未参与求解的独立序列，固定内参计算重投影残差。未设置阈值时只报告测量值。"),
        validationGroup);
    m_validationNote->setWordWrap(true);
    m_validationNote->setStyleSheet(QStringLiteral("color:#536177;"));
    validationLayout->addWidget(m_validationStatus);
    validationLayout->addWidget(m_validationStats);
    validationLayout->addWidget(m_validationNote);
    right->addWidget(validationGroup);

    auto* queryGroup = makeGroup(QStringLiteral("像素 → 视线"), this);
    auto* queryForm = new QFormLayout(queryGroup);
    m_queryU = new QLineEdit(queryGroup);
    m_queryV = new QLineEdit(queryGroup);
    m_queryU->setObjectName(QStringLiteral("queryU"));
    m_queryV->setObjectName(QStringLiteral("queryV"));
    m_queryEncoder = new QLineEdit(QStringLiteral("0"), queryGroup);
    addRow(queryForm, QStringLiteral("u / px"), m_queryU);
    addRow(queryForm, QStringLiteral("v / px"), m_queryV);
    addRow(queryForm, QStringLiteral("转台编码角 / °"), m_queryEncoder);
    m_queryButton = makeButton(QStringLiteral("计算视线"), queryGroup);
    queryForm->addRow(QString(), m_queryButton);
    m_queryResult = new QLabel(QStringLiteral("方位右正 · 俯仰上正"), queryGroup);
    m_queryResult->setWordWrap(true);
    queryForm->addRow(m_queryResult);
    right->addWidget(queryGroup);

    auto* detailGroup = makeGroup(QStringLiteral("完整模型与诊断"), this);
    auto* detailLayout = new QVBoxLayout(detailGroup);
    m_modelJson = new QPlainTextEdit(detailGroup);
    m_modelJson->setReadOnly(true);
    m_modelJson->setMinimumHeight(200);
    m_modelJson->setStyleSheet(QStringLiteral("font-family:Consolas,monospace;font-size:12px;"));
    detailLayout->addWidget(m_modelJson);
    auto* detailsToggle = new QToolButton(this);
    detailsToggle->setObjectName(QStringLiteral("modelDetailsToggle"));
    detailsToggle->setText(QStringLiteral("完整模型与诊断"));
    detailsToggle->setCheckable(true);
    detailsToggle->setArrowType(Qt::RightArrow);
    detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    detailsToggle->setMinimumHeight(36);
    detailsToggle->setAccessibleName(QStringLiteral("展开完整模型与诊断"));
    detailGroup->hide();
    connect(detailsToggle, &QToolButton::toggled, this, [detailsToggle, detailGroup](bool expanded) {
        detailGroup->setVisible(expanded);
        detailsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        detailsToggle->setAccessibleName(expanded ? QStringLiteral("收起完整模型与诊断")
                                                 : QStringLiteral("展开完整模型与诊断"));
    });
    right->addWidget(detailsToggle);
    right->addWidget(detailGroup);
    right->addStretch(1);
    columns->addLayout(right, 2);
    root->addLayout(columns);

    connect(m_solveButton, &QPushButton::clicked, this, &IntrinsicsPanel::onSolve);
    connect(m_validateButton, &QPushButton::clicked, this, &IntrinsicsPanel::onValidate);
    connect(m_queryButton, &QPushButton::clicked, this, &IntrinsicsPanel::onQuery);
    for (QLineEdit* field : {m_queryU, m_queryV, m_queryEncoder}) {
        connect(field, &QLineEdit::textChanged, this, [this] {
            m_queryResult->setText(QStringLiteral("方位右正 · 俯仰上正"));
        });
    }
}

void IntrinsicsPanel::setProject(const QJsonObject& project)
{
    if (project.value(QStringLiteral("id")) != m_project.value(QStringLiteral("id"))
        || project.value(QStringLiteral("model")) != m_project.value(QStringLiteral("model"))
        || project.value(QStringLiteral("turntable")) != m_project.value(QStringLiteral("turntable"))) {
        m_queryResult->setText(QStringLiteral("方位右正 · 俯仰上正"));
        m_queryU->clear();
        m_queryV->clear();
    }
    m_project = project;
    refreshModel();
    refreshValidation();
}

void IntrinsicsPanel::refreshModel()
{
    const QJsonObject model = m_project.value(QStringLiteral("model")).toObject();
    const bool solved = !model.isEmpty();
    m_queryButton->setEnabled(solved);
    int train = 0, test = 0;
    for (const QJsonValue& item : m_project.value(QStringLiteral("captures")).toArray()) {
        const QJsonObject capture = item.toObject();
        if (!capture.value(QStringLiteral("enabled")).toBool()
            || !capture.value(QStringLiteral("found")).toBool()) continue;
        (capture.value(QStringLiteral("split")).toString() == QStringLiteral("train") ? train : test) += 1;
    }
    m_solveButton->setEnabled(hasProject() && train >= 6);
    m_validateButton->setEnabled(hasProject() && solved && test > 0);
    m_status->setText(solved ? QStringLiteral("已求解 · 冻结模型") : QStringLiteral("内参尚未求解"));
    bool synthetic = false;
    for (const QJsonValue& item : model.value(QStringLiteral("evidence")).toArray()) {
        if (item.toString() == QStringLiteral("synthetic")) synthetic = true;
    }
    m_synthetic->setText(solved && synthetic ? QStringLiteral("合成结果，不能作为实机验收") : QString());
    m_synthetic->setStyleSheet(QStringLiteral("color:#87520b;"));
    m_matrix->clearContents();
    const QJsonArray K = model.value(QStringLiteral("K")).toArray();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const double value = K.at(r).toArray().at(c).toDouble();
            m_matrix->setItem(r, c, new QTableWidgetItem(solved ? fmt(value, 5) : QStringLiteral("—")));
        }
    }
    const QStringList terms = {QStringLiteral("k₁"), QStringLiteral("k₂"), QStringLiteral("p₁"),
                               QStringLiteral("p₂"), QStringLiteral("k₃"), QStringLiteral("k₄"),
                               QStringLiteral("k₅"), QStringLiteral("k₆")};
    const QJsonArray distortion = model.value(QStringLiteral("distortion")).toArray();
    m_distortion->setRowCount(static_cast<int>(distortion.size()));
    for (int i = 0; i < distortion.size(); ++i) {
        m_distortion->setItem(i, 0, new QTableWidgetItem(i < terms.size()
                                                             ? terms.at(i) : QStringLiteral("d%1").arg(i + 1)));
        m_distortion->setItem(i, 1, new QTableWidgetItem(fmt(distortion.at(i).toDouble(), 8)));
    }
    QStringList warnings;
    for (const QJsonValue& item : model.value(QStringLiteral("warnings")).toArray()) {
        warnings.append(item.toString());
    }
    m_warnings->setText(warnings.isEmpty() ? QString() : QStringLiteral("提示：%1").arg(warnings.join(QStringLiteral("；"))));

    const QJsonArray perView = model.value(QStringLiteral("diagnostics")).toObject()
                                  .value(QStringLiteral("per_view")).toArray();
    m_perView->setRowCount(static_cast<int>(perView.size()));
    for (int i = 0; i < perView.size(); ++i) {
        const QJsonObject item = perView.at(i).toObject();
        m_perView->setItem(i, 0, new QTableWidgetItem(item.value(QStringLiteral("name")).toString()));
        m_perView->setItem(i, 1, new QTableWidgetItem(fmt(item.value(QStringLiteral("rms_px")).toDouble())));
    }
    m_modelJson->setPlainText(solved ? pretty(model) : QStringLiteral("至少需要 6 个有效训练姿态并改变倾角。"));
    if (m_queryU->text().isEmpty() && solved) {
        m_queryU->setText(fmt(K.at(0).toArray().at(2).toDouble(), 3));
        m_queryV->setText(fmt(K.at(1).toArray().at(2).toDouble(), 3));
    }
}

void IntrinsicsPanel::refreshValidation()
{
    const QJsonObject validation = m_project.value(QStringLiteral("validation")).toObject();
    if (validation.isEmpty()) {
        m_validationStatus->setText(QStringLiteral("等待独立验证"));
        m_validationStats->setText(QStringLiteral("—"));
        m_validationNote->setText(QStringLiteral(
            "使用未参与求解的独立序列，固定内参计算重投影残差。未设置阈值时只报告测量值。"));
        return;
    }
    const QJsonObject stats = validation.value(QStringLiteral("stats")).toObject();
    m_validationStatus->setText(statusText(validation.value(QStringLiteral("status")).toString()));
    m_validationStats->setText(QStringLiteral("P95 %1 px · RMS %2 px · 最大 %3 px")
                                   .arg(fmt(validation.value(QStringLiteral("p95_px")).toDouble()),
                                        fmt(stats.value(QStringLiteral("rms_px")).toDouble()),
                                        fmt(stats.value(QStringLiteral("max_px")).toDouble())));
    const double gate = m_project.value(QStringLiteral("config")).toObject()
                            .value(QStringLiteral("acceptance")).toObject()
                            .value(QStringLiteral("intrinsics_p95_px")).toDouble();
    m_validationNote->setText(gate > 0
                                  ? QStringLiteral("内参 P95 阈值：%1 px。").arg(fmt(gate))
                                  : QStringLiteral("尚未设置验收阈值；当前为测量结果，不生成通过判定。"));
}

void IntrinsicsPanel::onSolve()
{
    runBackground(QStringLiteral("正在求解 Spike 内参…"), [workbench = m_workbench, id = projectId()] {
        workbench->solve(id);
    }, QStringLiteral("Spike 内参已求解。"));
}

void IntrinsicsPanel::onValidate()
{
    runBackground(QStringLiteral("正在使用独立序列验证…"), [workbench = m_workbench, id = projectId()] {
        workbench->validate(id);
    }, QStringLiteral("独立验证完成，请查看实测值与阈值判定。"));
}

void IntrinsicsPanel::onQuery()
{
    m_queryResult->setText(QStringLiteral("方位右正 · 俯仰上正"));
    run(QStringLiteral("正在计算视线…"), [this] {
        const double u = lineToNumber(m_queryU, QStringLiteral("u"));
        const double v = lineToNumber(m_queryV, QStringLiteral("v"));
        const QJsonValue encoder = lineToOptionalNumber(m_queryEncoder, QStringLiteral("转台编码角"));
        const QJsonObject result = m_workbench->query(
            projectId(), QJsonArray{QJsonArray{QJsonValue(u), QJsonValue(v)}}, encoder);
        const QJsonObject point = result.value(QStringLiteral("points")).toArray().at(0).toObject();
        QString text = QStringLiteral("方位角 %1° · 俯仰角 %2°")
                           .arg(fmt(point.value(QStringLiteral("azimuth_deg")).toDouble()),
                                fmt(point.value(QStringLiteral("elevation_deg")).toDouble()));
        if (point.contains(QStringLiteral("world_azimuth_deg"))) {
            text += QStringLiteral(" · 机构方位角 %1°")
                        .arg(fmt(point.value(QStringLiteral("world_azimuth_deg")).toDouble()));
        }
        m_queryResult->setText(text);
    });
}

} // namespace ui
} // namespace spike

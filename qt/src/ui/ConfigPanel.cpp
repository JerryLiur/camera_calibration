#include "ConfigPanel.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

namespace spike {
namespace ui {

ConfigPanel::ConfigPanel(Workbench* workbench, QWidget* parent)
    : ProjectPanel(workbench, parent)
{
    auto* root = new QHBoxLayout(this);
    auto* left = new QVBoxLayout();
    auto* targetGroup = makeGroup(QStringLiteral("标定板与图像方向"), this);
    auto* targetForm = new QFormLayout(targetGroup);
    m_kind = new QComboBox(targetGroup);
    m_kind->addItem(QStringLiteral("棋盘格"), QStringLiteral("chessboard"));
    m_kind->addItem(QStringLiteral("对称圆点阵"), QStringLiteral("circles"));
    m_kind->addItem(QStringLiteral("非对称圆点阵"), QStringLiteral("asymmetric_circles"));
    m_spacing = new QLineEdit(QStringLiteral("0.025"), targetGroup);
    m_cols = new QLineEdit(QStringLiteral("9"), targetGroup);
    m_rows = new QLineEdit(QStringLiteral("6"), targetGroup);
    m_rotation = new QComboBox(targetGroup);
    for (int angle : {0, 90, 180, 270}) m_rotation->addItem(QStringLiteral("%1°").arg(angle), angle);
    m_flipX = new QCheckBox(QStringLiteral("水平镜像"), targetGroup);
    m_flipY = new QCheckBox(QStringLiteral("垂直镜像"), targetGroup);
    auto* flips = new QHBoxLayout();
    flips->addWidget(m_flipX);
    flips->addWidget(m_flipY);
    flips->addStretch(1);
    addRow(targetForm, QStringLiteral("标定板类型"), m_kind);
    addRow(targetForm, QStringLiteral("角点 / 圆心间距 / m"), m_spacing);
    addRow(targetForm, QStringLiteral("列数（棋盘格为内角点）"), m_cols);
    addRow(targetForm, QStringLiteral("行数（棋盘格为内角点）"), m_rows);
    addRow(targetForm, QStringLiteral("方向旋转"), m_rotation);
    targetForm->addRow(QStringLiteral("镜像"), flips);
    targetForm->addRow(new QLabel(QStringLiteral(
        "初始 9 × 6 内角点、0.025 m 间距仅为可编辑示例，请按实际标定板核验填写。"), targetGroup));
    left->addWidget(targetGroup);

    auto* opticsGroup = makeGroup(QStringLiteral("光路装配记录（只标定 Spike）"), this);
    auto* opticsForm = new QFormLayout(opticsGroup);
    m_lens = new QLineEdit(opticsGroup);
    m_focus = new QLineEdit(opticsGroup);
    m_splitter = new QLineEdit(opticsGroup);
    addRow(opticsForm, QStringLiteral("镜头编号"), m_lens);
    addRow(opticsForm, QStringLiteral("对焦记录"), m_focus);
    addRow(opticsForm, QStringLiteral("分光器安装记录"), m_splitter);
    opticsForm->addRow(new QLabel(QStringLiteral(
        "目标 → 共用镜头 → 1:1 分光器 → Spike / RGB。RGB 保持正式安装，本工程只使用 Spike 支路求解。"), opticsGroup));
    left->addWidget(opticsGroup);
    root->addLayout(left, 3);

    auto* right = new QVBoxLayout();
    auto* acceptanceGroup = makeGroup(QStringLiteral("验收阈值与操作人"), this);
    auto* acceptanceForm = new QFormLayout(acceptanceGroup);
    m_intrinsicsThreshold = new QLineEdit(acceptanceGroup);
    m_intrinsicsThreshold->setPlaceholderText(QStringLiteral("留空仅报告测量值"));
    m_turntableThreshold = new QLineEdit(acceptanceGroup);
    m_turntableThreshold->setPlaceholderText(QStringLiteral("留空仅报告测量值"));
    m_operator = new QLineEdit(acceptanceGroup);
    addRow(acceptanceForm, QStringLiteral("内参 P95 / px"), m_intrinsicsThreshold);
    addRow(acceptanceForm, QStringLiteral("转台 P95 / °"), m_turntableThreshold);
    addRow(acceptanceForm, QStringLiteral("操作人"), m_operator);
    acceptanceForm->addRow(new QLabel(QStringLiteral(
        "修改标定板、方向或光路记录会清空本工程旧观测及结果；只修改阈值会保留内参，需重新验证。"), acceptanceGroup));
    right->addWidget(acceptanceGroup);
    m_save = makeButton(QStringLiteral("保存工程配置"), this, true);
    right->addWidget(m_save);
    right->addStretch(1);
    root->addLayout(right, 2);

    connect(m_save, &QPushButton::clicked, this, &ConfigPanel::onSave);
}

QJsonObject ConfigPanel::collectConfig() const
{
    QJsonObject target;
    target.insert(QStringLiteral("kind"), m_kind->currentData().toString());
    target.insert(QStringLiteral("cols"), lineToInt(m_cols, QStringLiteral("列数")));
    target.insert(QStringLiteral("rows"), lineToInt(m_rows, QStringLiteral("行数")));
    target.insert(QStringLiteral("square_length_m"), lineToNumber(m_spacing, QStringLiteral("间距")));
    QJsonObject orientation;
    orientation.insert(QStringLiteral("rotation_deg"), m_rotation->currentData().toInt());
    orientation.insert(QStringLiteral("flip_x"), m_flipX->isChecked());
    orientation.insert(QStringLiteral("flip_y"), m_flipY->isChecked());
    QJsonObject optics;
    optics.insert(QStringLiteral("shared_lens"), true);
    optics.insert(QStringLiteral("split_ratio"), QStringLiteral("1:1"));
    optics.insert(QStringLiteral("lens_id"), m_lens->text().trimmed());
    optics.insert(QStringLiteral("focus_note"), m_focus->text().trimmed());
    optics.insert(QStringLiteral("splitter_note"), m_splitter->text().trimmed());
    QJsonObject acceptance;
    acceptance.insert(QStringLiteral("intrinsics_p95_px"),
                      lineToOptionalNumber(m_intrinsicsThreshold, QStringLiteral("内参阈值")));
    acceptance.insert(QStringLiteral("turntable_p95_deg"),
                      lineToOptionalNumber(m_turntableThreshold, QStringLiteral("转台阈值")));
    QJsonObject config;
    config.insert(QStringLiteral("target"), target);
    config.insert(QStringLiteral("orientation"), orientation);
    config.insert(QStringLiteral("optics"), optics);
    config.insert(QStringLiteral("acceptance"), acceptance);
    config.insert(QStringLiteral("operator"), m_operator->text().trimmed());
    return config;
}

void ConfigPanel::setProject(const QJsonObject& project)
{
    m_project = project;
    const QJsonObject config = project.value(QStringLiteral("config")).toObject();
    const QJsonObject target = config.value(QStringLiteral("target")).toObject();
    const QJsonObject orientation = config.value(QStringLiteral("orientation")).toObject();
    const QJsonObject optics = config.value(QStringLiteral("optics")).toObject();
    const QJsonObject acceptance = config.value(QStringLiteral("acceptance")).toObject();
    m_kind->setCurrentIndex(m_kind->findData(target.value(QStringLiteral("kind")).toString()));
    m_cols->setText(QString::number(target.value(QStringLiteral("cols")).toInt()));
    m_rows->setText(QString::number(target.value(QStringLiteral("rows")).toInt()));
    m_spacing->setText(fmt(target.value(QStringLiteral("square_length_m")).toDouble(), 6));
    m_rotation->setCurrentIndex(m_rotation->findData(orientation.value(QStringLiteral("rotation_deg")).toInt()));
    m_flipX->setChecked(orientation.value(QStringLiteral("flip_x")).toBool());
    m_flipY->setChecked(orientation.value(QStringLiteral("flip_y")).toBool());
    m_lens->setText(optics.value(QStringLiteral("lens_id")).toString());
    m_focus->setText(optics.value(QStringLiteral("focus_note")).toString());
    m_splitter->setText(optics.value(QStringLiteral("splitter_note")).toString());
    m_intrinsicsThreshold->setText(acceptance.value(QStringLiteral("intrinsics_p95_px")).isNull()
                                       ? QString() : fmt(acceptance.value(QStringLiteral("intrinsics_p95_px")).toDouble(), 4));
    m_turntableThreshold->setText(acceptance.value(QStringLiteral("turntable_p95_deg")).isNull()
                                      ? QString() : fmt(acceptance.value(QStringLiteral("turntable_p95_deg")).toDouble(), 4));
    m_operator->setText(config.value(QStringLiteral("operator")).toString());
    m_save->setEnabled(hasProject());
}

void ConfigPanel::onSave()
{
    if (!hasProject()) { emit notify(QStringLiteral("请先新建工程。"), true); return; }
    QJsonObject config;
    try {
        config = collectConfig();
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
        return;
    }
    if (!m_project.value(QStringLiteral("captures")).toArray().isEmpty()) {
        const QJsonObject current = m_project.value(QStringLiteral("config")).toObject();
        for (const QString& key : {QStringLiteral("target"), QStringLiteral("orientation"),
                                   QStringLiteral("optics")}) {
            if (config.value(key) != current.value(key)) {
                if (QMessageBox::question(this, QStringLiteral("几何配置已改变"),
                                          QStringLiteral("保存将清空当前采集与标定结果。继续保存？"))
                    != QMessageBox::Yes) return;
                break;
            }
        }
    }
    run(QStringLiteral("正在保存工程配置…"), [this, config] {
        QJsonObject body;
        body.insert(QStringLiteral("config"), config);
        m_workbench->updateConfig(projectId(), body);
        emit notify(QStringLiteral("工程配置已保存，受影响的旧结果已失效。"));
    });
}

} // namespace ui
} // namespace spike

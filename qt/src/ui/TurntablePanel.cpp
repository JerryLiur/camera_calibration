#include "TurntablePanel.h"

#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QVBoxLayout>

namespace spike {
namespace ui {

TurntablePanel::TurntablePanel(Workbench* workbench, QWidget* parent)
    : ProjectPanel(workbench, parent)
{
    auto* root = new QHBoxLayout(this);
    auto* left = new QVBoxLayout();
    auto* conditionGroup = makeGroup(QStringLiteral("观测与参考条件"), this);
    auto* form = new QFormLayout(conditionGroup);
    m_mode = new QComboBox(conditionGroup);
    m_mode->addItem(QStringLiteral("远场固定方向参考"), QStringLiteral("far_field"));
    m_mode->addItem(QStringLiteral("近距离已知几何"), QStringLiteral("finite_distance"));
    m_sign = new QComboBox(conditionGroup);
    m_sign->addItem(QStringLiteral("+1 · 正角向右转"), 1);
    m_sign->addItem(QStringLiteral("−1 · 正角向左转"), -1);
    addRow(form, QStringLiteral("参考模型"), m_mode);
    addRow(form, QStringLiteral("编码角方向"), m_sign);

    m_farWidget = new QWidget(conditionGroup);
    auto* farForm = new QFormLayout(m_farWidget);
    farForm->setContentsMargins(0, 0, 0, 0);
    m_referenceAngle = new QLineEdit(QStringLiteral("0"), m_farWidget);
    m_farConfirmed = new QCheckBox(QStringLiteral("已确认满足远场近似条件"), m_farWidget);
    addRow(farForm, QStringLiteral("固定参考方向 / °"), m_referenceAngle);
    farForm->addRow(QString(), m_farConfirmed);
    form->addRow(m_farWidget);

    m_finiteWidget = new QWidget(conditionGroup);
    auto* finiteForm = new QFormLayout(m_finiteWidget);
    finiteForm->setContentsMargins(0, 0, 0, 0);
    m_targetX = new QLineEdit(QStringLiteral("0"), m_finiteWidget);
    m_targetY = new QLineEdit(QStringLiteral("0"), m_finiteWidget);
    m_targetZ = new QLineEdit(QStringLiteral("1.75"), m_finiteWidget);
    m_offsetX = new QLineEdit(QStringLiteral("0"), m_finiteWidget);
    m_offsetY = new QLineEdit(QStringLiteral("0"), m_finiteWidget);
    m_offsetZ = new QLineEdit(QStringLiteral("0"), m_finiteWidget);
    auto* targetRow = new QHBoxLayout();
    targetRow->addWidget(m_targetX); targetRow->addWidget(m_targetY); targetRow->addWidget(m_targetZ);
    auto* offsetRow = new QHBoxLayout();
    offsetRow->addWidget(m_offsetX); offsetRow->addWidget(m_offsetY); offsetRow->addWidget(m_offsetZ);
    finiteForm->addRow(QStringLiteral("目标位置 X/Y/Z / m"), targetRow);
    finiteForm->addRow(QStringLiteral("入口瞳偏置 X/Y/Z / m"), offsetRow);
    form->addRow(m_finiteWidget);
    left->addWidget(conditionGroup);

    auto* observationGroup = makeGroup(QStringLiteral("编码角观测（CSV / JSON）"), this);
    auto* observationLayout = new QVBoxLayout(observationGroup);
    m_observations = new QPlainTextEdit(observationGroup);
    m_observations->setPlaceholderText(
        QStringLiteral("sequence_id,split,encoder_deg,u,v\ntrain-rotation,train,-9,320.5,240.2"));
    m_observations->setMinimumHeight(220);
    m_observations->setStyleSheet(QStringLiteral("font-family:Consolas,monospace;font-size:12px;"));
    observationLayout->addWidget(m_observations);
    auto* buttons = new QHBoxLayout();
    auto* load = makeButton(QStringLiteral("读取观测文件…"), observationGroup);
    m_fitButton = makeButton(QStringLiteral("标定单轴转台关系"), observationGroup, true);
    buttons->addWidget(load);
    buttons->addStretch(1);
    buttons->addWidget(m_fitButton);
    observationLayout->addLayout(buttons);
    left->addWidget(observationGroup);
    root->addLayout(left, 3);

    auto* right = new QVBoxLayout();
    auto* resultGroup = makeGroup(QStringLiteral("零位关系"), this);
    auto* resultLayout = new QVBoxLayout(resultGroup);
    m_result = new QLabel(QStringLiteral("等待单轴关系标定"), resultGroup);
    m_result->setWordWrap(true);
    resultLayout->addWidget(m_result);
    right->addWidget(resultGroup);
    auto* detailGroup = makeGroup(QStringLiteral("完整转台结果"), this);
    auto* detailLayout = new QVBoxLayout(detailGroup);
    m_detail = new QPlainTextEdit(detailGroup);
    m_detail->setReadOnly(true);
    m_detail->setStyleSheet(QStringLiteral("font-family:Consolas,monospace;font-size:12px;"));
    detailLayout->addWidget(m_detail);
    right->addWidget(detailGroup, 1);
    root->addLayout(right, 2);

    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TurntablePanel::onModeChanged);
    connect(load, &QPushButton::clicked, this, &TurntablePanel::onLoadObservations);
    connect(m_fitButton, &QPushButton::clicked, this, &TurntablePanel::onFit);
    onModeChanged();
}

void TurntablePanel::onModeChanged()
{
    const bool far = m_mode->currentData().toString() == QStringLiteral("far_field");
    m_farWidget->setVisible(far);
    m_finiteWidget->setVisible(!far);
}

QJsonArray TurntablePanel::parseObservations(const QString& text) const
{
    const QString raw = text.trimmed();
    if (raw.isEmpty()) fail(QStringLiteral("请填写转台观测。"));
    if (raw.startsWith(QLatin1Char('[')) || raw.startsWith(QLatin1Char('{'))) {
        const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
        if (document.isArray()) return document.array();
        const QJsonArray list = document.object().value(QStringLiteral("observations")).toArray();
        if (list.isEmpty()) fail(QStringLiteral("转台 JSON 需要为数组或包含 observations 数组。"));
        return list;
    }
    QStringList lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) fail(QStringLiteral("请填写转台观测。"));
    QStringList headers;
    for (const QString& part : lines.takeFirst().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        headers.append(part.trimmed());
    }
    for (const QString& header : {QStringLiteral("sequence_id"), QStringLiteral("split"),
                                  QStringLiteral("encoder_deg"), QStringLiteral("u"), QStringLiteral("v")}) {
        if (!headers.contains(header)) {
            fail(QStringLiteral("CSV 表头应为 sequence_id,split,encoder_deg,u,v。"));
        }
    }
    auto column = [&headers](const QString& name) { return headers.indexOf(name); };
    QJsonArray result;
    for (int i = 0; i < lines.size(); ++i) {
        const QStringList cells = lines.at(i).split(QLatin1Char(','), Qt::KeepEmptyParts);
        if (cells.size() != headers.size()) {
            fail(QStringLiteral("CSV 第 %1 行列数不一致。").arg(i + 2));
        }
        bool okQ = false, okU = false, okV = false;
        const double encoder = cells.at(column(QStringLiteral("encoder_deg"))).trimmed().toDouble(&okQ);
        const double u = cells.at(column(QStringLiteral("u"))).trimmed().toDouble(&okU);
        const double v = cells.at(column(QStringLiteral("v"))).trimmed().toDouble(&okV);
        if (!okQ || !okU || !okV) {
            fail(QStringLiteral("CSV 第 %1 行角度或坐标无效。").arg(i + 2));
        }
        QJsonObject record;
        record.insert(QStringLiteral("sequence_id"),
                      cells.at(column(QStringLiteral("sequence_id"))).trimmed());
        record.insert(QStringLiteral("split"), cells.at(column(QStringLiteral("split"))).trimmed());
        record.insert(QStringLiteral("encoder_deg"), encoder);
        record.insert(QStringLiteral("pixel"), QJsonArray{QJsonValue(u), QJsonValue(v)});
        result.append(record);
    }
    return result;
}

void TurntablePanel::onLoadObservations()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("读取转台观测"), QString(),
                                                      QStringLiteral("观测文件 (*.csv *.txt *.json);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { emit notify(QStringLiteral("无法读取观测文件。"), true); return; }
    const QString text = QString::fromUtf8(file.readAll());
    try {
        parseObservations(text);
        m_observations->setPlainText(text);
        emit notify(QStringLiteral("已读取观测文件，尚未求解。"));
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
    }
}

void TurntablePanel::onFit()
{
    if (!hasProject()) { emit notify(QStringLiteral("请先新建工程。"), true); return; }
    run(QStringLiteral("正在标定单轴转台关系…"), [this] {
        const QJsonArray observations = parseObservations(m_observations->toPlainText());
        QJsonObject reference;
        const QString mode = m_mode->currentData().toString();
        reference.insert(QStringLiteral("mode"), mode);
        reference.insert(QStringLiteral("sign"), m_sign->currentData().toInt());
        if (mode == QStringLiteral("far_field")) {
            reference.insert(QStringLiteral("reference_angle_deg"),
                             lineToNumber(m_referenceAngle, QStringLiteral("固定参考方向")));
            reference.insert(QStringLiteral("far_field_confirmed"), m_farConfirmed->isChecked());
        } else {
            reference.insert(QStringLiteral("target_position_m"),
                             QJsonArray{QJsonValue(lineToNumber(m_targetX, QStringLiteral("目标 X"))),
                                        QJsonValue(lineToNumber(m_targetY, QStringLiteral("目标 Y"))),
                                        QJsonValue(lineToNumber(m_targetZ, QStringLiteral("目标 Z")))});
            reference.insert(QStringLiteral("camera_offset_m"),
                             QJsonArray{QJsonValue(lineToNumber(m_offsetX, QStringLiteral("偏置 X"))),
                                        QJsonValue(lineToNumber(m_offsetY, QStringLiteral("偏置 Y"))),
                                        QJsonValue(lineToNumber(m_offsetZ, QStringLiteral("偏置 Z")))});
        }
        const QJsonObject project = m_workbench->calibrateTurntable(projectId(), observations, reference);
        emit notify(QStringLiteral("单轴转台关系标定完成：%1")
                        .arg(statusText(project.value(QStringLiteral("turntable")).toObject()
                                            .value(QStringLiteral("status")).toString())));
    });
}

void TurntablePanel::setProject(const QJsonObject& project)
{
    m_project = project;
    const QJsonObject fit = project.value(QStringLiteral("turntable")).toObject();
    m_fitButton->setEnabled(hasProject() && !project.value(QStringLiteral("model")).isNull());
    if (fit.isEmpty()) {
        m_result->setText(QStringLiteral("等待单轴关系标定：先完成 Spike 内参，再录入编码角与对应像素。"));
        m_detail->setPlainText(QString());
        return;
    }
    m_result->setText(QStringLiteral("安装零位偏差 %1° · %2 · 独立验证 P95 %3°\n参考：%4 · 编码角正方向 %5")
                          .arg(fmt(fit.value(QStringLiteral("zero_offset_deg")).toDouble()),
                               statusText(fit.value(QStringLiteral("status")).toString()),
                               fmt(fit.value(QStringLiteral("test_p95_deg")).toDouble()),
                               fit.value(QStringLiteral("reference")).toObject()
                                   .value(QStringLiteral("mode")).toString() == QStringLiteral("far_field")
                                   ? QStringLiteral("已确认远场") : QStringLiteral("已知近距离几何"),
                               fit.value(QStringLiteral("sign")).toInt() == -1
                                   ? QStringLiteral("−1") : QStringLiteral("+1")));
    const QJsonObject reference = fit.value(QStringLiteral("reference")).toObject();
    m_mode->setCurrentIndex(reference.value(QStringLiteral("mode")).toString()
                                == QStringLiteral("finite_distance") ? 1 : 0);
    m_sign->setCurrentIndex(fit.value(QStringLiteral("sign")).toInt() == -1 ? 1 : 0);
    if (reference.contains(QStringLiteral("reference_angle_deg"))) {
        m_referenceAngle->setText(fmt(reference.value(QStringLiteral("reference_angle_deg")).toDouble(), 3));
    }
    m_farConfirmed->setChecked(reference.value(QStringLiteral("far_field_confirmed")).toBool());
    m_detail->setPlainText(pretty(fit));
    onModeChanged();
}

} // namespace ui
} // namespace spike

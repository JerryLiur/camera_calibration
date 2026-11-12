#include "UiHelpers.h"

#include <QCursor>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <cmath>

namespace spike {
namespace ui {

QString fmt(double value, int precision)
{
    if (!std::isfinite(value)) return QStringLiteral("—");
    return QString::number(value, 'f', precision);
}

QString pretty(const QJsonValue& value)
{
    if (value.isNull() || value.isUndefined()) return QStringLiteral("—");
    return dumpsJson(value);
}

QString statusText(const QString& status)
{
    if (status == QStringLiteral("empty")) return QStringLiteral("等待采集");
    if (status == QStringLiteral("collecting")) return QStringLiteral("采集检查");
    if (status == QStringLiteral("solved")) return QStringLiteral("已求解");
    if (status == QStringLiteral("validated")) return QStringLiteral("已验证");
    if (status == QStringLiteral("passed")) return QStringLiteral("阈值检查通过");
    if (status == QStringLiteral("failed")) return QStringLiteral("未通过阈值");
    if (status == QStringLiteral("measured")) return QStringLiteral("已测量 · 未设阈值");
    return status.isEmpty() ? QStringLiteral("等待求解") : status;
}

QString evidenceText(const QString& evidence)
{
    if (evidence == QStringLiteral("synthetic")) return QStringLiteral("合成示例");
    if (evidence == QStringLiteral("hardware")) return QStringLiteral("实采数据");
    if (evidence == QStringLiteral("public")) return QStringLiteral("公开数据");
    return QStringLiteral("来源待确认");
}

QPushButton* makeButton(const QString& text, QWidget* parent, bool primary)
{
    auto* button = new QPushButton(text, parent);
    button->setMinimumHeight(34);
    if (primary) button->setStyleSheet(QStringLiteral(
        "QPushButton{background:#1256c0;color:white;border:1px solid #1256c0;border-radius:4px;padding:6px 14px;}"
        "QPushButton:hover{background:#0b449d;}"
        "QPushButton:disabled{background:#e9edf3;color:#8794a5;border-color:#d9dfe7;}"));
    return button;
}

QGroupBox* makeGroup(const QString& title, QWidget* parent)
{
    auto* group = new QGroupBox(title, parent);
    group->setStyleSheet(QStringLiteral(
        "QGroupBox{font-weight:600;border:1px solid #d8e0ea;border-radius:4px;margin-top:14px;padding-top:10px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;}"));
    return group;
}

void addRow(QFormLayout* layout, const QString& label, QWidget* field)
{
    layout->addRow(label, field);
}

QJsonValue lineToOptionalNumber(QLineEdit* field, const QString& name)
{
    const QString text = field->text().trimmed();
    if (text.isEmpty()) return QJsonValue();
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
        throw Error(QStringLiteral("请检查“%1”的数值。").arg(name));
    }
    return QJsonValue(value);
}

double lineToNumber(QLineEdit* field, const QString& name)
{
    const QJsonValue value = lineToOptionalNumber(field, name);
    if (value.isNull() || value.isUndefined()) {
        throw Error(QStringLiteral("请填写“%1”。").arg(name));
    }
    return value.toDouble();
}

int lineToInt(QLineEdit* field, const QString& name)
{
    const double value = lineToNumber(field, name);
    if (std::floor(value) != value) {
        throw Error(QStringLiteral("“%1”必须为整数。").arg(name));
    }
    return static_cast<int>(value);
}

void ProjectPanel::run(const QString& busyMessage, const std::function<void()>& action)
{
    emit notify(busyMessage, false);
    QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    QCoreApplication::processEvents();
    bool ok = false;
    try {
        action();
        ok = true;
    } catch (const Error& error) {
        emit notify(error.text(), true);
    } catch (const std::exception& error) {
        emit notify(QString::fromUtf8(error.what()), true);
    }
    QGuiApplication::restoreOverrideCursor();
    if (ok) emit projectChanged();
}

} // namespace ui
} // namespace spike

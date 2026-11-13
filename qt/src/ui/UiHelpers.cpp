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
#include <QSaveFile>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QThread>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

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
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("primary", primary);
    return button;
}

QGroupBox* makeGroup(const QString& title, QWidget* parent)
{
    auto* group = new QGroupBox(title, parent);
    return group;
}

void addRow(QFormLayout* layout, const QString& label, QWidget* field)
{
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setHorizontalSpacing(16);
    layout->setVerticalSpacing(10);
    field->setAccessibleName(label);
    layout->addRow(label, field);
}

QString workbenchStyleSheet()
{
    // Shared desktop palette: ink #18263b, muted #56677d, accent #215ecb.
    // Keep child widgets transparent so white groups do not get grey label tiles.
    return QStringLiteral(R"QSS(
        QWidget { color:#18263b; font-family:'Microsoft YaHei UI','Microsoft YaHei','Segoe UI'; font-size:13px; }
        QMainWindow, QWidget#workbenchRoot { background:#f2f5fa; }
        QWidget#workspaceHeader { background:#ffffff; border-bottom:1px solid #dbe3ee; }
        QLabel { background:transparent; }
        QLabel#brandTitle { font-size:21px; font-weight:600; color:#152b4c; }
        QLabel#brandSubtitle { font-size:12px; color:#56677d; }
        QLabel#projectState, QLabel#connectionState { background:#edf3fc; color:#2154a5; border:1px solid #cbdaf1; border-radius:12px; padding:5px 12px; }
        QLabel#connectionState { background:#f5f7fa; color:#56677d; border-color:#dbe3ee; }
        QGroupBox { background:#ffffff; border:1px solid #d8e1ed; border-radius:8px; margin-top:16px; padding:16px 12px 12px; font-weight:600; }
        QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 6px; color:#243d60; }
        QTabWidget::pane { border:0; background:#f2f5fa; }
        QTabBar::tab { background:transparent; color:#56677d; border:0; border-bottom:3px solid transparent; padding:12px 18px; margin-right:4px; }
        QTabBar::tab:selected { color:#215ecb; background:#e8effb; border-bottom-color:#215ecb; font-weight:600; }
        QTabBar::tab:hover:!selected { background:#e9eef6; color:#18263b; }
        QTabBar::tab:focus { border-bottom-color:#215ecb; }
        QScrollArea { border:0; background:transparent; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background:#ffffff; border:1px solid #a7b6c9; border-radius:5px; padding:7px 9px; min-height:18px; selection-background-color:#215ecb; selection-color:#ffffff; }
        QComboBox { padding-right:24px; }
        QComboBox::drop-down { width:22px; border:0; }
        QComboBox::down-arrow { image:url(:/ui/chevron-down.png); width:14px; height:14px; }
        QComboBox QAbstractItemView { background:#ffffff; color:#18263b; selection-background-color:#e6efff; selection-color:#174da8; border:1px solid #a7b6c9; }
        QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border:1px solid #215ecb; background:#f8fbff; }
        QLineEdit:disabled, QComboBox:disabled { background:#eef2f7; color:#78869a; border-color:#d4dce8; }
        QPushButton, QToolButton { background:#ffffff; color:#283e5c; border:1px solid #a7b6c9; border-radius:5px; padding:5px 13px; }
        QPushButton:hover, QToolButton:hover { background:#edf3ff; border-color:#215ecb; color:#174da8; }
        QPushButton:pressed, QToolButton:pressed { background:#dce8fc; }
        QPushButton:focus, QToolButton:focus { border:2px solid #215ecb; padding:4px 12px; }
        QPushButton[primary="true"] { background:#215ecb; color:#ffffff; border-color:#215ecb; font-weight:600; }
        QPushButton[primary="true"]:hover { background:#174da8; }
        QPushButton[primary="true"]:pressed { background:#143f86; }
        QPushButton[primary="true"]:focus { border-color:#102e63; }
        QPushButton:disabled, QToolButton:disabled { background:#e9eef5; color:#7a889a; border-color:#d1dbe8; }
        QCheckBox { spacing:7px; min-height:24px; background:transparent; }
        QCheckBox::indicator { width:16px; height:16px; border:1px solid #8093ad; border-radius:3px; background:#ffffff; }
        QCheckBox::indicator:checked { background:#215ecb; border-color:#215ecb; image:url(:/ui/check.png); }
        QCheckBox::indicator:disabled { background:#e9eef5; border-color:#c0ccdc; }
        QCheckBox::indicator:checked:disabled { background:#7a889a; image:url(:/ui/check.png); }
        QCheckBox:focus { color:#174da8; background:#e8effb; }
        QTableView { background:#ffffff; alternate-background-color:#f4f7fc; border:1px solid #d8e1ed; border-radius:5px; gridline-color:#e4eaf2; selection-background-color:#dfebff; selection-color:#153e7d; }
        QTableView::item { padding:5px; }
        QHeaderView::section { background:#edf2f8; color:#465b76; padding:7px 8px; border:0; border-bottom:1px solid #d8e1ed; font-weight:600; }
        QTableCornerButton::section { background:#edf2f8; border:0; }
        QPlainTextEdit { background:#f8fafd; border:1px solid #b1bed0; border-radius:5px; padding:8px; selection-background-color:#215ecb; selection-color:#ffffff; }
        QSplitter::handle { background:#e2e9f3; width:6px; height:6px; }
        QSplitter::handle:hover { background:#a7bfe6; }
        QScrollBar:vertical { background:#edf1f7; width:12px; margin:0; }
        QScrollBar::handle:vertical { background:#aab8ca; border-radius:5px; min-height:32px; margin:2px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QStatusBar { background:#ffffff; border-top:1px solid #dbe3ee; color:#56677d; padding:4px 12px; }
        QStatusBar::item { border:0; }
        QToolTip { background:#172c4c; color:#ffffff; border:0; padding:7px 10px; }
        QProgressBar { border:1px solid #c6d4e6; border-radius:4px; background:#edf2f8; text-align:center; }
        QProgressBar::chunk { background:#215ecb; }
    )QSS");
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
    if (std::floor(value) != value || value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max()) {
        throw Error(QStringLiteral("“%1”必须为整数。").arg(name));
    }
    return static_cast<int>(value);
}

void saveFileAtomically(const QString& path, const QByteArray& data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        fail(QStringLiteral("无法保存文件 %1：%2").arg(path, file.errorString()));
    }
}

bool ProjectPanel::run(const QString& busyMessage, const std::function<void()>& action)
{
    if (window()->property("spikeBusy").toBool()) {
        emit notify(QStringLiteral("已有操作正在处理，请完成后重试。"), false);
        return false;
    }
    window()->setProperty("spikeBusy", true);
    emit notify(busyMessage, false);
    QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    // Repaint the status without dispatching queued input against mutable project state.
    window()->repaint();
    bool ok = false;
    QString errorMessage;
    try {
        action();
        ok = true;
    } catch (const Error& error) {
        errorMessage = error.text();
    } catch (const std::exception& error) {
        errorMessage = QString::fromUtf8(error.what());
    }
    QGuiApplication::restoreOverrideCursor();
    window()->setProperty("spikeBusy", false);
    if (!errorMessage.isEmpty()) emit notify(errorMessage, true);
    if (ok) emit projectChanged();
    return ok;
}

bool ProjectPanel::runBackground(const QString& busyMessage, const std::function<void()>& action,
                                const QString& completedMessage)
{
    if (window()->property("spikeBusy").toBool()) {
        emit notify(QStringLiteral("已有操作正在处理，请完成后重试。"), false);
        return false;
    }
    // OpenCV does not expose safe cancellation of calibration. Keep the parent
    // modal until the worker finishes; never detach a worker using Workbench.
    class BusyDialog : public QDialog {
    public:
        explicit BusyDialog(QWidget* parent) : QDialog(parent) {}
        void reject() override {}
    };
    window()->setProperty("spikeBusy", true);
    emit notify(busyMessage, false);
    BusyDialog dialog(window());
    dialog.setWindowTitle(QStringLiteral("正在处理"));
    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    dialog.setWindowModality(Qt::ApplicationModal);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(busyMessage, &dialog));
    auto* progress = new QProgressBar(&dialog);
    progress->setRange(0, 0);
    layout->addWidget(progress);
    dialog.setMinimumWidth(360);
    QString errorMessage;
    auto* worker = QThread::create([action, &errorMessage] {
        try { action(); }
        catch (const std::exception& error) { errorMessage = QString::fromUtf8(error.what()); }
        catch (...) { errorMessage = QStringLiteral("后台处理发生未知错误。"); }
    });
    connect(worker, &QThread::finished, &dialog, &QDialog::accept, Qt::QueuedConnection);
    worker->start();
    dialog.exec();
    worker->wait();
    delete worker;
    window()->setProperty("spikeBusy", false);
    if (!errorMessage.isEmpty()) {
        emit notify(errorMessage, true);
        return false;
    }
    emit projectChanged();
    emit notify(completedMessage, false);
    return true;
}

} // namespace ui
} // namespace spike

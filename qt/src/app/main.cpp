#include "core/Workbench.h"
#include "device/CameraDeviceFactory.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>

#ifdef SPIKE_USE_VENDOR_SHIM
#include "VendorSdkShim.h"
#endif

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setApplicationName(QStringLiteral("SpikeCalibrationTool"));
    QApplication::setOrganizationName(QStringLiteral("SpikeCalibration"));
    QApplication::setApplicationVersion(QString::fromUtf8(spike::kVersion));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app/icon.ico")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Spike 单支路标定工作台"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("data"), QStringLiteral("工程保存目录"), QStringLiteral("directory")});
    parser.process(app);
    QString dataRoot = parser.value(QStringLiteral("data"));
    if (dataRoot.isEmpty()) {
        dataRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                       .filePath(QStringLiteral("workspace"));
    }

#ifdef SPIKE_USE_VENDOR_SHIM
    spike::registerVendorSdkShim();
#endif

    try {
        spike::Workbench workbench(dataRoot);
        spike::ui::MainWindow window(&workbench);
        window.show();
        return app.exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, QStringLiteral("无法启动标定工具"),
                              QStringLiteral("工程目录：%1\n%2").arg(dataRoot, QString::fromUtf8(error.what())));
        return 1;
    }
}

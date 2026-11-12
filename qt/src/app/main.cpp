#include "core/Workbench.h"
#include "device/CameraDeviceFactory.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QStringList>

#ifdef SPIKE_USE_VENDOR_SHIM
#include "VendorSdkShim.h"
#endif

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SpikeCalibrationTool"));
    QApplication::setOrganizationName(QStringLiteral("SpikeCalibration"));
    QApplication::setApplicationVersion(QString::fromUtf8(spike::kVersion));

    QString dataRoot;
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i) == QStringLiteral("--data") && i + 1 < arguments.size()) {
            dataRoot = arguments.at(++i);
        }
    }
    if (dataRoot.isEmpty()) {
        dataRoot = QDir(QDir::currentPath()).filePath(QStringLiteral("spike_calibration_workspace"));
    }

#ifdef SPIKE_USE_VENDOR_SHIM
    spike::registerVendorSdkShim();
#endif

    spike::Workbench workbench(dataRoot);
    spike::ui::MainWindow window(&workbench);
    window.show();
    return app.exec();
}

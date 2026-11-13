#include "DeviceController.h"

#include "CameraDeviceFactory.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <QDateTime>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <vector>

namespace spike {
namespace {

QImage toQImage(const cv::Mat& input)
{
    if (input.empty()) return {};
    cv::Mat mat = input;
    if (mat.depth() != CV_8U) {
        cv::Mat converted;
        mat.convertTo(converted, CV_8U);
        mat = converted;
    }
    if (mat.channels() == 1) {
        return QImage(mat.data, mat.cols, mat.rows, static_cast<qsizetype>(mat.step),
                      QImage::Format_Grayscale8).copy();
    }
    if (mat.channels() == 3) {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<qsizetype>(rgb.step),
                      QImage::Format_RGB888).copy();
    }
    if (mat.channels() == 4) {
        cv::Mat rgba;
        cv::cvtColor(mat, rgba, cv::COLOR_BGRA2RGBA);
        return QImage(rgba.data, rgba.cols, rgba.rows, static_cast<qsizetype>(rgba.step),
                      QImage::Format_RGBA8888).copy();
    }
    return {};
}

} // namespace

DeviceWorker::DeviceWorker(QObject* parent) : QObject(parent) {}

bool DeviceWorker::grabInternal(bool still, QString* error)
{
    try {
    if (!m_device || !m_device->isOpen()) {
        if (error) *error = QStringLiteral("设备尚未连接");
        return false;
    }
    DeviceFrame frame;
    QString message;
    if (!m_device->grab(frame, still ? 2000 : 100, &message)) {
        if (error) *error = message.isEmpty() && still ? QStringLiteral("采集超时，请检查设备或触发信号") : message;
        return false;
    }
    const QImage image = toQImage(frame.image);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("不支持的帧格式");
        return false;
    }
    const QString source = frame.metadata.value(QStringLiteral("source")).toString();
    emit frameReady(image, frame.frameIndex, source);
    if (still) {
        std::vector<uchar> buffer;
        if (!cv::imencode(".png", frame.image, buffer)) {
            if (error) *error = QStringLiteral("无法编码采集帧");
            return false;
        }
        const QString name = QStringLiteral("device_%1_%2.png")
                                 .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss")))
                                 .arg(frame.frameIndex);
        emit stillCaptured(QByteArray(reinterpret_cast<const char*>(buffer.data()),
                                      static_cast<int>(buffer.size())),
                           name, source);
    }
    return true;
    } catch (const std::exception& exception) {
        if (error) *error = QStringLiteral("设备采集异常：%1").arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        if (error) *error = QStringLiteral("设备采集发生未知异常");
    }
    return false;
}

void DeviceWorker::openDevice(const QString& backend, const QVariantMap& parameters)
{
    stopStream();
    try {
        if (m_device) m_device->close();
        m_device.reset();
        m_device = createCameraDevice(backend);
        QString message;
        const bool ok = m_device->open(parameters, &message);
        if (!ok) {
            m_device.reset();
            emit opened(false, message, QVariantMap());
            return;
        }
        emit opened(true, message, m_device->parameters());
    } catch (const std::exception& exception) {
        m_device.reset();
        emit opened(false, QString::fromUtf8(exception.what()), QVariantMap());
    } catch (...) {
        m_device.reset();
        emit opened(false, QStringLiteral("设备连接发生未知异常"), QVariantMap());
    }
}

void DeviceWorker::closeDevice()
{
    stopStream();
    try {
        if (m_device) m_device->close();
    } catch (const std::exception& exception) {
        emit errorOccurred(QString::fromUtf8(exception.what()));
    } catch (...) {
        emit errorOccurred(QStringLiteral("设备断开发生未知异常"));
    }
    m_device.reset();
    emit closed(QStringLiteral("设备已断开"));
}

void DeviceWorker::setParameter(const QString& key, const QVariant& value)
{
    try {
    if (!m_device || !m_device->isOpen()) {
        emit errorOccurred(QStringLiteral("设备尚未连接"));
        return;
    }
    QString message;
    if (!m_device->setParameter(key, value, &message)) {
        emit errorOccurred(message);
        return;
    }
    emit parametersChanged(m_device->parameters());
    } catch (const std::exception& exception) {
        emit errorOccurred(QString::fromUtf8(exception.what()));
    } catch (...) {
        emit errorOccurred(QStringLiteral("设备参数设置发生未知异常"));
    }
}

void DeviceWorker::startStream(int intervalMs)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, [this] {
            QString error;
            if (!grabInternal(false, &error) && !error.isEmpty()) {
                stopStream();
                emit errorOccurred(error);
            }
        });
    }
    m_timer->setInterval(std::max(10, intervalMs));
    m_timer->start();
    QString error;
    if (!grabInternal(false, &error) && !error.isEmpty()) {
        stopStream();
        emit errorOccurred(error);
    }
}

void DeviceWorker::stopStream()
{
    if (m_timer) m_timer->stop();
}

void DeviceWorker::grabStill()
{
    QString error;
    if (!grabInternal(true, &error) && !error.isEmpty()) {
        emit errorOccurred(error);
    }
}

DeviceController::DeviceController(QObject* parent) : QObject(parent)
{
    m_worker = new DeviceWorker();
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &DeviceWorker::opened, this,
            [this](bool ok, const QString& message, const QVariantMap& parameters) {
                emit connectionChanged(ok, message);
                if (ok) emit parametersChanged(parameters);
            });
    connect(m_worker, &DeviceWorker::closed, this,
            [this](const QString& message) { emit connectionChanged(false, message); });
    connect(m_worker, &DeviceWorker::frameReady, this,
            [this](const QImage& image, quint64 index, const QString& source) {
                emit frameReady(image, index, source);
            });
    connect(m_worker, &DeviceWorker::stillCaptured, this,
            [this](const QByteArray& png, const QString& name, const QString& message) {
                emit stillCaptured(png, name, message);
            });
    connect(m_worker, &DeviceWorker::errorOccurred, this,
            [this](const QString& message) { emit errorOccurred(message); });
    connect(m_worker, &DeviceWorker::parametersChanged, this,
            [this](const QVariantMap& parameters) { emit parametersChanged(parameters); });
    m_thread.start();
}

DeviceController::~DeviceController()
{
    stopPreview();
    QMetaObject::invokeMethod(m_worker, "closeDevice", Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait(3000);
}

void DeviceController::connectDevice(const QString& backend, const QVariantMap& parameters)
{
    QMetaObject::invokeMethod(m_worker, "openDevice", Qt::QueuedConnection,
                              Q_ARG(QString, backend), Q_ARG(QVariantMap, parameters));
}

void DeviceController::disconnectDevice()
{
    QMetaObject::invokeMethod(m_worker, "closeDevice", Qt::QueuedConnection);
}

void DeviceController::setParameter(const QString& key, const QVariant& value)
{
    QMetaObject::invokeMethod(m_worker, "setParameter", Qt::QueuedConnection,
                              Q_ARG(QString, key), Q_ARG(QVariant, value));
}

void DeviceController::startPreview(int intervalMs)
{
    QMetaObject::invokeMethod(m_worker, "startStream", Qt::QueuedConnection,
                              Q_ARG(int, intervalMs));
}

void DeviceController::stopPreview()
{
    QMetaObject::invokeMethod(m_worker, "stopStream", Qt::QueuedConnection);
}

void DeviceController::captureStill()
{
    QMetaObject::invokeMethod(m_worker, "grabStill", Qt::QueuedConnection);
}

} // namespace spike

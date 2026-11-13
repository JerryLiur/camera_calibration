#include "VendorSdkCameraDevice.h"

#include <opencv2/imgproc.hpp>

#include <QDateTime>

#include <algorithm>
#include <cstring>

namespace spike {

VendorSdkCameraDevice::VendorSdkCameraDevice(VendorSdkApi api) : m_api(api) {}

bool VendorSdkCameraDevice::open(const QVariantMap& parameters, QString* error)
{
    auto failWith = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!m_api.open || !m_api.grab) {
        return failWith(QStringLiteral("厂商 SDK 适配器未注册 open/grab 回调"));
    }
    m_parameters = parameters;
    m_parameters.insert(QStringLiteral("max_width"),
                        parameters.value(QStringLiteral("max_width"), 4096));
    m_parameters.insert(QStringLiteral("max_height"),
                        parameters.value(QStringLiteral("max_height"), 3000));
    m_parameters.insert(QStringLiteral("channels"), parameters.value(QStringLiteral("channels"), 1));
    const int width = m_parameters.value(QStringLiteral("max_width")).toInt();
    const int height = m_parameters.value(QStringLiteral("max_height")).toInt();
    const int channels = m_parameters.value(QStringLiteral("channels")).toInt();
    // Only packed 8-bit 1/3/4-channel frames are mapped onto cv::Mat. A 2-channel
    // (e.g. YUY2-style) buffer has no matching cv::Mat type here, so it must be
    // rejected rather than reinterpreted as 4 channels and read past the buffer.
    if (width <= 0 || height <= 0 || (channels != 1 && channels != 3 && channels != 4)
        || static_cast<qint64>(width) * height * channels > 100000000LL) {
        return failWith(QStringLiteral("SDK 帧缓冲尺寸无效：仅支持 1/3/4 通道 8 位帧"));
    }
    m_buffer.resize(static_cast<int>(static_cast<qint64>(width) * height * channels));

    const QByteArray connection = parameters.value(QStringLiteral("connection")).toString().toUtf8();
    char message[512]{};
    m_handle = m_api.open(connection.constData(), message, static_cast<int>(sizeof(message)));
    if (!m_handle) {
        return failWith(QStringLiteral("设备连接失败：%1")
                            .arg(QString::fromUtf8(message)));
    }
    // Push the common acquisition controls into the SDK once the handle exists.
    for (const QString& key : {QStringLiteral("exposure_us"), QStringLiteral("gain"),
                               QStringLiteral("trigger_mode"), QStringLiteral("binning")}) {
        if (parameters.contains(key)) {
            QString ignored;
            setParameter(key, parameters.value(key), &ignored);
        }
    }
    m_frameIndex = 0;
    if (error) *error = QStringLiteral("已连接 %1").arg(m_api.version ? QString::fromUtf8(m_api.version())
                                                                     : QStringLiteral("厂商 SDK"));
    return true;
}

void VendorSdkCameraDevice::close()
{
    if (m_handle && m_api.close) m_api.close(m_handle);
    m_handle = nullptr;
}

bool VendorSdkCameraDevice::setParameter(const QString& key, const QVariant& value, QString* error)
{
    auto failWith = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!m_handle) return failWith(QStringLiteral("设备尚未连接"));
    const QByteArray name = key.toUtf8();
    int result = 0;
    if (value.typeId() == QMetaType::Double) {
        if (!m_api.setDouble) return failWith(QStringLiteral("SDK 未提供浮点参数设置"));
        result = m_api.setDouble(m_handle, name.constData(), value.toDouble());
    } else {
        if (!m_api.setInt) return failWith(QStringLiteral("SDK 未提供整型参数设置"));
        result = m_api.setInt(m_handle, name.constData(), value.toInt());
    }
    if (result != 0) return failWith(QStringLiteral("SDK 拒绝参数 %1").arg(key));
    m_parameters.insert(key, value);
    return true;
}

bool VendorSdkCameraDevice::grab(DeviceFrame& frame, int timeoutMs, QString* error)
{
    auto failWith = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!m_handle) return failWith(QStringLiteral("设备尚未连接"));
    int width = 0, height = 0, channels = 1;
    const int result = m_api.grab(m_handle,
                                  reinterpret_cast<unsigned char*>(m_buffer.data()),
                                  m_buffer.size(), &width, &height, &channels, timeoutMs);
    if (result == 1) return false;   // timeout is not an error for a live stream
    if (result < 0) return failWith(QStringLiteral("SDK 采集失败（代码 %1）").arg(result));
    if (width <= 0 || height <= 0) {
        return failWith(QStringLiteral("SDK 返回了无效的帧尺寸"));
    }
    if (channels != 1 && channels != 3 && channels != 4) {
        return failWith(QStringLiteral("SDK 返回了不支持的 %1 通道帧；仅支持 1/3/4 通道 8 位帧")
                            .arg(channels));
    }
    if (static_cast<qint64>(width) * height * channels > m_buffer.size()) {
        return failWith(QStringLiteral("SDK 返回了超缓冲区的帧尺寸"));
    }
    const int type = channels == 1 ? CV_8UC1 : channels == 3 ? CV_8UC3 : CV_8UC4;
    cv::Mat raw(height, width, type, m_buffer.data());
    if (channels == 4) {
        cv::cvtColor(raw, frame.image, cv::COLOR_BGRA2BGR);
    } else {
        frame.image = raw.clone();
    }
    ++m_frameIndex;
    frame.frameIndex = m_frameIndex;
    frame.timestampNs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ULL;
    frame.metadata.insert(QStringLiteral("width"), width);
    frame.metadata.insert(QStringLiteral("height"), height);
    frame.metadata.insert(QStringLiteral("channels"), channels);
    frame.metadata.insert(QStringLiteral("backend"), backend());
    return true;
}

} // namespace spike

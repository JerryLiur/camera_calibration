#include "FileCameraDevice.h"

#include "core/Importers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariant>

#include <opencv2/imgcodecs.hpp>

#include <algorithm>

namespace spike {
namespace {

QJsonValue variantToJson(const QVariant& value)
{
    if (value.typeId() == QMetaType::Bool) return QJsonValue(value.toBool());
    if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong)
        return QJsonValue(static_cast<qint64>(value.toLongLong()));
    if (value.typeId() == QMetaType::Double) return QJsonValue(value.toDouble());
    if (value.typeId() == QMetaType::QString) return QJsonValue(value.toString());
    return QJsonValue();
}

} // namespace

bool FileCameraDevice::open(const QVariantMap& parameters, QString* error)
{
    auto failWith = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString path = parameters.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) return failWith(QStringLiteral("请选择回放目录或单个采集文件"));
    // Re-opening replaces the playlist; appending here would duplicate every
    // frame after a reconnect and break the replay length.
    m_files.clear();
    m_decodeOptions = QJsonObject();
    const QFileInfo info(path);
    const QStringList suffixes = {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                  QStringLiteral("*.jpeg"), QStringLiteral("*.bmp"),
                                  QStringLiteral("*.tif"), QStringLiteral("*.tiff"),
                                  QStringLiteral("*.npy"), QStringLiteral("*.dat"),
                                  QStringLiteral("*.bin")};
    if (info.isDir()) {
        const QFileInfoList entries = QDir(path).entryInfoList(suffixes, QDir::Files, QDir::Name);
        for (const QFileInfo& entry : entries) m_files.append(entry.absoluteFilePath());
    } else if (info.isFile()) {
        m_files.append(info.absoluteFilePath());
    } else {
        return failWith(QStringLiteral("回放路径不存在"));
    }
    if (m_files.isEmpty()) return failWith(QStringLiteral("回放目录中没有支持的采集文件"));

    m_parameters = parameters;
    m_loop = parameters.value(QStringLiteral("loop"), true).toBool();
    m_index = 0;
    m_frameIndex = 0;
    for (const QString& key : {QStringLiteral("method"), QStringLiteral("period_s"),
                               QStringLiteral("window_start_tick"), QStringLiteral("window_ticks"),
                               QStringLiteral("layout"), QStringLiteral("layout_confirmed"),
                               QStringLiteral("bitorder"), QStringLiteral("width"),
                               QStringLiteral("height"), QStringLiteral("header_bytes"),
                               QStringLiteral("frame_stride_bytes"), QStringLiteral("black_level"),
                               QStringLiteral("white_level")}) {
        if (parameters.contains(key)) {
            const QJsonValue value = variantToJson(parameters.value(key));
            if (!value.isNull() && !value.isUndefined()) m_decodeOptions.insert(key, value);
        }
    }
    if (error) *error = QStringLiteral("离线回放已就绪：%1 个文件").arg(m_files.size());
    return true;
}

void FileCameraDevice::close()
{
    m_files.clear();
    m_index = 0;
}

QVariantMap FileCameraDevice::parameters() const
{
    QVariantMap result = m_parameters;
    result.insert(QStringLiteral("loop"), m_loop);
    result.insert(QStringLiteral("frame_index"), m_index);
    result.insert(QStringLiteral("frame_count"), m_files.size());
    return result;
}

bool FileCameraDevice::setParameter(const QString& key, const QVariant& value, QString* error)
{
    if (key == QStringLiteral("loop")) {
        m_loop = value.toBool();
        m_parameters.insert(key, m_loop);
        return true;
    }
    if (key == QStringLiteral("restart")) {
        m_index = 0;
        return true;
    }
    if (key == QStringLiteral("frame_interval_ms")) {
        m_parameters.insert(key, value.toInt());
        return true;
    }
    if (m_decodeOptions.contains(key)) {
        m_decodeOptions.insert(key, variantToJson(value));
        m_parameters.insert(key, value);
        return true;
    }
    if (error) *error = QStringLiteral("离线回放不支持参数 %1").arg(key);
    return false;
}

bool FileCameraDevice::grab(DeviceFrame& frame, int, QString* error)
{
    if (m_files.isEmpty()) {
        if (error) *error = QStringLiteral("设备尚未连接");
        return false;
    }
    if (m_index >= m_files.size()) {
        if (!m_loop) {
            if (error) *error = QStringLiteral("离线回放已结束");
            return false;
        }
        m_index = 0;
    }
    const QString path = m_files.at(m_index++);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取 %1").arg(path);
        return false;
    }
    if (file.size() <= 0 || file.size() > 64 * 1024 * 1024) {
        if (error) *error = QStringLiteral("回放文件为空或超过64MiB导入限制");
        return false;
    }
    const QByteArray bytes = file.readAll();
    const QString suffix = QStringLiteral(".") + QFileInfo(path).suffix().toLower();
    try {
        if (suffix == QStringLiteral(".npy") || suffix == QStringLiteral(".dat")
            || suffix == QStringLiteral(".bin")) {
            frame.image = reconstructFrame(bytes, suffix, m_decodeOptions);
        } else {
            frame.image = decodeImageBytes(bytes, cv::IMREAD_UNCHANGED);
        }
    } catch (const std::exception& exception) {
        if (error) *error = QStringLiteral("解码失败：%1").arg(QString::fromUtf8(exception.what()));
        return false;
    }
    ++m_frameIndex;
    frame.frameIndex = m_frameIndex;
    frame.timestampNs = m_frameIndex * 1000000ULL;
    frame.metadata.insert(QStringLiteral("source"), QFileInfo(path).fileName());
    frame.metadata.insert(QStringLiteral("backend"), backend());
    return true;
}

} // namespace spike

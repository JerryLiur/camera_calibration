#include "Importers.h"

#include "Targets.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace spike {
namespace {

struct RawWindow {
    cv::Mat image;
    QJsonObject source;
};

int optionInt(const QJsonObject& options, const QString& key, int defaultValue, int minimum = 0)
{
    const QJsonValue value = options.value(key);
    if (isNone(value)) {
        require(defaultValue >= minimum,
                QStringLiteral("%1 必须为不小于 %2 的整数").arg(key).arg(minimum));
        return defaultValue;
    }
    const int result = requireInt(value, key);
    require(result >= minimum, QStringLiteral("%1 必须为不小于 %2 的整数").arg(key).arg(minimum));
    return result;
}

std::optional<double> optionDouble(const QJsonObject& options, const QString& key)
{
    const QJsonValue value = options.value(key);
    if (isNone(value)) return std::nullopt;
    return requireNumber(value, key);
}

std::pair<int, int> windowBounds(const QJsonObject& options, qint64 totalTicks, qint64 spatialPixels)
{
    const int start = optionInt(options, QStringLiteral("window_start_tick"), 0);
    const qint64 remaining = totalTicks - start;
    const qint64 limit = 100000000LL / std::max<qint64>(spatialPixels, 1);
    const qint64 defaultTicks = std::min<qint64>({256LL, remaining, limit});
    const int safeDefault = static_cast<int>(std::max<qint64>(1, defaultTicks));
    const int ticks = optionInt(options, QStringLiteral("window_ticks"), safeDefault, 1);
    require(static_cast<qint64>(start) + ticks <= totalTicks
                && static_cast<qint64>(ticks) * spatialPixels <= 100000000LL,
            QStringLiteral("重建时间窗超出数据范围或超过一亿个脉冲元素"));
    return {start, ticks};
}

double readElement(const uchar* pointer, char kind, int itemsize, bool littleEndian)
{
    quint64 raw = 0;
    if (littleEndian) {
        for (int i = 0; i < itemsize; ++i) raw |= static_cast<quint64>(pointer[i]) << (8 * i);
    } else {
        for (int i = 0; i < itemsize; ++i) raw = (raw << 8) | pointer[i];
    }
    if (kind == 'b') return raw != 0 ? 1.0 : 0.0;
    if (kind == 'u') return static_cast<double>(raw);
    if (kind == 'i') {
        const int bits = itemsize * 8;
        qint64 signedValue = static_cast<qint64>(raw);
        if (bits < 64 && (raw & (1ULL << (bits - 1)))) signedValue -= (1LL << bits);
        return static_cast<double>(signedValue);
    }
    if (kind == 'f') {
        if (itemsize == 4) {
            const auto bits = static_cast<quint32>(raw);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return static_cast<double>(value);
        }
        if (itemsize == 8) {
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(value));
            return value;
        }
    }
    fail(QStringLiteral("无法读取不含对象的二值Spike NPY"));
}

struct NpyInfo {
    int ticks = 0;
    int height = 0;
    int width = 0;
    int itemsize = 1;
    char kind = 'u';
    bool littleEndian = true;
    int dataOffset = 0;
};

NpyInfo parseNpy(const QByteArray& bytes)
{
    static const QByteArray magic = QByteArray::fromHex("93") + QByteArray("NUMPY");
    require(bytes.size() >= 12 && bytes.left(6) == magic,
            QStringLiteral("无法读取不含对象的二值Spike NPY"));
    const int major = static_cast<uchar>(bytes.at(6));
    const int headerStart = major == 1 ? 10 : 12;
    quint32 headerLength = 0;
    if (major == 1) {
        headerLength = static_cast<uchar>(bytes.at(8)) | (static_cast<uchar>(bytes.at(9)) << 8);
    } else if (major == 2 || major == 3) {
        headerLength = static_cast<uchar>(bytes.at(8))
            | (static_cast<uchar>(bytes.at(9)) << 8)
            | (static_cast<uchar>(bytes.at(10)) << 16)
            | (static_cast<quint32>(static_cast<uchar>(bytes.at(11))) << 24);
    } else {
        fail(QStringLiteral("无法读取不含对象的二值Spike NPY"));
    }
    require(headerStart + static_cast<int>(headerLength) <= bytes.size(),
            QStringLiteral("无法读取不含对象的二值Spike NPY"));
    const QString header = QString::fromLatin1(bytes.mid(headerStart, static_cast<int>(headerLength)));
    require(!header.contains(QStringLiteral("fortran_order': True"))
                && !header.contains(QStringLiteral("fortran_order\": True")),
            QStringLiteral("无法读取不含对象的二值Spike NPY（不支持 Fortran 顺序）"));

    QRegularExpression descrRx(QStringLiteral("'descr'\\s*:\\s*'([^']+)'"));
    const QRegularExpressionMatch descrMatch = descrRx.match(header);
    require(descrMatch.hasMatch(), QStringLiteral("无法读取不含对象的二值Spike NPY"));
    const QString descr = descrMatch.captured(1);
    require(descr.size() >= 2, QStringLiteral("无法读取不含对象的二值Spike NPY"));
    const char endian = descr.at(0).toLatin1();
    const char kind = descr.at(1).toLatin1();
    bool itemsizeOk = false;
    const int itemsize = descr.mid(2).toInt(&itemsizeOk);
    require(itemsizeOk && itemsize >= 1 && itemsize <= 8
                && (kind == 'b' || kind == 'u' || kind == 'i' || kind == 'f'),
            QStringLiteral("无法读取不含对象的二值Spike NPY"));
    require((kind == 'f' && (itemsize == 4 || itemsize == 8))
                || (kind != 'f' && (itemsize == 1 || itemsize == 2 || itemsize == 4 || itemsize == 8)),
            QStringLiteral("无法读取不含对象的二值Spike NPY"));

    QRegularExpression shapeRx(QStringLiteral("'shape'\\s*:\\s*\\(([^)]*)\\)"));
    const QRegularExpressionMatch shapeMatch = shapeRx.match(header);
    require(shapeMatch.hasMatch(), QStringLiteral("无法读取不含对象的二值Spike NPY"));
    QRegularExpression numberRx(QStringLiteral("\\d+"));
    std::vector<int> shape;
    auto it = numberRx.globalMatch(shapeMatch.captured(1));
    while (it.hasNext()) shape.push_back(it.next().captured().toInt());
    require(shape.size() == 3 && shape[0] > 0 && shape[1] > 0 && shape[2] > 0,
            QStringLiteral("Spike NPY 必须是二值 [T,H,W] 数组，最多一亿个元素"));

    NpyInfo info;
    info.ticks = shape[0];
    info.height = shape[1];
    info.width = shape[2];
    info.itemsize = itemsize;
    info.kind = kind;
    info.littleEndian = endian != '>';
    info.dataOffset = headerStart + static_cast<int>(headerLength);
    const qint64 elements = static_cast<qint64>(shape[0]) * shape[1] * shape[2];
    require(elements <= 100000000LL
                && static_cast<qint64>(shape[1]) * shape[2] <= 25000000LL,
            QStringLiteral("Spike NPY 必须是二值 [T,H,W] 数组，最多一亿个元素"));
    require(info.dataOffset + elements * itemsize <= bytes.size(),
            QStringLiteral("无法读取不含对象的二值Spike NPY"));
    return info;
}

std::vector<uchar> decodeNpyWindow(const QByteArray& bytes, const NpyInfo& info,
                                   int start, int ticks)
{
    const size_t pixels = static_cast<size_t>(info.height) * info.width;
    std::vector<uchar> window(static_cast<size_t>(ticks) * pixels, 0);
    for (int t = 0; t < ticks; ++t) {
        const qint64 frame = static_cast<qint64>(start) + t;
        for (size_t p = 0; p < pixels; ++p) {
            const qint64 index = frame * static_cast<qint64>(pixels) + static_cast<qint64>(p);
            const uchar* pointer = reinterpret_cast<const uchar*>(bytes.constData())
                + info.dataOffset + index * info.itemsize;
            const double value = readElement(pointer, info.kind, info.itemsize, info.littleEndian);
            require(std::isfinite(value) && (value == 0.0 || value == 1.0),
                    QStringLiteral("Spike NPY 所选时间窗必须为0/1二值，不能把灰度视频当原始脉冲"));
            window[static_cast<size_t>(t) * pixels + p] = value == 1.0 ? 1 : 0;
        }
    }
    return window;
}

std::vector<uchar> decodePackedWindow(const QByteArray& bytes, int width, int height,
                                      int header, int stride, int minimum,
                                      const QString& layout, const QString& bitorder,
                                      int start, int ticks)
{
    const int rowBytes = (width + 7) / 8;
    std::vector<uchar> window(static_cast<size_t>(ticks) * width * height, 0);
    for (int t = 0; t < ticks; ++t) {
        const uchar* raw = reinterpret_cast<const uchar*>(bytes.constData())
            + header + static_cast<qint64>(start + t) * stride;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int k = y * width + x;
                const int byteIndex = layout == QStringLiteral("packed_flat")
                                          ? k / 8 : y * rowBytes + x / 8;
                const int bitIndex = layout == QStringLiteral("packed_flat") ? k % 8 : x % 8;
                const uchar byte = raw[byteIndex];
                const int shift = bitorder == QStringLiteral("little") ? bitIndex : 7 - bitIndex;
                window[static_cast<size_t>(t) * width * height + k] = (byte >> shift) & 1;
            }
        }
    }
    require(minimum <= stride, QStringLiteral("文件长度与所填头长度/帧步长不匹配；请核实厂商布局"));
    return window;
}

RawWindow decodeRawWindow(const QByteArray& bytes, const QJsonObject& options,
                          const QString& suffix)
{
    const QString method = options.value(QStringLiteral("method")).toString(QStringLiteral("count"));
    const std::optional<double> period = optionDouble(options, QStringLiteral("period_s"));
    const qint64 fileLength = bytes.size();
    require(fileLength > 0 && fileLength <= 64 * 1024 * 1024,
            QStringLiteral("文件为空或超过64MiB导入限制"));

    std::vector<uchar> binary;
    int width = 0, height = 0, ticks = 0, start = 0;
    QJsonObject source;

    if (suffix == QStringLiteral(".npy")) {
        const NpyInfo info = parseNpy(bytes);
        width = info.width;
        height = info.height;
        const auto bounds = windowBounds(options, info.ticks,
                                         static_cast<qint64>(width) * height);
        start = bounds.first;
        ticks = bounds.second;
        binary = decodeNpyWindow(bytes, info, start, ticks);
    } else {
        require(options.value(QStringLiteral("layout_confirmed")).isBool()
                    && options.value(QStringLiteral("layout_confirmed")).toBool(),
                QStringLiteral("DAT/BIN 不猜测布局：请明确 layout_confirmed=true、layout=packed_flat/row_packed 和 bitorder=little/big"));
        const QString layout = options.value(QStringLiteral("layout")).toString();
        const QString bitorder = options.value(QStringLiteral("bitorder")).toString();
        require(layout == QStringLiteral("packed_flat") || layout == QStringLiteral("row_packed"),
                QStringLiteral("DAT/BIN 不猜测布局：请明确 layout_confirmed=true、layout=packed_flat/row_packed 和 bitorder=little/big"));
        require(bitorder == QStringLiteral("little") || bitorder == QStringLiteral("big"),
                QStringLiteral("DAT/BIN 不猜测布局：请明确 layout_confirmed=true、layout=packed_flat/row_packed 和 bitorder=little/big"));
        width = optionInt(options, QStringLiteral("width"), 0, 1);
        height = optionInt(options, QStringLiteral("height"), 0, 1);
        require(static_cast<qint64>(width) * height <= 25000000LL,
                QStringLiteral("DAT/BIN图像尺寸超过2500万像素"));
        const int rowBytes = (width + 7) / 8;
        const int minimum = layout == QStringLiteral("packed_flat")
                                ? static_cast<int>((static_cast<qint64>(width) * height + 7) / 8)
                                : rowBytes * height;
        const int stride = optionInt(options, QStringLiteral("frame_stride_bytes"), minimum, minimum);
        const int header = optionInt(options, QStringLiteral("header_bytes"), 0);
        require(header < fileLength && (fileLength - header) % stride == 0,
                QStringLiteral("文件长度与所填头长度/帧步长不匹配；请核实厂商布局"));
        const int total = static_cast<int>((fileLength - header) / stride);
        const auto bounds = windowBounds(options, total, static_cast<qint64>(width) * height);
        start = bounds.first;
        ticks = bounds.second;
        binary = decodePackedWindow(bytes, width, height, header, stride, minimum,
                                    layout, bitorder, start, ticks);
        source.insert(QStringLiteral("layout"), layout);
        source.insert(QStringLiteral("bitorder"), bitorder);
        source.insert(QStringLiteral("header_bytes"), header);
        source.insert(QStringLiteral("frame_stride_bytes"), stride);
        source.insert(QStringLiteral("total_ticks"), total);
        source.insert(QStringLiteral("source_image_size"),
                      QJsonArray{QJsonValue(width), QJsonValue(height)});
    }

    const SpikeRepresentation representation =
        spikeRepresentation(binary, ticks, height, width, method, period);

    source.insert(QStringLiteral("kind"), QStringLiteral("raw_spike_window"));
    source.insert(QStringLiteral("method"), representation.method);
    source.insert(QStringLiteral("unit"), representation.unit);
    source.insert(QStringLiteral("window_start_tick"), start);
    source.insert(QStringLiteral("window_ticks"), ticks);
    source.insert(QStringLiteral("period_s"), period ? QJsonValue(*period) : QJsonValue());
    source.insert(QStringLiteral("observation_semantics"), representation.semantics);
    source.insert(QStringLiteral("raw_data_modified"), false);
    source.insert(QStringLiteral("spike_quality"), representation.quality);

    RawWindow result;
    result.image = representation.image;
    result.source = source;
    return result;
}

} // namespace

FileRecord FileRecord::fromJson(const QJsonObject& object)
{
    FileRecord record;
    record.name = requireString(object.value(QStringLiteral("name")), QStringLiteral("导入文件名称"));
    const QString content = requireString(object.value(QStringLiteral("content_base64")),
                                          QStringLiteral("文件内容"));
    require(content.size() <= 90000000, QStringLiteral("文件内容缺失或超过64MiB导入限制"));
    const QString payload = content.contains(QLatin1Char(','))
                                ? content.mid(content.indexOf(QLatin1Char(',')) + 1) : content;
    record.content = decodeBase64Strict(payload);
    require(!record.content.isEmpty() && record.content.size() <= 64 * 1024 * 1024,
            QStringLiteral("文件为空或超过64MiB导入限制"));
    return record;
}

cv::Mat decodeImageBytes(const QByteArray& bytes, int flags)
{
    const cv::Mat buffer(1, static_cast<int>(bytes.size()), CV_8UC1,
                         const_cast<char*>(bytes.constData()));
    cv::Mat image = cv::imdecode(buffer, flags);
    require(!image.empty(), QStringLiteral("无法解码图像文件"));
    return image;
}

cv::Mat previewGray(const cv::Mat& image, const QJsonObject& options)
{
    cv::Mat work = image;
    if (work.channels() == 3) {
        cv::cvtColor(work, work, cv::COLOR_BGR2GRAY);
    } else if (work.channels() == 4) {
        cv::cvtColor(work, work, cv::COLOR_BGRA2GRAY);
    }
    require(work.channels() == 1 && !work.empty()
                && static_cast<double>(work.rows) * work.cols <= 25000000.0,
            QStringLiteral("需要有效灰度图像，且不超过2500万像素"));
    cv::Mat data;
    work.convertTo(data, CV_64F);
    double low = 0.0, high = 0.0;
    bool haveLow = false, haveHigh = false;
    const QJsonValue lowValue = options.value(QStringLiteral("black_level"));
    const QJsonValue highValue = options.value(QStringLiteral("white_level"));
    if (!isNone(lowValue)) { low = requireNumber(lowValue, QStringLiteral("黑电平")); haveLow = true; }
    if (!isNone(highValue)) { high = requireNumber(highValue, QStringLiteral("白电平")); haveHigh = true; }
    if (!haveLow || !haveHigh) {
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
        for (int r = 0; r < data.rows; ++r) {
            const double* row = data.ptr<double>(r);
            for (int c = 0; c < data.cols; ++c) {
                require(std::isfinite(row[c]), QStringLiteral("需要有效灰度图像，且不超过2500万像素"));
                minimum = std::min(minimum, row[c]);
                maximum = std::max(maximum, row[c]);
            }
        }
        if (!haveLow) low = minimum;
        if (!haveHigh) high = maximum;
    }
    require(std::isfinite(low) && std::isfinite(high), QStringLiteral("预览黑白电平必须为有限数值"));
    if (high <= low) {
        if (haveLow || haveHigh) fail(QStringLiteral("预览白电平必须大于黑电平"));
        return cv::Mat(data.size(), CV_8U, cv::Scalar(0));
    }
    cv::Mat normalized = (data - low) / (high - low) * 255.0;
    cv::Mat result;
    for (int r = 0; r < normalized.rows; ++r) {
        double* row = normalized.ptr<double>(r);
        for (int c = 0; c < normalized.cols; ++c) row[c] = std::clamp(row[c], 0.0, 255.0);
    }
    normalized.convertTo(result, CV_8U);
    return result;
}

cv::Mat reconstructFrame(const QByteArray& bytes, const QString& suffix,
                         const QJsonObject& options)
{
    require(suffix == QStringLiteral(".npy") || suffix == QStringLiteral(".dat")
                || suffix == QStringLiteral(".bin"),
            QStringLiteral("不支持的设备回放文件格式"));
    const RawWindow window = decodeRawWindow(bytes, options, suffix);
    return previewGray(window.image, options);
}

QByteArray encodePng(const cv::Mat& image)
{
    std::vector<uchar> buffer;
    require(cv::imencode(".png", image, buffer), QStringLiteral("无法编码预览图"));
    return QByteArray(reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()));
}

QString encodePreview(const cv::Mat& gray8)
{
    return QString::fromLatin1(encodePng(gray8).toBase64());
}

Capture importCapture(const FileRecord& file, const Config& config, const QString& split,
                       const QString& sequenceId, const QString& evidence,
                       const QJsonObject& options)
{
    QString name = file.name;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1);
    require(!name.isEmpty() && name.size() <= 500, QStringLiteral("文件名无效"));
    const QString suffix = QStringLiteral(".") + QFileInfo(name).suffix().toLower();
    cv::Mat image;
    QJsonObject source;
    if (suffix == QStringLiteral(".npy") || suffix == QStringLiteral(".dat")
        || suffix == QStringLiteral(".bin")) {
        const RawWindow window = decodeRawWindow(file.content, options, suffix);
        image = window.image;
        source = window.source;
    } else if (suffix == QStringLiteral(".png") || suffix == QStringLiteral(".jpg")
               || suffix == QStringLiteral(".jpeg") || suffix == QStringLiteral(".bmp")
               || suffix == QStringLiteral(".tif") || suffix == QStringLiteral(".tiff")) {
        image = decodeImageBytes(file.content, cv::IMREAD_UNCHANGED);
        source.insert(QStringLiteral("kind"), QStringLiteral("image"));
        source.insert(QStringLiteral("note"),
                      QStringLiteral("导入图像仅用于几何标定，不把8位预览当辐射量"));
    } else {
        fail(QStringLiteral("支持PNG/JPEG/BMP/TIFF、二值Spike NPY及已知布局DAT/BIN；角点JSON请使用观测导入"));
    }

    const cv::Mat gray = applyOrientation(previewGray(image, options), config.orientation);
    const TargetResult target = detectTarget(gray, targetConfigJson(config));
    QJsonObject quality = target.quality;
    QJsonObject metrics = quality.value(QStringLiteral("metrics")).toObject();
    if (metrics.contains(QStringLiteral("saturated_fraction"))) {
        metrics.insert(QStringLiteral("preview_white_endpoint_fraction"),
                       metrics.take(QStringLiteral("saturated_fraction")));
    }
    if (metrics.contains(QStringLiteral("black_fraction"))) {
        metrics.insert(QStringLiteral("preview_black_endpoint_fraction"),
                       metrics.take(QStringLiteral("black_fraction")));
    }
    quality.insert(QStringLiteral("metrics"), metrics);
    QJsonArray suggestions;
    for (const QJsonValue& item : quality.value(QStringLiteral("suggestions")).toArray()) {
        if (!item.toString().contains(QStringLiteral("剪切"))) suggestions.append(item);
    }
    quality.insert(QStringLiteral("suggestions"), suggestions);
    quality.insert(QStringLiteral("scope"),
                   QStringLiteral("预览灰度已按显示电平拉伸，端点占比不代表传感器饱和；仅作几何角点质量诊断"));

    source.insert(QStringLiteral("sha256"), sha256Hex(file.content));
    source.insert(QStringLiteral("bytes"), static_cast<qint64>(file.content.size()));
    source.insert(QStringLiteral("orientation"), orientationJson(config));

    QJsonObject capture;
    capture.insert(QStringLiteral("id"), makeId());
    capture.insert(QStringLiteral("name"), name);
    capture.insert(QStringLiteral("sequence_id"), sequenceId);
    capture.insert(QStringLiteral("split"), split);
    capture.insert(QStringLiteral("evidence"), evidence);
    capture.insert(QStringLiteral("enabled"), true);
    capture.insert(QStringLiteral("found"), target.found);
    capture.insert(QStringLiteral("image_size"),
                   QJsonArray{QJsonValue(gray.cols), QJsonValue(gray.rows)});
    capture.insert(QStringLiteral("object_points"), point3Array(target.objectPoints));
    capture.insert(QStringLiteral("image_points"), point2Array(target.points));
    capture.insert(QStringLiteral("coordinate_domain"), QStringLiteral("transformed_pixels"));
    capture.insert(QStringLiteral("quality"), quality);
    capture.insert(QStringLiteral("preview_base64"), encodePreview(gray));
    capture.insert(QStringLiteral("source"), source);
    QJsonArray reasons;
    for (const QString& reason : target.reasons) reasons.append(reason);
    capture.insert(QStringLiteral("reasons"), reasons);
    return validateCapture(capture);
}

} // namespace spike

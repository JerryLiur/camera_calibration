#include "Common.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <QCryptographicHash>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>

namespace spike {

void fail(const QString& message) { throw Error(message); }

void require(bool condition, const QString& message)
{
    if (!condition) fail(message);
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
        + QStringLiteral("+00:00");
}

QString makeId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
}

QByteArray sha256Raw(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(sha256Raw(data).toHex());
}

QString sha256HexUtf8(const QString& text)
{
    return sha256Hex(text.toUtf8());
}

QByteArray encodeBase64(const QByteArray& data) { return data.toBase64(); }

QByteArray decodeBase64Strict(const QString& text)
{
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9+/]*={0,2}$"));
    require(allowed.match(text).hasMatch() && text.size() % 4 == 0,
            QStringLiteral("文件内容不是有效Base64"));
    QByteArray decoded = QByteArray::fromBase64(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    require(!decoded.isEmpty(), QStringLiteral("文件内容不是有效Base64"));
    return decoded;
}

bool finiteJson(const QJsonValue& value)
{
    if (value.isDouble()) return std::isfinite(value.toDouble());
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            if (!finiteJson(item)) return false;
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!finiteJson(it.value())) return false;
        }
    }
    return true;
}

QJsonValue strictLoads(const QByteArray& utf8)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(utf8, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        fail(QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString()));
    }
    QJsonValue value = document.isObject() ? QJsonValue(document.object())
                                           : QJsonValue(document.array());
    if (!finiteJson(value)) fail(QStringLiteral("JSON 数值超出有限范围"));
    return value;
}

QString dumpsJson(const QJsonValue& value, bool indented)
{
    if (!finiteJson(value)) fail(QStringLiteral("JSON 不允许非有限数值"));
    const QJsonDocument::JsonFormat format = indented ? QJsonDocument::Indented
                                                      : QJsonDocument::Compact;
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(format));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(format));
    if (value.isNull() || value.isUndefined()) return QStringLiteral("null");
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) return pythonNumber(value.toDouble());
    if (value.isString()) return pythonJsonString(value.toString());
    fail(QStringLiteral("JSON 顶层必须为对象或数组"));
}

QString htmlEscape(const QString& text)
{
    QString result = text;
    result.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    result.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    result.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    result.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    result.replace(QLatin1Char('\''), QStringLiteral("&#x27;"));
    return result;
}

QString pythonJsonString(const QString& text)
{
    QString out = QStringLiteral("\"");
    for (int i = 0; i < text.size(); ++i) {
        uint code = text.at(i).unicode();
        if (QChar(text.at(i)).isHighSurrogate() && i + 1 < text.size()
            && QChar(text.at(i + 1)).isLowSurrogate()) {
            const uint low = text.at(i + 1).unicode();
            code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
            ++i;
        }
        if (code == '"') { out += QStringLiteral("\\\""); continue; }
        if (code == '\\') { out += QStringLiteral("\\\\"); continue; }
        if (code == '\n') { out += QStringLiteral("\\n"); continue; }
        if (code == '\r') { out += QStringLiteral("\\r"); continue; }
        if (code == '\t') { out += QStringLiteral("\\t"); continue; }
        if (code == '\b') { out += QStringLiteral("\\b"); continue; }
        if (code == '\f') { out += QStringLiteral("\\f"); continue; }
        if (code >= 0x20 && code <= 0x7e) {
            out += QChar(static_cast<ushort>(code));
        } else if (code <= 0xffff) {
            out += QStringLiteral("\\u%1").arg(static_cast<qulonglong>(code), 4, 16, QLatin1Char('0'));
        } else {
            const uint v = code - 0x10000u;
            const uint hi = 0xD800u + (v >> 10);
            const uint lo = 0xDC00u + (v & 0x3ffu);
            out += QStringLiteral("\\u%1\\u%2")
                       .arg(static_cast<qulonglong>(hi), 4, 16, QLatin1Char('0'))
                       .arg(static_cast<qulonglong>(lo), 4, 16, QLatin1Char('0'));
        }
    }
    return out + QStringLiteral("\"");
}

QString pythonNumber(double value)
{
    if (!std::isfinite(value)) fail(QStringLiteral("JSON 数值超出有限范围"));
    if (std::floor(value) == value && std::abs(value) < 1e15) {
        return QString::number(static_cast<qint64>(value));
    }
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general);
    if (result.ec == std::errc()) {
        return QString::fromLatin1(buffer, static_cast<int>(result.ptr - buffer));
    }
    return QString::number(value, 'g', 17);
}

QString pythonDumpValue(const QJsonValue& value)
{
    if (value.isNull()) return QStringLiteral("null");
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) return pythonNumber(value.toDouble());
    if (value.isString()) return pythonJsonString(value.toString());
    if (value.isArray()) {
        QString out = QStringLiteral("[");
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i) out += QStringLiteral(", ");
            out += pythonDumpValue(array.at(i));
        }
        return out + QStringLiteral("]");
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        QString out = QStringLiteral("{");
        for (qsizetype i = 0; i < keys.size(); ++i) {
            if (i) out += QStringLiteral(", ");
            out += pythonJsonString(keys.at(i)) + QStringLiteral(": ")
                + pythonDumpValue(object.value(keys.at(i)));
        }
        return out + QStringLiteral("}");
    }
    return QStringLiteral("null");
}

QString configFingerprint(const QJsonObject& config)
{
    QJsonObject geometry;
    for (const QString& key : {QStringLiteral("target"), QStringLiteral("orientation"),
                               QStringLiteral("optics")}) {
        geometry.insert(key, config.value(key));
    }
    return sha256HexUtf8(pythonDumpValue(geometry));
}

double percentile(std::vector<double> values, double p)
{
    require(!values.empty(), QStringLiteral("缺少残差样本"));
    std::sort(values.begin(), values.end());
    const double position = p / 100.0 * static_cast<double>(values.size() - 1);
    const double low = std::floor(position);
    const double high = std::ceil(position);
    const auto a = static_cast<qsizetype>(low);
    const auto b = static_cast<qsizetype>(high);
    if (a == b) return values.at(a);
    return values.at(a) + (position - low) * (values.at(b) - values.at(a));
}

QJsonObject errorStats(const std::vector<double>& errors, const QString& unit)
{
    require(!errors.empty(), QStringLiteral("缺少残差样本"));
    double sum = 0.0, sumSquares = 0.0, maximum = 0.0;
    for (double value : errors) {
        require(std::isfinite(value), QStringLiteral("残差 的形状或数值无效"));
        sum += value;
        sumSquares += value * value;
        maximum = std::max(maximum, value);
    }
    const double count = static_cast<double>(errors.size());
    QJsonObject result;
    result.insert(QStringLiteral("count"), static_cast<qint64>(errors.size()));
    result.insert(QStringLiteral("rms_") + unit, std::sqrt(sumSquares / count));
    result.insert(QStringLiteral("mean_") + unit, sum / count);
    result.insert(QStringLiteral("p95_") + unit, percentile(errors, 95.0));
    result.insert(QStringLiteral("max_") + unit, maximum);
    return result;
}

double wrapDegrees(double degrees)
{
    double value = std::fmod(degrees + 180.0, 360.0);
    if (value < 0.0) value += 360.0;
    return value - 180.0;
}

std::vector<double> wrapDegrees(const std::vector<double>& degrees)
{
    std::vector<double> result;
    result.reserve(degrees.size());
    for (double value : degrees) result.push_back(wrapDegrees(value));
    return result;
}

int requireInt(const QJsonValue& value, const QString& name)
{
    if (value.isBool() || !value.isDouble()) fail(name + QStringLiteral(" 必须为整数"));
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || std::abs(number) > 2147483647.0) {
        fail(name + QStringLiteral(" 必须为有限整数"));
    }
    return static_cast<int>(number);
}

bool requireBool(const QJsonValue& value, const QString& name)
{
    if (!value.isBool()) fail(name + QStringLiteral(" 必须为布尔值"));
    return value.toBool();
}

QString requireString(const QJsonValue& value, const QString& name)
{
    if (!value.isString()) fail(name + QStringLiteral(" 必须为文本"));
    return value.toString();
}

double requireNumber(const QJsonValue& value, const QString& name)
{
    if (value.isBool() || !value.isDouble()) fail(name + QStringLiteral(" 必须为数值"));
    const double number = value.toDouble();
    if (!std::isfinite(number)) fail(name + QStringLiteral(" 必须为有限数值"));
    return number;
}

double requirePositiveNumber(const QJsonValue& value, const QString& name)
{
    const double number = requireNumber(value, name);
    if (number <= 0.0) fail(name + QStringLiteral(" 必须为有限正数"));
    return number;
}

QJsonArray requireArray(const QJsonValue& value, const QString& name)
{
    if (!value.isArray()) fail(name + QStringLiteral(" 必须为数组"));
    return value.toArray();
}

QJsonObject requireObject(const QJsonValue& value, const QString& name)
{
    if (!value.isObject()) fail(name + QStringLiteral(" 必须为对象"));
    return value.toObject();
}

std::vector<double> numberVector(const QJsonValue& value, const QString& name, int size)
{
    const QJsonArray array = requireArray(value, name);
    if (size > 0 && array.size() != size) {
        fail(QStringLiteral("%1 的形状或数值无效").arg(name));
    }
    std::vector<double> result;
    result.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& item : array) result.push_back(requireNumber(item, name));
    return result;
}

std::vector<std::vector<double>> numberMatrix(const QJsonValue& value, const QString& name, int columns)
{
    const QJsonArray array = requireArray(value, name);
    std::vector<std::vector<double>> result;
    result.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& item : array) {
        result.push_back(numberVector(item, name, columns));
    }
    return result;
}

} // namespace spike

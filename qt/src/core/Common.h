#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <optional>
#include <stdexcept>
#include <vector>

namespace spike {

constexpr const char* kAppId = "spike-calibration-tool";
#ifdef SPIKE_VERSION
constexpr const char* kVersion = SPIKE_VERSION;
#else
constexpr const char* kVersion = "1.0.0";
#endif

// Every validation failure from the Python version was a ValueError. Keeping one
// exception type makes the C++ port keep the same "reject, explain, do not guess"
// behaviour at every layer.
class Error : public std::runtime_error {
public:
    explicit Error(const QString& text)
        : std::runtime_error(text.toStdString()), m_text(text) {}
    const QString& text() const { return m_text; }

private:
    QString m_text;
};

[[noreturn]] void fail(const QString& message);
void require(bool condition, const QString& message);

QString nowIso();
QString makeId();
QByteArray sha256Raw(const QByteArray& data);
QString sha256Hex(const QByteArray& data);
QString sha256HexUtf8(const QString& text);
QByteArray encodeBase64(const QByteArray& data);
QByteArray decodeBase64Strict(const QString& text);

// json.dumps(..., allow_nan=False) / json.loads(parse_constant=..., parse_float=...)
// equivalents. Qt's JSON parser already rejects NaN/Infinity literals; finiteJson
// guards values produced in memory as well.
QJsonValue strictLoads(const QByteArray& utf8);
QString dumpsJson(const QJsonValue& value, bool indented = true);
bool finiteJson(const QJsonValue& value);

QString htmlEscape(const QString& text);
QString pythonJsonString(const QString& text);
QString pythonNumber(double value);
QString pythonDumpValue(const QJsonValue& value);
QString configFingerprint(const QJsonObject& config);

double percentile(std::vector<double> values, double p);
QJsonObject errorStats(const std::vector<double>& errors, const QString& unit);
double wrapDegrees(double degrees);
std::vector<double> wrapDegrees(const std::vector<double>& degrees);

int requireInt(const QJsonValue& value, const QString& name);
bool requireBool(const QJsonValue& value, const QString& name);
QString requireString(const QJsonValue& value, const QString& name);
double requireNumber(const QJsonValue& value, const QString& name);
double requirePositiveNumber(const QJsonValue& value, const QString& name);
QJsonArray requireArray(const QJsonValue& value, const QString& name);
QJsonObject requireObject(const QJsonValue& value, const QString& name);
std::vector<double> numberVector(const QJsonValue& value, const QString& name, int size = 0);
std::vector<std::vector<double>> numberMatrix(const QJsonValue& value, const QString& name, int columns = 0);

} // namespace spike

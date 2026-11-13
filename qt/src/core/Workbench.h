#pragma once

#include "Model.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <mutex>
#include <atomic>

namespace spike {

// service.Workbench equivalent: on-disk projects, invalidation rules and exports.
// The HTTP layer is gone; the GUI talks to this class directly.
class Workbench {
public:
    explicit Workbench(const QString& root);

    QString root() const { return m_root; }
    QJsonObject state() const;

    void setHardwareConnected(bool connected);
    bool hardwareConnected() const;

    QJsonObject createProject(const QString& name);
    QJsonObject getProject(const QString& id) const;
    QJsonObject updateConfig(const QString& id, const QJsonObject& body);
    QJsonObject loadDemo(const QString& id);
    QJsonObject importFiles(const QString& id, const QJsonArray& files,
                            const QString& split, const QString& sequenceId,
                            const QString& evidence, const QJsonObject& options);
    QJsonObject importObservations(const QString& id, const QJsonArray& captures);
    QJsonObject updateCapture(const QString& id, const QString& captureId,
                              const QJsonObject& body);
    QJsonObject solve(const QString& id);
    QJsonObject validate(const QString& id);
    QJsonObject calibrateTurntable(const QString& id, const QJsonArray& observations,
                                   const QJsonObject& reference);
    QJsonObject query(const QString& id, const QJsonArray& points,
                      const QJsonValue& encoderDeg = QJsonValue()) const;
    QByteArray preview(const QString& id, const QString& captureId, bool undistort) const;
    QJsonObject exportProject(const QString& id) const;
    QString reportHtml(const QString& id) const;

private:
    void checkId(const QString& id) const;
    QJsonObject projectCopy(const QString& id) const;
    QJsonObject save(QJsonObject project);
    static void invalidate(QJsonObject& project);
    static void checkCaptures(QJsonArray& captures, const QJsonArray& existing);

    QString m_root;
    QMap<QString, QJsonObject> m_projects;
    QStringList m_loadErrors;
    std::atomic_bool m_hardwareConnected{false};
    mutable std::recursive_mutex m_mutex;
};

} // namespace spike

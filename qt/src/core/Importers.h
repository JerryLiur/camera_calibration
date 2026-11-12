#pragma once

#include "Model.h"

#include <QByteArray>
#include <QJsonObject>

#include <opencv2/core.hpp>

namespace spike {

struct FileRecord {
    QString name;
    QByteArray content;

    static FileRecord fromJson(const QJsonObject& object);
};

// importers.import_capture() equivalent. Never touches arbitrary filesystem paths
// and never guesses a DAT layout.
Capture importCapture(const FileRecord& file, const Config& config, const QString& split,
                       const QString& sequenceId, const QString& evidence,
                       const QJsonObject& options = QJsonObject());

cv::Mat previewGray(const cv::Mat& image, const QJsonObject& options);

// Decode one Spike NPY / packed DAT frame into a displayable 8-bit gray image.
// Used by the file-backed camera device to replay sensor frames offline.
cv::Mat reconstructFrame(const QByteArray& bytes, const QString& suffix,
                         const QJsonObject& options);

QByteArray encodePng(const cv::Mat& image);
QString encodePreview(const cv::Mat& gray8);
cv::Mat decodeImageBytes(const QByteArray& bytes, int flags);

} // namespace spike

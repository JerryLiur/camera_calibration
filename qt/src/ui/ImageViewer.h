#pragma once

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QVector>
#include <QWidget>

namespace spike {
namespace ui {

// Coordinate-aware image viewport: corner overlay, pointer readout and click-to-query.
class ImageViewer : public QWidget {
    Q_OBJECT
public:
    explicit ImageViewer(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setPoints(const QVector<QPointF>& points, const QSize& imageSize);
    void setEmptyText(const QString& title, const QString& hint);
    void clear();

signals:
    void pixelHovered(const QPointF& pixel);
    void pixelClicked(const QPointF& pixel);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QRectF targetRect() const;
    QPointF mapToPixel(const QPoint& position) const;

    QImage m_image;
    QVector<QPointF> m_points;
    QSize m_imageSize;
    QString m_emptyTitle = QStringLiteral("等待标定图像");
    QString m_emptyHint = QStringLiteral("导入标定板数据，逐张检查角点与视场覆盖");
};

} // namespace ui
} // namespace spike

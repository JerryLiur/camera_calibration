#include "ImageViewer.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>

namespace spike {
namespace ui {

ImageViewer::ImageViewer(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(360);
    setMouseTracking(true);
    setStyleSheet(QStringLiteral("background:#1c2431;"));
}

void ImageViewer::setImage(const QImage& image)
{
    m_image = image;
    if (m_imageSize.isEmpty()) m_imageSize = image.size();
    update();
}

void ImageViewer::setPoints(const QVector<QPointF>& points, const QSize& imageSize)
{
    m_points = points;
    m_imageSize = imageSize;
    update();
}

void ImageViewer::clear()
{
    m_image = QImage();
    m_points.clear();
    m_imageSize = QSize();
    update();
}

QRectF ImageViewer::targetRect() const
{
    if (m_image.isNull() || m_imageSize.isEmpty()) return QRectF();
    const QSizeF available = QSizeF(size()) - QSizeF(24, 24);
    const double scale = std::min(available.width() / std::max(1, m_imageSize.width()),
                                  available.height() / std::max(1, m_imageSize.height()));
    const QSizeF drawn(m_imageSize.width() * scale, m_imageSize.height() * scale);
    return QRectF((width() - drawn.width()) / 2.0, (height() - drawn.height()) / 2.0,
                  drawn.width(), drawn.height());
}

QPointF ImageViewer::mapToPixel(const QPoint& position) const
{
    const QRectF rect = targetRect();
    if (!rect.contains(position)) return QPointF(-1.0, -1.0);
    return QPointF((position.x() - rect.left()) / rect.width() * m_imageSize.width(),
                   (position.y() - rect.top()) / rect.height() * m_imageSize.height());
}

void ImageViewer::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#1c2431")));
    if (m_image.isNull()) {
        painter.setPen(QColor(QStringLiteral("#9fb0c6")));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("导入不同姿态的标定板数据，逐张检查角点"));
        return;
    }
    const QRectF target = targetRect();
    painter.drawImage(target, m_image);
    if (m_imageSize.isEmpty()) return;
    const double scaleX = target.width() / m_imageSize.width();
    const double scaleY = target.height() / m_imageSize.height();
    const double radius = std::max(2.0, target.width() / 250.0);
    for (int i = 0; i < m_points.size(); ++i) {
        const QPointF point = m_points.at(i);
        const QPointF centre(target.left() + point.x() * scaleX,
                             target.top() + point.y() * scaleY);
        painter.setPen(QPen(i == 0 ? QColor(QStringLiteral("#ffcb75"))
                                   : QColor(QStringLiteral("#98c6ff")), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(centre, radius, radius);
    }
    painter.setPen(QColor(QStringLiteral("#5b6b80")));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
}

void ImageViewer::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pixel = mapToPixel(event->pos());
    if (pixel.x() >= 0.0) emit pixelHovered(pixel);
    QWidget::mouseMoveEvent(event);
}

void ImageViewer::mousePressEvent(QMouseEvent* event)
{
    const QPointF pixel = mapToPixel(event->pos());
    if (pixel.x() >= 0.0) emit pixelClicked(pixel);
    QWidget::mousePressEvent(event);
}

} // namespace ui
} // namespace spike

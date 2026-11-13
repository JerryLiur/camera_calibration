#include "ImageViewer.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>

namespace spike {
namespace ui {

ImageViewer::ImageViewer(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("图像预览"));
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

void ImageViewer::setEmptyText(const QString& title, const QString& hint)
{
    m_emptyTitle = title;
    m_emptyHint = hint;
    setAccessibleDescription(title + QStringLiteral("。") + hint);
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
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath viewport;
    viewport.addRoundedRect(QRectF(rect()), 8, 8);
    painter.setClipPath(viewport);
    painter.fillRect(rect(), QColor(QStringLiteral("#101e30")));
    if (m_image.isNull()) {
        painter.setPen(QPen(QColor(QStringLiteral("#1b2b40")), 1));
        for (int x = width() / 2 % 40; x < width(); x += 40)
            painter.drawLine(x, 0, x, height());
        for (int y = height() / 2 % 40; y < height(); y += 40)
            painter.drawLine(0, y, width(), y);
        const QPointF center(width() / 2.0, height() / 2.0 - 42);
        const QRectF focus(center.x() - 26, center.y() - 26, 52, 52);
        painter.setPen(QPen(QColor(QStringLiteral("#89a3c1")), 2, Qt::SolidLine, Qt::RoundCap));
        const qreal corner = 12;
        for (const QPointF& point : {focus.topLeft(), focus.topRight(), focus.bottomLeft(), focus.bottomRight()}) {
            const qreal dx = point.x() < center.x() ? corner : -corner;
            const qreal dy = point.y() < center.y() ? corner : -corner;
            painter.drawLine(point, point + QPointF(dx, 0));
            painter.drawLine(point, point + QPointF(0, dy));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#62bfc9")), 1.5));
        painter.drawLine(center - QPointF(9, 0), center + QPointF(9, 0));
        painter.drawLine(center - QPointF(0, 9), center + QPointF(0, 9));
        QFont titleFont = font();
        titleFont.setPixelSize(16);
        titleFont.setWeight(QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#e0e8f2")));
        painter.drawText(QRectF(20, center.y() + 46, width() - 40, 30), Qt::AlignCenter, m_emptyTitle);
        QFont hintFont = font();
        hintFont.setPixelSize(12);
        painter.setFont(hintFont);
        painter.setPen(QColor(QStringLiteral("#a8b9cc")));
        painter.drawText(QRectF(24, center.y() + 80, width() - 48, 56),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_emptyHint);
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

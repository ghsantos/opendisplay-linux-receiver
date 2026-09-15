#include "video_widget.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTouchEvent>
#include <QWheelEvent>

namespace od {

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
}

void VideoWidget::setFrame(const QImage& frame) {
    frame_ = frame;
    videoSize_ = frame.size();
    update();
}

void VideoWidget::clearFrame() {
    frame_ = QImage();
    videoSize_ = QSize();
    update();
}

void VideoWidget::setCursorState(const double x, const double y, const bool visible) {
    cursorX_ = x;
    cursorY_ = y;
    cursorVisible_ = visible;
    update();
}

void VideoWidget::setCursorSprite(const QPixmap& pixmap, const QSizeF normSize,
                                  const QPointF anchorNorm) {
    cursorSprite_ = pixmap;
    cursorNormSize_ = normSize;
    cursorAnchor_ = anchorNorm;
    update();
}

void VideoWidget::setHudText(const QString& text) {
    if (text != hudText_) {
        hudText_ = text;
        update();
    }
}

QRectF VideoWidget::videoRect() const {
    if (videoSize_.isEmpty()) {
        return {};
    }
    const QSizeF fitted = QSizeF(videoSize_).scaled(QSizeF(size()),
                                                  Qt::KeepAspectRatio);
    const QPointF topLeft((width() - fitted.width()) / 2.0,
                          (height() - fitted.height()) / 2.0);
    return QRectF(topLeft, fitted);
}

void VideoWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    videoRect_ = videoRect();
    if (!frame_.isNull() && !videoRect_.isEmpty()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(videoRect_, frame_);
    }
    if (cursorVisible_ && !cursorSprite_.isNull() && !videoRect_.isEmpty()) {
        const QSizeF spriteSize(cursorNormSize_.width() * videoRect_.width(),
                                cursorNormSize_.height() * videoRect_.height());
        const QPointF topLeft = videoRect_.topLeft()
            + QPointF(cursorX_ * videoRect_.width(), cursorY_ * videoRect_.height())
            - QPointF(cursorAnchor_.x() * spriteSize.width(),
                      cursorAnchor_.y() * spriteSize.height());
        painter.drawPixmap(QRectF(topLeft, spriteSize), cursorSprite_,
                           QRectF(QPointF(0, 0), QSizeF(cursorSprite_.size())));
    }
    if (!hudText_.isEmpty()) {
        QFontMetrics metrics(font());
        const QRect textRect = metrics.boundingRect(hudText_);
        const QRect box(8, 8, textRect.width() + 16, textRect.height() + 8);
        painter.fillRect(box, QColor(0, 0, 0, 160));
        painter.setPen(Qt::white);
        painter.drawText(box, Qt::AlignCenter, hudText_);
    }
}

QPointF VideoWidget::toVideoNorm(const QPointF& pos) const {
    if (videoRect_.isEmpty()) {
        return {};
    }
    const double x = std::clamp((pos.x() - videoRect_.x()) / videoRect_.width(),
                                0.0, 1.0);
    const double y = std::clamp((pos.y() - videoRect_.y()) / videoRect_.height(),
                                0.0, 1.0);
    return QPointF(x, y);
}

void VideoWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        const QPointF norm = toVideoNorm(event->position());
        emit touched(QStringLiteral("began"), norm.x(), norm.y());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        const QPointF norm = toVideoNorm(event->position());
        emit touched(QStringLiteral("moved"), norm.x(), norm.y());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        const QPointF norm = toVideoNorm(event->position());
        emit touched(QStringLiteral("ended"), norm.x(), norm.y());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoWidget::wheelEvent(QWheelEvent* event) {
    // Scroll deltas travel in *video pixels* with natural-scrolling sign
    // (protocol 7): positive dy = content moves down = wheel/trackpad up.
    QPointF delta = event->pixelDelta();
    if (delta.isNull()) {
        delta = QPointF(event->angleDelta()) / 8.0;  // degrees -> pixels
    }
    if (!videoRect_.isEmpty() && videoSize_.height() > 0) {
        const double sx = videoSize_.width() / videoRect_.width();
        const double sy = videoSize_.height() / videoRect_.height();
        emit scrolled(delta.x() * sx, delta.y() * sy);
    }
    event->accept();
}

bool VideoWidget::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin
        || event->type() == QEvent::TouchUpdate
        || event->type() == QEvent::TouchEnd
        || event->type() == QEvent::TouchCancel) {
        return handleTouchEvent(static_cast<QTouchEvent*>(event));
    }
    return QWidget::event(event);
}

bool VideoWidget::handleTouchEvent(QTouchEvent* event) {
    if (event->points().isEmpty()) {
        return QWidget::event(event);
    }
    const auto& point = event->points().first();
    const QPointF norm = toVideoNorm(point.position());
    switch (point.state()) {
    case QEventPoint::Pressed:
        emit touched(QStringLiteral("began"), norm.x(), norm.y());
        break;
    case QEventPoint::Updated:
        emit touched(QStringLiteral("moved"), norm.x(), norm.y());
        break;
    case QEventPoint::Released:
        emit touched(QStringLiteral("ended"), norm.x(), norm.y());
        break;
    default:
        break;
    }
    event->accept();
    return true;
}

void VideoWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F11) {
        emit fullscreenToggled();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace od

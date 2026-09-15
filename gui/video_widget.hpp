#pragma once

#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QSizeF>
#include <QWidget>

class QTouchEvent;

namespace od {

/// Paints the decoded stream aspect-fit with black letterbox bars, the remote
/// cursor sprite on top, and an optional perf HUD — and turns local
/// mouse/touch/wheel gestures into `touch`/`scroll` input coordinates
/// normalized against the letterboxed video rect (protocol section 7).
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);

    void setFrame(const QImage& frame);
    void clearFrame();
    void setCursorState(double x, double y, bool visible);
    void setCursorSprite(const QPixmap& pixmap, QSizeF normSize, QPointF anchorNorm);
    void setHudText(const QString& text);

    /// The rect (widget coords) the video is drawn into — for tests and for
    /// mapping input consistently.
    [[nodiscard]] QRectF videoRect() const;

signals:
    void touched(QString phase, double x, double y);
    void scrolled(double dx, double dy);
    void fullscreenToggled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// Widget point -> normalized video coordinates (clamped to 0..1).
    QPointF toVideoNorm(const QPointF& pos) const;
    bool handleTouchEvent(QTouchEvent* event);

    QImage frame_;
    QSize videoSize_;       // decoded stream size, from the last frame
    QRectF videoRect_;
    double cursorX_ = 0.5;
    double cursorY_ = 0.5;
    bool cursorVisible_ = false;
    QPixmap cursorSprite_;
    QSizeF cursorNormSize_;
    QPointF cursorAnchor_;
    QString hudText_;
    bool dragging_ = false;
};

}  // namespace od

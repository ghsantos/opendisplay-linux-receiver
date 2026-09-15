#include "receiver_window.hpp"

#include "opendisplay/receiver_session.hpp"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QScreen>
#include <QStatusBar>
#include <QWindow>

namespace od {

ReceiverWindow::ReceiverWindow(ReceiverSession* session,
                               const ReceiverOptions& options, QWidget* parent)
    : QMainWindow(parent), session_(session), options_(options) {
    setWindowTitle(QStringLiteral("OpenDisplay Receiver"));
    video_ = new VideoWidget(this);
    setCentralWidget(video_);
    status_ = new QLabel(this);
    statusBar()->addWidget(status_);
    resize(1280, 800);

    connect(session_, &ReceiverSession::statusChanged, status_, &QLabel::setText);
    connect(session_, &ReceiverSession::advertisedChanged, this,
            [this](bool advertised, const QString& name) {
                if (advertised) {
                    status_->setText(status_->text() + QStringLiteral("  •  %1").arg(name));
                }
            });
    connect(session_, &ReceiverSession::connectedChanged, this,
            [this](bool connected) {
                video_->setCursor(connected ? Qt::BlankCursor : Qt::ArrowCursor);
                if (!connected) {
                    video_->clearFrame();
                }
            });
    connect(session_, &ReceiverSession::frameReady, video_, [this](const QImage& image, qint64) {
        video_->setFrame(image);
    });
    connect(session_, &ReceiverSession::cursorChanged, this,
            [this](double x, double y, bool visible) {
                video_->setCursorState(x, y, visible);
            });
    connect(session_, &ReceiverSession::cursorImageReady, this,
            [this](const QByteArray& png, double nw, double nh, double ax, double ay) {
                QPixmap pixmap;
                if (pixmap.loadFromData(png, "PNG")) {
                    video_->setCursorSprite(pixmap, QSizeF(nw, nh), QPointF(ax, ay));
                }
            });
    if (options_.showHud) {
        connect(session_, &ReceiverSession::hudChanged, video_,
                &VideoWidget::setHudText);
    }
    connect(session_, &ReceiverSession::peerMessage, this,
            [this](const QString& title, const QString& message, const QString& url) {
                QString text = message;
                if (!url.isEmpty()) {
                    text += QStringLiteral("\n\n<a href=\"%1\">%1</a>").arg(url);
                }
                QMessageBox::warning(this, title, text);
            });
    connect(session_, &ReceiverSession::connectionUnstable, this, [this] {
        status_->setText(QStringLiteral("Connection unstable — resyncing"));
    });

    if (options_.input) {
        connect(video_, &VideoWidget::touched, session_,
                &ReceiverSession::sendTouch);
        connect(video_, &VideoWidget::scrolled, session_,
                &ReceiverSession::sendScroll);
    }
    connect(video_, &VideoWidget::fullscreenToggled, this,
            &ReceiverWindow::toggleFullscreen);

    // A window moving between screens can change the physical panel size;
    // announce the new one (the sender rebuilds its virtual display). The
    // window handle only exists after show(), so hook it lazily.
    if (windowHandle() != nullptr) {
        hookScreenChange();
    }

    if (options_.fullscreen) {
        showFullScreen();
    }
}

Size ReceiverWindow::announcedPanel() const {
    if (options_.panel.has_value()) {
        return *options_.panel;
    }
    const QScreen* screen = windowHandle() != nullptr ? windowHandle()->screen()
                                                      : nullptr;
    if (screen == nullptr) {
        return {.width = 1920, .height = 1080};
    }
    const QSize logical = screen->geometry().size();
    const double dpr = screen->devicePixelRatio();
    return {.width = static_cast<int>(logical.width() * dpr),
            .height = static_cast<int>(logical.height() * dpr)};
}

double ReceiverWindow::announcedScale() const {
    if (options_.scale.has_value()) {
        return *options_.scale;
    }
    const QScreen* screen = windowHandle() != nullptr ? windowHandle()->screen()
                                                      : nullptr;
    return screen != nullptr ? screen->devicePixelRatio() : 1.0;
}

void ReceiverWindow::hookScreenChange() {
    connect(windowHandle(), &QWindow::screenChanged, this,
            [this](QScreen*) { announcePanel(); });
}

bool ReceiverWindow::event(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange
        || event->type() == QEvent::Show) {
        announcePanel();
    }
    if (event->type() == QEvent::Show && windowHandle() != nullptr) {
        hookScreenChange();
    }
    return QMainWindow::event(event);
}

void ReceiverWindow::announcePanel() {
    session_->setPanel(announcedPanel(), announcedScale());
}

void ReceiverWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        showNormal();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void ReceiverWindow::toggleFullscreen() {
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void ReceiverWindow::closeEvent(QCloseEvent* event) {
    session_->shutdown();
    QMainWindow::closeEvent(event);
}

}  // namespace od

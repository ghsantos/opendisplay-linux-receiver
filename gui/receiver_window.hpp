#pragma once

#include "opendisplay/types.hpp"
#include "video_widget.hpp"

#include <QLabel>
#include <QMainWindow>
#include <QPixmap>

namespace od {

class ReceiverSession;

/// Top-level window: owns the VideoWidget, mirrors session status in the
/// status bar, hides the local pointer while a sender drives the cursor,
/// and announces the target screen's physical size as the panel.
class ReceiverWindow : public QMainWindow {
    Q_OBJECT
public:
    ReceiverWindow(ReceiverSession* session, const ReceiverOptions& options,
                   QWidget* parent = nullptr);

    /// Physical pixel size of the screen this window should report as the
    /// panel — `options.panel` wins when set.
    [[nodiscard]] Size announcedPanel() const;
    [[nodiscard]] double announcedScale() const;

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void announcePanel();
    void toggleFullscreen();
    void hookScreenChange();   // once windowHandle() exists

    ReceiverSession* session_;
    const ReceiverOptions& options_;
    VideoWidget* video_;
    QLabel* status_;
};

}  // namespace od

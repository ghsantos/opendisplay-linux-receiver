#include "opendisplay/log.hpp"
#include "opendisplay/receiver_session.hpp"
#include "receiver_window.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QRegularExpression>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::atomic_bool interrupted = false;

void handleSignal(int) { interrupted.store(true); }

od::Size parseSize(const QString& value, const std::string& option) {
    static const QRegularExpression pattern(QStringLiteral(R"(^(\d+)[xX](\d+)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error(option + " must use WIDTHxHEIGHT");
    }
    const od::Size result{.width = match.captured(1).toInt(),
                          .height = match.captured(2).toInt()};
    if (result.width < 2 || result.height < 2 || result.width > 65'535
        || result.height > 65'535) {
        throw std::runtime_error(option + " dimensions must be between 2 and 65535");
    }
    return result;
}

od::DecoderKind parseDecoder(const QString& value) {
    if (value == QStringLiteral("auto")) return od::DecoderKind::Auto;
    if (value == QStringLiteral("software")) return od::DecoderKind::Software;
    if (value == QStringLiteral("vaapi")) return od::DecoderKind::Vaapi;
    throw std::runtime_error("--decoder must be auto, software, or vaapi");
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication::setApplicationName(QStringLiteral("opendisplay-receiver"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("opendisplay"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Use this Linux machine as a display for an OpenDisplay sender."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption portOption({"p", "port"}, "TCP port to listen on.",
                                        "port", "9000");
    const QCommandLineOption nameOption("name",
        "Bonjour service name (default: host name).", "name");
    const QCommandLineOption panelOption("panel",
        "Override the announced panel pixels.", "WIDTHxHEIGHT");
    const QCommandLineOption scaleOption("scale",
        "Override the announced UI scale factor.", "factor");
    const QCommandLineOption fullscreenOption({"f", "fullscreen"},
        "Start fullscreen (F11 toggles, Esc exits).");
    const QCommandLineOption noInputOption("no-input",
        "Do not forward mouse/touch/scroll input.");
    const QCommandLineOption noCursorChannelOption("no-cursor-channel",
        "Disable the UDP cursor side channel (TCP cursor still works).");
    const QCommandLineOption decoderOption({"d", "decoder"},
        "H.264 decoder: auto, software, or vaapi.", "decoder", "auto");
    const QCommandLineOption vaapiOption("vaapi-device", "VA-API render node.",
                                         "path", "/dev/dri/renderD128");
    const QCommandLineOption maxEncodeOption("max-encode",
        "Advertise a decode ceiling smaller than the panel.", "WIDTHxHEIGHT");
    const QCommandLineOption noHudOption("no-hud", "Hide the perf overlay.");
    const QCommandLineOption verboseOption("verbose", "Enable diagnostic logging.");
    parser.addOptions({portOption, nameOption, panelOption, scaleOption,
                       fullscreenOption, noInputOption, noCursorChannelOption,
                       decoderOption, vaapiOption, maxEncodeOption, noHudOption,
                       verboseOption});
    parser.process(application);

    try {
        od::ReceiverOptions options;
        const QString portText = parser.value(portOption);
        bool valid = false;
        const int port = portText.toInt(&valid);
        if (!valid || port < 1 || port > 65'535) {
            throw std::runtime_error("--port must be between 1 and 65535");
        }
        options.port = static_cast<std::uint16_t>(port);
        options.serviceName = parser.value(nameOption).toStdString();
        if (parser.isSet(panelOption)) {
            options.panel = parseSize(parser.value(panelOption), "--panel");
        }
        if (parser.isSet(scaleOption)) {
            const double scale = parser.value(scaleOption).toDouble(&valid);
            if (!valid || scale < 0.5 || scale > 4.0) {
                throw std::runtime_error("--scale must be between 0.5 and 4.0");
            }
            options.scale = scale;
        }
        options.fullscreen = parser.isSet(fullscreenOption);
        options.input = !parser.isSet(noInputOption);
        options.cursorChannel = !parser.isSet(noCursorChannelOption);
        options.decoder = parseDecoder(parser.value(decoderOption));
        options.vaapiDevice = parser.value(vaapiOption).toStdString();
        if (parser.isSet(maxEncodeOption)) {
            options.maxEncode = parseSize(parser.value(maxEncodeOption),
                                          "--max-encode");
        }
        options.showHud = !parser.isSet(noHudOption);
        options.verbose = parser.isSet(verboseOption);
        od::verboseLogging = options.verbose;

        od::ReceiverSession session(options);
        od::ReceiverWindow window(&session, options);
        window.show();
        session.setPanel(window.announcedPanel(), window.announcedScale());
        session.start();

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        QTimer interruptTimer;
        QObject::connect(&interruptTimer, &QTimer::timeout, &application, [&] {
            if (interrupted.load()) {
                application.quit();
            }
        });
        interruptTimer.start(100);

        const int result = application.exec();
        session.shutdown();   // announce `closing` so the sender ends cleanly
        return result;
    } catch (const std::exception& error) {
        std::cerr << "opendisplay-receiver: " << error.what() << '\n';
        return 1;
    }
}

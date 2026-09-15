#include "opendisplay/advertise.hpp"

namespace od {

/// Fallback used when libavahi-client is unavailable at build time: the
/// receiver still listens on TCP (a sender can dial by address), only the
/// Bonjour advertisement is missing. start() reports the missing backend so
/// the status line is honest about it.
AvahiAdvertiser::AvahiAdvertiser(QObject* parent) : QObject(parent) {}

AvahiAdvertiser::~AvahiAdvertiser() { stop(); }

void AvahiAdvertiser::start(QString serviceName, quint16, QString) {
    {
        std::lock_guard lock(mutex_);
        requestedName_ = std::move(serviceName);
    }
    emit errorOccurred(QStringLiteral(
        "built without Avahi — not advertised; senders must dial by address"));
}

void AvahiAdvertiser::setServiceName(const QString& name) {
    std::lock_guard lock(mutex_);
    requestedName_ = name;
}

void AvahiAdvertiser::stop() {}

QString AvahiAdvertiser::effectiveName() const {
    std::lock_guard lock(mutex_);
    return requestedName_;
}

}  // namespace od

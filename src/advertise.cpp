#include "opendisplay/advertise.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/wire.hpp"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>

namespace od {

AvahiAdvertiser::AvahiAdvertiser(QObject* parent) : QObject(parent) {}

AvahiAdvertiser::~AvahiAdvertiser() { stop(); }

void AvahiAdvertiser::start(QString serviceName, const quint16 port, QString installId) {
    stop();
    {
        std::lock_guard lock(mutex_);
        requestedName_ = std::move(serviceName);
        installId_ = std::move(installId);
    }
    port_ = port;
    stopRequested_ = false;
    dirty_ = true;
    thread_ = std::thread(&AvahiAdvertiser::run, this);
}

void AvahiAdvertiser::setServiceName(const QString& name) {
    {
        std::lock_guard lock(mutex_);
        if (name == requestedName_) {
            return;
        }
        requestedName_ = name;
    }
    dirty_ = true;
    if (poll_ != nullptr) {
        avahi_simple_poll_wakeup(poll_);
    }
}

void AvahiAdvertiser::stop() {
    stopRequested_ = true;
    if (poll_ != nullptr) {
        avahi_simple_poll_wakeup(poll_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

QString AvahiAdvertiser::effectiveName() const {
    std::lock_guard lock(mutex_);
    return publishedName_.isEmpty() ? requestedName_ : publishedName_;
}

void AvahiAdvertiser::run() {
    poll_ = avahi_simple_poll_new();
    if (poll_ == nullptr) {
        emit errorOccurred(QStringLiteral("cannot create Avahi poll"));
        return;
    }
    int error = 0;
    client_ = avahi_client_new(avahi_simple_poll_get(poll_), AVAHI_CLIENT_NO_FAIL,
                               reinterpret_cast<AvahiClientCallback>(clientCallback),
                               this, &error);
    if (client_ == nullptr) {
        emit errorOccurred(QStringLiteral("cannot connect to Avahi: %1")
                               .arg(QString::fromUtf8(avahi_strerror(error))));
        avahi_simple_poll_free(poll_);
        poll_ = nullptr;
        return;
    }
    for (;;) {
        applyPending();
        if (avahi_simple_poll_iterate(poll_, -1) != 0) {
            break;
        }
    }
    unpublish();
    avahi_client_free(client_);
    client_ = nullptr;
    avahi_simple_poll_free(poll_);
    poll_ = nullptr;
}

void AvahiAdvertiser::applyPending() {
    if (stopRequested_) {
        avahi_simple_poll_quit(poll_);
        return;
    }
    if (dirty_.exchange(false) && client_ != nullptr
        && avahi_client_get_state(client_) == AVAHI_CLIENT_S_RUNNING) {
        publish();
    }
}

void AvahiAdvertiser::publish() {
    QString name;
    QString installId;
    {
        std::lock_guard lock(mutex_);
        name = requestedName_;
        installId = installId_;
    }
    if (name.isEmpty() || client_ == nullptr) {
        return;
    }
    unpublish();
    group_ = avahi_entry_group_new(
        client_, reinterpret_cast<AvahiEntryGroupCallback>(entryGroupCallback), this);
    if (group_ == nullptr) {
        emit errorOccurred(QStringLiteral("Avahi entry group failed: %1")
                               .arg(QString::fromUtf8(
                                   avahi_strerror(avahi_client_errno(client_)))));
        return;
    }
    const auto nameUtf8 = name.toUtf8();
    const auto idTxt = QByteArray("id=") + installId.toUtf8();
    const auto pvTxt = QByteArray("pv=") + QByteArray::number(wire::protocolVersion);
    const int result = avahi_entry_group_add_service(
        group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, AvahiPublishFlags(0),
        nameUtf8.constData(), "_opensidecar._tcp", nullptr, nullptr, port_,
        idTxt.constData(), pvTxt.constData(), nullptr);
    if (result < 0) {
        emit errorOccurred(QStringLiteral("Avahi service registration failed: %1")
                               .arg(QString::fromUtf8(avahi_strerror(result))));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        publishedName_ = name;
    }
    avahi_entry_group_commit(group_);
}

void AvahiAdvertiser::unpublish() {
    if (group_ != nullptr) {
        avahi_entry_group_free(group_);
        group_ = nullptr;
    }
    {
        std::lock_guard lock(mutex_);
        publishedName_.clear();
    }
}

void AvahiAdvertiser::clientCallback(AvahiClient* client, const int state,
                                     void* userdata) {
    auto* self = static_cast<AvahiAdvertiser*>(userdata);
    switch (static_cast<AvahiClientState>(state)) {
    case AVAHI_CLIENT_S_RUNNING:
        self->publish();
        break;
    case AVAHI_CLIENT_FAILURE:
        emit self->errorOccurred(QStringLiteral("Avahi client failed: %1")
            .arg(QString::fromUtf8(avahi_strerror(avahi_client_errno(client)))));
        break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
        // Group data is stale while the client re-registers; drop it — the
        // next S_RUNNING republishes.
        self->unpublish();
        break;
    default:
        break;
    }
}

void AvahiAdvertiser::entryGroupCallback(AvahiEntryGroup* group, const int state,
                                         void* userdata) {
    auto* self = static_cast<AvahiAdvertiser*>(userdata);
    switch (static_cast<AvahiEntryGroupState>(state)) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        emit self->advertisedChanged(true, self->effectiveName());
        break;
    case AVAHI_ENTRY_GROUP_COLLISION: {
        // Another device holds our name: take Avahi's alternative and re-add.
        char* alternative = avahi_alternative_service_name(
            self->effectiveName().toUtf8().constData());
        const QString renamed = QString::fromUtf8(alternative);
        avahi_free(alternative);
        {
            std::lock_guard lock(self->mutex_);
            self->requestedName_ = renamed;
        }
        avahi_entry_group_reset(group);
        self->dirty_ = true;
        avahi_simple_poll_wakeup(self->poll_);
        debug("Bonjour name collision; retrying as " + renamed.toStdString());
        break;
    }
    case AVAHI_ENTRY_GROUP_FAILURE:
        emit self->errorOccurred(QStringLiteral("Avahi publish failed: %1")
            .arg(QString::fromUtf8(avahi_strerror(
                avahi_client_errno(avahi_entry_group_get_client(group))))));
        break;
    default:
        break;
    }
}

}  // namespace od

// SPDX-License-Identifier: Apache-2.0
#include "connection_manager.h"
#include "device_info.h"
#include "ble_limits.h"
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>

ConnectionManager::ConnectionManager(QObject *parent) : QObject(parent)
{
    m_windowsBleDeadline.setSingleShot(true);
    connect(&m_windowsBleDeadline, &QTimer::timeout, this, [this] { m_windowsBleService.kill(); });
    connect(&m_windowsBleService, &QProcess::readyReadStandardError, this, [this] {
        emit log({}, QString::fromUtf8(m_windowsBleService.readAllStandardError()));
    });
    connect(&m_windowsBleService, &QProcess::readyReadStandardOutput, this, [this] {
        m_windowsBleEvents += m_windowsBleService.readAllStandardOutput();
        if (m_windowsBleEvents.size() > 65536) { m_windowsBleService.kill(); return; }
        int end;
        while ((end = m_windowsBleEvents.indexOf('\n')) >= 0) {
            auto object = QJsonDocument::fromJson(m_windowsBleEvents.left(end)).object();
            m_windowsBleEvents.remove(0, end + 1);
            if (object.value("event") == "ble-multi-ready" && !m_windowsBleStopping) {
                m_windowsBleDeadline.stop();
                m_windowsBleReady = true;
                launchPending();
                emit changed({});
            }
        }
        finishWindowsBleStop();
    });
    const auto ended = [this] {
        m_windowsBleDeadline.stop();
        const bool expected = m_windowsBleStopping;
        m_windowsBleReady = m_windowsBleStopping = false;
        m_windowsBleToken.clear();
        m_windowsBleProgram.clear();
        if (!expected) {
            for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
                if (it->options.windowsBleToken.isEmpty()) continue;
                it->errorId = "otg.ble.service_failed";
                it->errorDetail = m_windowsBleService.error() == QProcess::FailedToStart ?
                    m_windowsBleService.errorString() : QString::number(m_windowsBleService.exitCode());
                it->pending = false;
                it->waitingForBleAttach = false;
                it->session->stopBleProvider();
                it->session->stop();
            }
        }
        if (!busy()) m_server.stop();
        emit changed({});
    };
    connect(&m_windowsBleService, &QProcess::finished, this, ended);
    connect(&m_windowsBleService, &QProcess::errorOccurred, this, [ended](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) ended();
    });
    connect(&m_server, &DeskflowServer::ready, this, [this] {
        launchPending();
        configureWaitingBle();
    });
    connect(&m_server, &DeskflowServer::changed, this, [this] { emit changed({}); });
    connect(&m_server, &DeskflowServer::log, this, [this](const QString &message) { emit log({}, message); });
    connect(&m_server, &DeskflowServer::failed, this, [this](const QString &id, const QString &detail) {
        auto message = qtTrId(id.toLatin1().constData());
        if (message.contains("%1")) message = message.arg(detail);
        emit log({}, message);
        for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
            if (state(it.key()) != Session::State::Idle) { it->errorId = id; it->errorDetail = detail; }
        }
        stopAll();
    });
    connect(&m_server, &DeskflowServer::stopped, this, [this] { emit changed({}); });
}

Session *ConnectionManager::addDevice(const QString &id)
{
    if (m_devices.contains(id)) return session(id);
    auto *child = new Session(this, m_bleStartup);
    m_devices.insert(id, {child, {}, false, false, {}, {}});
    connectSession(child, id);
    return child;
}

void ConnectionManager::connectSession(Session *child, const QString &id)
{
    connect(child, &Session::changed, this, [this, id] { update(id); });
    connect(child, &Session::bleProviderChanged, this, [this, id] { update(id); });
    connect(child, &Session::bleIdentityDetected, this, [this, id](const QString &identity, const QString &name) {
        emit bleIdentityDetected(id, identity, name);
    });
    connect(child, &Session::clipboardChanged, this, [this, id] { emit changed(id); });
    connect(child, &Session::log, this, [this, id](const QString &message) { emit log(id, message); });
    connect(child, &Session::usbAttached, this, &ConnectionManager::usbAttached);
    connect(child, &Session::displaySizeChanged, this, [this, id](const QSize &size) {
        emit displaySizeChanged(id, size);
    });
}

bool ConnectionManager::bindBleDiscovery(const QString &sourceId, const QString &deviceId)
{
    if (!m_devices.contains(sourceId) || !m_devices[sourceId].waitingForBleAttach) return false;
    if (sourceId == deviceId) return true;
    if (state(deviceId) != Session::State::Idle || (session(deviceId) && session(deviceId)->hasBleProvider()))
        return false;
    addDevice(deviceId);
    // Route the running listener's signals to the identified device. Profiles and
    // their stable IDs stay in place, including whichever one the user is editing.
    auto &source = m_devices[sourceId];
    auto &target = m_devices[deviceId];
    source.session->disconnect(this);
    target.session->disconnect(this);
    std::swap(source, target);
    connectSession(source.session, sourceId);
    connectSession(target.session, deviceId);
    return true;
}

ConnectionManager::~ConnectionManager()
{
    m_windowsBleService.disconnect(this);
    // QObject deletes children after members. Disconnect first so a QProcess
    // destructor cannot call into an already destroyed device map.
    m_server.disconnect(this);
    for (auto &entry : m_devices) {
        entry.session->disconnect(this);
        delete entry.session;
    }
    m_devices.clear();
}

Session *ConnectionManager::session(const QString &id) const { return m_devices.value(id).session; }

bool ConnectionManager::canRemoveDevice(const QString &id) const
{
    const auto *child = session(id);
    return child && state(id) == Session::State::Idle && !child->hasBleProvider();
}

bool ConnectionManager::removeDevice(const QString &id)
{
    if (!canRemoveDevice(id)) return false;
    auto *child = m_devices.take(id).session;
    child->disconnect(this);
    delete child;
    return true;
}

bool ConnectionManager::hasFailure(const QString &id) const
{
    const auto entry = m_devices.value(id);
    return !entry.errorId.isEmpty() || (entry.session && entry.session->hasFailure());
}

Session::State ConnectionManager::state(const QString &id) const
{
    const auto entry = m_devices.value(id);
    return entry.pending ? Session::State::Starting : entry.session ? entry.session->state() : Session::State::Idle;
}

QString ConnectionManager::activeUhidTarget(const QString &id) const
{
    const auto it = m_devices.constFind(id);
    return it != m_devices.cend() && state(id) != Session::State::Idle && it->options.transport == "uhid" ?
        it->options.adbSerial : QString();
}

QString ConnectionManager::status(const QString &id) const
{
    const auto entry = m_devices.value(id);
    if (!entry.errorId.isEmpty()) {
        const auto message = qtTrId(entry.errorId.toLatin1().constData());
        return message.contains("%1") ? message.arg(entry.errorDetail) : message;
    }
    if (entry.pending) return qtTrId("otg.status.connecting");
    const auto text = entry.session ? entry.session->statusMessage() : QString();
    return text.isEmpty() ? qtTrId("otg.status.not_connected") : text;
}

bool ConnectionManager::busy() const
{
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it)
        if (state(it.key()) != Session::State::Idle) return true;
    return false;
}

bool ConnectionManager::hasBleProvider() const
{
    if (m_windowsBleService.state() != QProcess::NotRunning) return true;
    for (const auto &entry : m_devices) if (entry.session->hasBleProvider()) return true;
    return false;
}

int ConnectionManager::activeBleSessions() const
{
    int count = 0;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it)
        if (it->options.transport == "ble" && state(it.key()) != Session::State::Idle) ++count;
    return count;
}

int ConnectionManager::reservedBleSessions() const
{
    int count = 0;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it)
        if ((it->options.transport == "ble" && state(it.key()) != Session::State::Idle) ||
            it->session->hasBleProvider()) ++count;
    return count;
}

const char *ConnectionManager::bleProviderStatusId() const
{
    for (const auto &entry : m_devices)
        if (entry.options.transport == "ble" && entry.session->state() == Session::State::Connected)
            return "otg.ble.status_connected";
    for (const auto &entry : m_devices) {
        if (const auto id = entry.session->bleProviderStatusId()) return id;
    }
    if (m_windowsBleService.state() != QProcess::NotRunning)
        return m_windowsBleStopping ? "otg.ble.status_stopping" :
            m_windowsBleReady ? "otg.ble.status_idle" : "otg.ble.status_starting";
    return nullptr;
}

bool ConnectionManager::settled() const
{
    return !busy() && !hasBleProvider() && m_server.state() == DeskflowServer::State::Idle;
}

bool ConnectionManager::reject(const QString &id, const char *message)
{
    m_devices[id].errorId = QString::fromLatin1(message);
    m_devices[id].errorDetail.clear();
    emit log(id, status(id));
    emit changed(id);
    return false;
}

bool ConnectionManager::start(const QString &id, const ConnectionOptions &options)
{
    if (m_shutdown || !m_devices.contains(id) || state(id) != Session::State::Idle) return false;
    if (!validScreenName(options.phone))
        return reject(id, "otg.error.phone_screen_name");
    if (options.manageServer && options.phone.compare(options.computer, Qt::CaseInsensitive) == 0)
        return reject(id, "otg.error.screen_names");
#if defined(OTG_GUI_HAVE_BLE) || defined(OTG_GUI_HAVE_BLE_WIN)
    if (options.transport == "ble" && activeBleSessions() >= OTG_BLE_MAX_PHONES)
        return reject(id, "otg.ble.multi_full");
#endif
#ifdef OTG_GUI_HAVE_BLE_WIN
    if (options.transport == "ble" && !session(id)->hasBleProvider() && reservedBleSessions() >= OTG_BLE_MAX_PHONES)
        return reject(id, "otg.ble.multi_full");
    if (options.transport == "ble" && !options.preferDirectedBle && !m_windowsBleToken.isEmpty())
        return reject(id, "otg.multi.ble_limit");
#endif
    bool requireDirected = false;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        if (it.key() == id) continue;
        const bool reserved = state(it.key()) != Session::State::Idle;
        if (reserved && (options.phone.compare(it->options.phone, Qt::CaseInsensitive) == 0 ||
            (options.transport == "usb" && it->options.transport == "usb" && options.serial == it->options.serial) ||
            (options.transport == "uhid" && it->options.transport == "uhid" &&
             options.adbSerial.trimmed() == it->options.adbSerial.trimmed())))
            return reject(id, "otg.multi.duplicate_device");
        if (options.transport == "ble" && ((reserved && it->options.transport == "ble") ||
            it->session->hasBleProvider())) {
#ifdef Q_OS_LINUX
            if (options.adapter != it->options.adapter || !options.preferDirectedBle ||
                !it->options.preferDirectedBle)
                return reject(id, "otg.multi.directed_required");
            if (!options.blePeer.isEmpty() && !options.blePeer.compare(it->options.blePeer, Qt::CaseInsensitive))
                return reject(id, "otg.multi.duplicate_device");
            requireDirected = true;
#elif defined(OTG_GUI_HAVE_BLE_WIN)
            if (!options.preferDirectedBle || !it->options.preferDirectedBle)
                return reject(id, "otg.multi.ble_limit");
            if (!options.blePeer.isEmpty() && !options.blePeer.compare(it->options.blePeer, Qt::CaseInsensitive))
                return reject(id, "otg.multi.duplicate_device");
#else
            return reject(id, "otg.multi.ble_limit");
#endif
        }
    }
    if (busy() || m_server.state() != DeskflowServer::State::Idle) {
        if (options.manageServer != m_shared.manageServer || options.port != m_shared.port ||
            options.host != m_shared.host || options.computer != m_shared.computer ||
            options.deskflow != m_shared.deskflow || options.tls != m_shared.tls ||
            options.tlsCertificate != m_shared.tlsCertificate ||
            options.tlsFingerprint != m_shared.tlsFingerprint ||
            options.tlsFingerprintFile != m_shared.tlsFingerprintFile ||
            (options.manageServer && options.deskflowConfig != m_shared.deskflowConfig))
            return reject(id, "otg.multi.shared_locked");
        if (m_server.state() == DeskflowServer::State::Stopping)
            return reject(id, "otg.multi.shared_locked");
    } else {
        m_shared = options;
    }
    auto &entry = m_devices[id];
    entry.options = options;
    entry.options.requireDirectedBle = requireDirected;
    entry.errorId.clear();
    entry.errorDetail.clear();
    entry.pending = true;
#ifdef OTG_GUI_HAVE_BLE_WIN
    if (options.transport == "ble" && options.preferDirectedBle) {
        if (m_windowsBleStopping || (!m_windowsBleProgram.isEmpty() && m_windowsBleProgram != options.bridge)) {
            entry.pending = false;
            return reject(id, "otg.ble.provider_target");
        }
        if (m_windowsBleToken.isEmpty()) {
            m_windowsBleToken = QUuid::createUuid().toString(QUuid::Id128);
            m_windowsBleProgram = options.bridge;
            m_windowsBleEvents.clear();
            m_windowsBleService.start(options.bridge, {"--ble-win-multi-service", m_windowsBleToken});
            m_windowsBleDeadline.start(15000);
        }
        entry.options.windowsBleToken = m_windowsBleToken;
    }
#endif
    emit changed(id);
    if (options.bleDiscovery || !options.manageServer || m_server.state() == DeskflowServer::State::Running) launchPending();
    else if (m_server.state() == DeskflowServer::State::Idle)
        m_server.start({options.deskflow, options.computer, options.deskflowConfig, options.port,
                        options.tls, options.tlsCertificate});
    return true;
}

void ConnectionManager::launchPending()
{
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (!it->pending) continue;
        if (!it->options.windowsBleToken.isEmpty() && !m_windowsBleReady) continue;
        if (it->options.manageServer && !it->options.bleDiscovery &&
            m_server.state() != DeskflowServer::State::Running) continue;
        auto options = it->options;
        options.host = options.manageServer ? "127.0.0.1" : options.host;
        options.manageServer = false; // The manager, never a child session, owns the shared server.
        it->pending = false;
        if (options.transport == "ble" && options.bleDiscovery)
            it->waitingForBleAttach = true;
        it->session->start(options);
    }
}

void ConnectionManager::configureWaitingBle()
{
    if (m_server.state() == DeskflowServer::State::Starting ||
        m_server.state() == DeskflowServer::State::Stopping)
        return;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (!it->waitingForBleAttach || it->options.bleDiscovery || it->session->state() != Session::State::Starting) continue;
        auto options = it->options;
        options.bleDiscovery = false;
        if (options.manageServer) {
            options.host = "127.0.0.1";
            options.manageServer = false;
        }
        if (it->session->configureBle(options)) {
            it->waitingForBleAttach = false;
            emit changed(it.key());
        }
    }
}

bool ConnectionManager::attachBle(const QString &id, const ConnectionOptions &options)
{
    if (!m_devices.contains(id) || options.transport != "ble" ||
        state(id) != Session::State::Starting || !m_devices[id].waitingForBleAttach)
        return false;
    if (m_server.state() == DeskflowServer::State::Stopping) return false;
    m_devices[id].options = options;
    if (options.preferDirectedBle) m_devices[id].options.windowsBleToken = m_windowsBleToken;
    if (options.manageServer && m_server.state() == DeskflowServer::State::Idle) {
        m_server.start({options.deskflow, options.computer, options.deskflowConfig, options.port,
                        options.tls, options.tlsCertificate});
        return true;
    }
    if (m_server.state() == DeskflowServer::State::Starting) return true;
    configureWaitingBle();
    return !m_devices[id].waitingForBleAttach;
}

void ConnectionManager::update(const QString &id)
{
    finishWindowsBleStop();
    if (!busy()) m_server.stop();
    emit changed(id);
}

void ConnectionManager::stop(const QString &id)
{
    if (!m_devices.contains(id)) return;
    auto &entry = m_devices[id];
    entry.pending = false;
    entry.waitingForBleAttach = false;
    entry.session->stop();
    update(id);
}

void ConnectionManager::stopAll()
{
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) it->pending = false;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        it->session->stop();
        emit changed(it.key());
    }
    if (!busy()) m_server.stop();
}

void ConnectionManager::stopBle()
{
    m_windowsBleStopping = m_windowsBleService.state() != QProcess::NotRunning;
    const auto ids = m_devices.keys();
    for (const auto &id : ids) {
        const auto &entry = m_devices[id];
        if (entry.session->hasBleProvider()) entry.session->stopBleProvider();
        if (entry.options.transport == "ble") stop(id);
    }
    if (!busy()) m_server.stop();
    finishWindowsBleStop();
}

void ConnectionManager::finishWindowsBleStop()
{
    if (!m_windowsBleStopping || m_windowsBleService.state() == QProcess::NotRunning) return;
    for (const auto &entry : m_devices)
        if (entry.session->hasBleProvider() ||
            (entry.options.transport == "ble" && entry.session->state() != Session::State::Idle)) return;
    m_windowsBleService.closeWriteChannel();
    if (!m_windowsBleDeadline.isActive()) m_windowsBleDeadline.start(15000);
}

void ConnectionManager::shutdown()
{
    m_shutdown = true;
    m_windowsBleStopping = m_windowsBleService.state() != QProcess::NotRunning;
    for (auto &entry : m_devices) entry.pending = false;
    for (auto &entry : m_devices) entry.session->shutdown();
    if (!busy()) m_server.stop();
    finishWindowsBleStop();
    emit changed({});
}

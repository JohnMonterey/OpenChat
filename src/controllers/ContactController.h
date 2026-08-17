#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <optional>

#include "domain/Identifiers.h"
#include "models/RequestListModel.h"

namespace OpenChat {

class AddContactService;
class ContactRequestService;
class ProfileSession;
class RelayClient;
class SyncEngine;

// The QML-facing bridge for the add-contact surface. It owns the inbound-request
// model and the transient AddContactService (send path), and drives the durable
// ContactRequestService (receive path), surfacing every outcome as a Status +
// human status message.
//
// Dual-world: the QML this controller backs is shared between the --capture mock
// path (no session/services) and the live path (real ProfileSession + services).
// In mock mode every operation resolves locally with a simulated outcome; the live
// seam is installed out-of-band through setLiveServices() and is never touched
// under --capture. The controller is exposed to QML as a separate, optional context
// property that defaults to null, so the byte-identical default chat capture is
// unaffected.
class ContactController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(RequestListModel *requests READ requests CONSTANT)
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(bool dialogOpen READ dialogOpen NOTIFY dialogOpenChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(QString myInvite READ myInvite NOTIFY myInviteChanged)
    Q_PROPERTY(bool inviteReady READ inviteReady NOTIFY myInviteChanged)
    Q_PROPERTY(bool safetyNumberOpen READ safetyNumberOpen NOTIFY safetyNumberChanged)
    Q_PROPERTY(QString safetyNumber READ safetyNumber NOTIFY safetyNumberChanged)
    Q_PROPERTY(bool safetyNumberVerified READ safetyNumberVerified NOTIFY safetyNumberChanged)
    Q_PROPERTY(QString safetyNumberContact READ safetyNumberContact NOTIFY safetyNumberChanged)

public:
    enum class Status { Idle, Working, Success, Error };
    Q_ENUM(Status)

    explicit ContactController(QObject *parent = nullptr);
    ~ContactController() override;

    ContactController(const ContactController &) = delete;
    ContactController &operator=(const ContactController &) = delete;

    [[nodiscard]] RequestListModel *requests();
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool dialogOpen() const;
    [[nodiscard]] Status status() const;
    [[nodiscard]] QString statusMessage() const;
    [[nodiscard]] QString myInvite() const;
    [[nodiscard]] bool inviteReady() const;
    [[nodiscard]] bool safetyNumberOpen() const;
    [[nodiscard]] QString safetyNumber() const;
    [[nodiscard]] bool safetyNumberVerified() const;
    [[nodiscard]] QString safetyNumberContact() const;

    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void closeDialog();
    Q_INVOKABLE void addByHandle(const QString &handle);
    Q_INVOKABLE void addByInvite(const QString &inviteText);
    Q_INVOKABLE void createMyInvite();
    Q_INVOKABLE void accept(const QString &requestId);
    Q_INVOKABLE void decline(const QString &requestId);
    Q_INVOKABLE void block(const QString &requestId);

    // Safety-number surface. openSafetyNumber(hex) resolves the contact's peer key
    // and our own identity into the formatted number; markVerified() records the
    // local user's assertion for the currently open contact (arg-less: it acts on
    // the contact the dialog is showing, so QML never handles a raw account id).
    Q_INVOKABLE void openSafetyNumber(const QString &contactId);
    Q_INVOKABLE void closeSafetyNumber();
    Q_INVOKABLE void markVerified();

    // C++-only live-services seam. Null-tolerant and never called under --capture:
    // installs the real receive/send dependencies, flips enabled, seeds the request
    // model from the durable roster and connects the request service's signals.
    void setLiveServices(ContactRequestService *requests, RelayClient *relay,
                         ProfileSession *session, SyncEngine *engine);

    // C++-only preview seeding, only used by main.cpp's --add-contact sub-mode.
    void enableForPreview();
    void addMockRequest(const QString &displayName, const QString &subtitle);
    void setMockInvite(const QString &inviteText);
    // C++-only safety-number preview seeding, used by the --verify sub-mode and
    // tst_qmlload: a preset grouped number + verified flag + display label with no
    // live session behind it. openSafetyNumberPreview() reveals the dialog.
    void setMockSafetyNumber(const QString &groupedNumber, bool verified,
                             const QString &contactLabel);
    void openSafetyNumberPreview();

signals:
    void enabledChanged();
    void dialogOpenChanged();
    void statusChanged();
    void myInviteChanged();
    void safetyNumberChanged();

private:
    void setStatus(Status status, const QString &message);
    // Creates a fresh AddContactService for one attempt (recreated per call) and
    // wires its succeeded/failed outcomes. Returns nullptr in mock mode.
    AddContactService *beginAdd();
    void seedFromRoster();
    void clearInviteConnections();

    RequestListModel m_requests;
    bool m_enabled = false;
    bool m_dialogOpen = false;
    Status m_status = Status::Idle;
    QString m_statusMessage;
    QString m_myInvite;
    QString m_mockInvite; // preset returned by createMyInvite() in mock mode

    // Safety-number surface. m_safetyNumberAccount is populated only on the live
    // open path (the contact the dialog is showing); markVerified() persists the
    // verified flag against it. In mock mode it stays empty and the preset from
    // setMockSafetyNumber() drives the surface.
    bool m_safetyNumberOpen = false;
    QString m_safetyNumber;
    bool m_safetyNumberVerified = false;
    QString m_safetyNumberContact;
    std::optional<AccountId> m_safetyNumberAccount;

    // Live seam (nullptr in mock mode). Borrowed; owned by the app runtime and kept
    // alive past this controller.
    ContactRequestService *m_requestsSvc = nullptr;
    RelayClient *m_relay = nullptr;
    ProfileSession *m_session = nullptr;
    SyncEngine *m_engine = nullptr;

    // Per-attempt send state machine; recreated on every add.
    std::unique_ptr<AddContactService> m_pendingAdd;
    // One-shot connections to the shared relay for a createMyInvite() attempt.
    QList<QMetaObject::Connection> m_inviteConnections;
};

} // namespace OpenChat

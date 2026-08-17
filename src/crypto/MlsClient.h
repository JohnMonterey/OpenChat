#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QMutex>

#include <memory>

struct oc_mls_client;

namespace OpenChat {

enum class MlsError {
    InvalidInput = 1,
    MissingGroup = 2,
    InvalidMessage = 3,
    Crypto = 4,
    Storage = 5,
    Unsupported = 6,
    Internal = 7,
};

class MlsStateStore
{
public:
    virtual ~MlsStateStore() = default;

    [[nodiscard]] virtual Result<QByteArray, MlsError> load() = 0;
    // The implementation must atomically replace the previous opaque snapshot.
    // The view is valid only for the duration of this call.
    [[nodiscard]] virtual Result<void, MlsError> store(QByteArrayView state) = 0;
};

struct MlsAddResult final {
    QByteArray commit;
    QByteArray welcome;
};

struct MlsCiphertext final {
    QByteArray bytes;
};

enum class MlsProcessKind {
    Application,
    Proposal,
    Commit,
};

struct MlsProcessResult final {
    MlsProcessKind kind = MlsProcessKind::Application;
    QByteArray applicationData;
    // MLS-authenticated sender credential for an application message (empty for
    // proposals/commits). The caller binds the plaintext to this identity rather
    // than to any relay-supplied envelope field.
    QByteArray senderIdentity;
};

class MlsClient final
{
public:
    // stateStore is non-owning and must outlive the returned client.
    [[nodiscard]] static Result<std::unique_ptr<MlsClient>, MlsError>
    create(QByteArrayView identity, MlsStateStore *stateStore = nullptr);

    ~MlsClient();
    MlsClient(const MlsClient &) = delete;
    MlsClient &operator=(const MlsClient &) = delete;

    [[nodiscard]] Result<QByteArray, MlsError> generateKeyPackage();
    [[nodiscard]] Result<void, MlsError> createGroup(const ConversationId &conversation);
    [[nodiscard]] Result<void, MlsError> joinGroup(const ConversationId &conversation,
                                                   QByteArrayView welcome);
    // Returns the MLS-authenticated credential identities of the Welcome's other
    // members (excluding self) without joining the group. Read-only: no ratchet
    // or state change. The caller authenticates one of these against the claimed
    // sender before any durable joinGroup.
    [[nodiscard]] Result<QList<QByteArray>, MlsError> inspectWelcome(QByteArrayView welcome);
    [[nodiscard]] Result<MlsAddResult, MlsError>
    addMembers(const ConversationId &conversation, const QList<QByteArray> &keyPackages);
    [[nodiscard]] Result<QByteArray, MlsError>
    removeMembers(const ConversationId &conversation, const QList<QByteArray> &identities);
    [[nodiscard]] Result<MlsCiphertext, MlsError>
    encrypt(const ConversationId &conversation, QByteArrayView plaintext);
    [[nodiscard]] Result<MlsProcessResult, MlsError>
    process(const ConversationId &conversation, QByteArrayView mlsMessage);

private:
    struct CallbackContext;

    MlsClient(std::unique_ptr<CallbackContext> callbackContext, oc_mls_client *handle);

    std::unique_ptr<CallbackContext> m_callbackContext;
    oc_mls_client *m_handle = nullptr;
    QMutex m_mutex;
};

} // namespace OpenChat

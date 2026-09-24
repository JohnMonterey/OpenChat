#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "repositories/RepositoryError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

#include <optional>

namespace OpenChat {

// The local user's page: the editor's draft and the page last published. Cores
// are the exact encoded PageCore bytes (domain/ProfilePageCodec.h); the hashes
// name blobs in the media store.
struct StoredLocalPage final {
    QByteArray draftCore; // empty: no draft
    std::optional<QByteArray> draftBackground;
    std::optional<QByteArray> draftSong;
    QString draftSongSource; // JSON, editor-only
    qint64 draftUpdatedAtMs = 0;
    QByteArray publishedCore; // empty: never published
    qint64 publishedRevision = 0;
    std::optional<QByteArray> publishedBackground;
    std::optional<QByteArray> publishedSong;
    qint64 publishedAtMs = 0;
};

// The newest page a contact sent us, with the media refs its core names.
struct StoredContactPage final {
    AccountId account;
    qint64 revision = 0;
    QByteArray core;
    std::optional<QByteArray> background;
    std::optional<QByteArray> song;
    qint64 receivedAtMs = 0;
    qint64 viewedAtMs = 0;
};

// Owner side: what one contact has been sent, against which of their devices,
// and how much of the request-answer budget they used.
struct PageDelivery final {
    AccountId account;
    std::optional<DeviceId> device;
    qint64 sentRevision = -1;
    qint64 sentAtMs = 0;
    bool pageCapable = false;
    qint64 lastAnswerAtMs = 0;
    qint64 answerWindowStartMs = 0;
    int answersInWindow = 0;
};

// Viewer side: the back-off state of our requests to one contact.
struct PageRequestState final {
    AccountId account;
    qint64 lastRequestAtMs = 0;
    int unanswered = 0;
    qint64 mediaRequestedRevision = -1;
};

// Durable storage for profile pages (migration 016), for one local profile.
//
// Media blobs are named by their SHA-256 and carry a kind: 1 a background
// image, 2 a song (Profile::MediaKind, kept as a plain int so storage does not
// depend on the page model). Every hash argument must be 32 bytes, every kind
// 1 or 2 and every blob 1 byte to 224 KiB, or the call fails with
// InvalidInput. A blob must hash to the hash it is stored under (InvalidInput
// otherwise): the store is content-addressed, so one mislabelled blob would
// otherwise stand in for the real one for every later sender.
//
// Received media is owner-scoped: a blob is stored once, but it is readable
// for a contact only through that contact's own record of having sent it, and
// only as the kind it arrived as. So no contact can probe whether we hold a
// blob someone else sent, and a blob declared as a song is never shown as a
// background.
//
// Page rows have no foreign key to contacts (see the migration); rows of
// accounts that are no longer Accepted contacts are dropped by
// dropPagesOfNonContacts(), which every collection runs first.
class ProfilePageRepository {
public:
    virtual ~ProfilePageRepository() = default;
    // --- Local media. Stores the blob AND points the draft's slot at it in one
    //     transaction (creating the local row if needed), so no collection can
    //     delete a just-imported blob before the draft names it.
    [[nodiscard]] virtual Result<void, RepositoryError> putLocalDraftMedia(int kind, QByteArrayView sha256,
                                                                           QByteArrayView data, qint64 nowMs) = 0;
    [[nodiscard]] virtual Result<std::optional<QByteArray>, RepositoryError> localMedia(QByteArrayView sha256) = 0; // only if the local page references it
    // --- Contact media, owner-scoped. putContactMedia: blob INSERT OR IGNORE +
    //     contact_page_media row (account, sha256, kind), one transaction.
    [[nodiscard]] virtual Result<void, RepositoryError> putContactMedia(const AccountId &account, int kind,
                                                                        QByteArrayView sha256, QByteArrayView data, qint64 nowMs) = 0;
    // Returns data only when THIS account sent this hash AS this kind.
    [[nodiscard]] virtual Result<std::optional<QByteArray>, RepositoryError> contactMedia(const AccountId &account,
                                                                                         QByteArrayView sha256, int kind) = 0;
    [[nodiscard]] virtual Result<bool, RepositoryError> hasContactMedia(const AccountId &account, QByteArrayView sha256, int kind) = 0;
    [[nodiscard]] virtual Result<int, RepositoryError> pendingContactMediaCount(const AccountId &account) = 0; // rows its core does not name
    // --- Collection (§3.5). Both arguments are cut-off times: pending contact
    //     media received before the first, and unreferenced blobs stored before
    //     the second, are deleted. Returns blobs deleted.
    [[nodiscard]] virtual Result<int, RepositoryError> collectGarbage(qint64 pendingOlderThanMs, qint64 localBlobOlderThanMs) = 0;
    // Bytes of the blobs contacts sent us that our own page does not also use.
    [[nodiscard]] virtual Result<qint64, RepositoryError> receivedMediaBytes() = 0;
    // Forgets every blob this account sent (their core stays), and deletes the
    // blobs nothing else references at once, grace or not: eviction exists to
    // free space now.
    [[nodiscard]] virtual Result<void, RepositoryError> evictContactMedia(const AccountId &account) = 0;  // core stays
    // Accounts holding received media, least recently viewed first (ORDER BY
    // viewed_at_ms, received_at_ms; an account whose media overtook its core
    // counts as never viewed). Accounts without media have nothing to evict.
    [[nodiscard]] virtual Result<QVector<AccountId>, RepositoryError> contactsLeastRecentlyViewed() = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> dropPagesOfNonContacts() = 0;  // every page/media/delivery/request row whose account is not state 2
    // --- Local page (the repository is constructed for one profile id).
    [[nodiscard]] virtual Result<StoredLocalPage, RepositoryError> localPage() = 0;   // defaults when no row
    // An empty core is InvalidInput (a draft is always an encoded core; use
    // clearDraft() to drop one). An empty songSource is stored as none.
    [[nodiscard]] virtual Result<void, RepositoryError> saveDraft(QByteArrayView core, const std::optional<QByteArray> &background,
                                                                  const std::optional<QByteArray> &song,
                                                                  const QString &songSource, qint64 nowMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> clearDraft() = 0;
    // Also clears the draft, in the same transaction. The revision must be
    // above the one published before (Conflict otherwise): contacts keep only
    // the highest revision they saw, so a lower one would never show.
    [[nodiscard]] virtual Result<void, RepositoryError> savePublished(QByteArrayView core, qint64 revision,
                                                                      const std::optional<QByteArray> &background,
                                                                      const std::optional<QByteArray> &song, qint64 nowMs) = 0;
    // --- Contacts' pages. storeContactPage returns false (and stores nothing) when revision <= stored.
    //     A newer revision keeps the stored viewedAtMs: the viewer's last look
    //     is about the contact, not about one revision of their page.
    [[nodiscard]] virtual Result<std::optional<StoredContactPage>, RepositoryError> contactPage(const AccountId &account) = 0;
    [[nodiscard]] virtual Result<bool, RepositoryError> storeContactPage(const StoredContactPage &page) = 0;
    // A no-op for an account with no stored page.
    [[nodiscard]] virtual Result<void, RepositoryError> markViewed(const AccountId &account, qint64 nowMs) = 0;
    // --- Owner-side delivery tracking.
    [[nodiscard]] virtual Result<PageDelivery, RepositoryError> delivery(const AccountId &account) = 0; // defaults when absent
    [[nodiscard]] virtual Result<void, RepositoryError> saveDelivery(const PageDelivery &delivery) = 0;
    [[nodiscard]] virtual Result<std::optional<qint64>, RepositoryError> mediaSentAt(const AccountId &account, QByteArrayView sha256) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> recordMediaSent(const AccountId &account, QByteArrayView sha256, qint64 nowMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> forgetSentMedia(const AccountId &account) = 0;          // device changed
    [[nodiscard]] virtual Result<void, RepositoryError> forgetSentMediaExcept(const QVector<QByteArray> &keep) = 0; // after publish
    // --- Viewer-side request throttle.
    [[nodiscard]] virtual Result<PageRequestState, RepositoryError> requestState(const AccountId &account) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> saveRequestState(const PageRequestState &state) = 0;
};

} // namespace OpenChat

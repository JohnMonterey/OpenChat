#pragma once

#include "app/ProfilePageSync.h"
#include "controllers/ProfileEditHistory.h"
#include "controllers/ProfilePageObject.h"
#include "domain/Contact.h"
#include "domain/Identifiers.h"
#include "domain/ProfilePage.h"
#include "models/Contact.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace OpenChat {

class ChatController;
class ContactRequestService;
class ProfileBackgroundImporter;
class ProfileSession;
class RelayClient;
class SongImporter;
class SyncEngine;
struct SongSourceInfo;

// The profile pages' QML API (ARCH §7): which profile is open and how the
// user got there, who that person is, the page on screen, and the owner's
// editor for their own page.
//
// Identity is the app's, never the page's: the name, @handle, picture,
// presence and relationship come from the roster, the local profile or the
// relay, so a page can style a name but never claim one. The page itself is
// one of three long-lived page objects QML binds to:
//   view   what is on screen (read-only);
//   draft  the owner's editor copy (writable, autosaved 400 ms after a change);
//   tryOn  the draft with a hovered preset applied (read-only).
//
// ChatController creates and owns this controller (declared last among its
// members, so it dies first) and forwards its live services. Without them it
// runs the reference mock: the mock roster's seeded pages and an in-memory
// own page. With them it runs a ProfilePageSync over the session. Everything
// here works without a QGuiApplication (tst_e2e is GUI-less): the fonts are
// registered only when there is one, and the clipboard is guarded.
class ProfileController final : public QObject
{
    Q_OBJECT
    // --- Navigation: a back stack of {person, scrollY}, at most 10 entries (the oldest goes)
    Q_PROPERTY(bool open READ isOpen NOTIFY navigationChanged)
    Q_PROPERTY(int depth READ depth NOTIFY navigationChanged)
    Q_PROPERTY(int origin READ origin NOTIFY navigationChanged) // Profile.Origin of the stack
    // "Chat"/"Call"/"Requests"/"Search"/"Settings" at depth 1, the previous
    // person's first name deeper, "Profile" while editing.
    Q_PROPERTY(QString backLabel READ backLabel NOTIFY navigationChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY navigationChanged) // [{index, name, avatarKey}], newest first
    Q_PROPERTY(qreal entryScrollY READ entryScrollY NOTIFY navigationChanged) // 0 on push, the saved value on pop
    // --- The person on screen: app-owned identity, never taken from their page
    Q_PROPERTY(QString personId READ personId NOTIFY personChanged)
    Q_PROPERTY(QString personAccountId READ personAccountId NOTIFY personChanged)
    Q_PROPERTY(int relationship READ relationship NOTIFY personChanged) // Profile.Relationship
    Q_PROPERTY(bool isOwnProfile READ isOwnProfile NOTIFY personChanged)
    Q_PROPERTY(QString personName READ personName NOTIFY personChanged)
    Q_PROPERTY(QString personFirstName READ personFirstName NOTIFY personChanged)
    Q_PROPERTY(QString personHandle READ personHandle NOTIFY personChanged) // canonical, no '@'; "" unknown
    Q_PROPERTY(bool personHandlePending READ personHandlePending NOTIFY personChanged)
    Q_PROPERTY(bool personBlocked READ personBlocked NOTIFY personChanged)
    Q_PROPERTY(bool personVerified READ personVerified NOTIFY personChanged)
    Q_PROPERTY(QString personAvatarKey READ personAvatarKey NOTIFY personChanged)
    Q_PROPERTY(int personPresence READ personPresence NOTIFY personChanged) // Offline when unreachable
    Q_PROPERTY(bool personOnline READ personOnline NOTIFY personChanged)   // "Online Now!"
    Q_PROPERTY(QString personStatusLine READ personStatusLine NOTIFY personChanged)
    Q_PROPERTY(QString requestId READ requestId NOTIFY personChanged)       // IncomingRequestPerson only
    Q_PROPERTY(QString referrerName READ referrerName NOTIFY personChanged) // Friend Space stubs
    Q_PROPERTY(int pageState READ pageState NOTIFY personChanged)           // Profile.PageState
    Q_PROPERTY(bool pageIncomplete READ pageIncomplete NOTIFY personChanged)
    Q_PROPERTY(bool pageLoading READ pageLoading NOTIFY personChanged)
    // --- Page objects
    Q_PROPERTY(OpenChat::ProfilePageObject *view READ view CONSTANT)
    Q_PROPERTY(OpenChat::ProfilePageObject *draft READ draft CONSTANT)
    Q_PROPERTY(OpenChat::ProfilePageObject *tryOn READ tryOn CONSTANT)
    // --- Viewer inputs
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY viewerChanged)
    Q_PROPERTY(bool plainStyle READ plainStyle WRITE setPlainStyle NOTIFY viewerChanged)
    Q_PROPERTY(bool callActive READ callActive WRITE setCallActive NOTIFY viewerChanged)
    Q_PROPERTY(bool linkOnline READ linkOnline NOTIFY viewerChanged)
    // --- Own identity
    Q_PROPERTY(QString localAccountId READ localAccountId NOTIFY localIdentityChanged)
    Q_PROPERTY(QString localHandle READ localHandle NOTIFY localIdentityChanged)
    // --- Editing (a property of the top entry: only while it is the own profile)
    Q_PROPERTY(bool editing READ editing NOTIFY editingChanged)
    Q_PROPERTY(bool draftDirty READ draftDirty NOTIFY draftDirtyChanged)
    Q_PROPERTY(qint64 publishedRevision READ publishedRevision NOTIFY publishedChanged)
    Q_PROPERTY(bool publishPending READ publishPending NOTIFY publishedChanged)
    Q_PROPERTY(qint64 survivingDraftAtMs READ survivingDraftAtMs NOTIFY editingChanged)
    Q_PROPERTY(int lastTab READ lastTab WRITE setLastTab NOTIFY editingChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(int tryOnPreset READ tryOnPreset WRITE setTryOnPreset NOTIFY tryOnChanged)
    // --- Media editing
    Q_PROPERTY(bool backgroundImporting READ backgroundImporting NOTIFY importChanged)
    Q_PROPERTY(qreal backgroundImportProgress READ backgroundImportProgress NOTIFY importChanged)
    Q_PROPERTY(bool songImporting READ songImporting NOTIFY importChanged)
    Q_PROPERTY(QVariantMap songSource READ songSource NOTIFY importChanged) // {fileName, formatLabel, durationMs, available}
    Q_PROPERTY(QVariantList songPeaks READ songPeaks NOTIFY importChanged)
    Q_PROPERTY(qint64 songWindowStartMs READ songWindowStartMs NOTIFY importChanged)
    Q_PROPERTY(qint64 songWindowMs READ songWindowMs NOTIFY importChanged)
    Q_PROPERTY(qint64 songClipBytes READ songClipBytes NOTIFY importChanged)
    // Accepted contacts: [{contactId, name, avatarKey, placedIndex}]
    Q_PROPERTY(QVariantList topFriendCandidates READ topFriendCandidates NOTIFY topFriendCandidatesChanged)
    Q_PROPERTY(QVariantList recentColors READ recentColors NOTIFY recentColorsChanged)
    // --- Catalogues
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    Q_PROPERTY(QVariantList fontChoices READ fontChoices CONSTANT)
    Q_PROPERTY(QVariantList motifChoices READ motifChoices CONSTANT)
    Q_PROPERTY(QVariantList nameEffectChoices READ nameEffectChoices CONSTANT)
    Q_PROPERTY(QVariantList flourishChoices READ flourishChoices CONSTANT)
    Q_PROPERTY(QVariantList ambientChoices READ ambientChoices CONSTANT)
    Q_PROPERTY(QVariantList zodiacChoices READ zodiacChoices CONSTANT)
    Q_PROPERTY(QVariantList moodChoices READ moodChoices CONSTANT)
    Q_PROPERTY(QVariantList hereForChoices READ hereForChoices CONSTANT)
    Q_PROPERTY(QVariantMap limits READ limits CONSTANT)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    static constexpr int maxStackDepth = 10;
    static constexpr int autosaveDelayMs = 400;
    static constexpr int songWindowDelayMs = 400;
    static constexpr int refreshIntervalMs = 5'000;
    static constexpr int maxRecentColors = 8;

    // How the ProfilePageSync this controller builds keeps time and picks
    // delays. Tests replace production's wall clock and random source (and
    // its limits) so background requests stay out of exact envelope counts.
    struct SyncTuning final {
        ProfilePageSync::Clock clock;   // empty: the wall clock
        ProfilePageSync::Random random; // empty: QRandomGenerator
        ProfilePageSync::Limits limits;
    };
    // Every sync built from now on uses `tuning`; nullopt restores production.
    static void setSyncTuningForTesting(std::optional<SyncTuning> tuning);

    explicit ProfileController(ChatController &chats, QObject *parent = nullptr);
    ~ProfileController() override; // flushes a pending autosave

    // Navigation. The open* calls start a new stack; origin -1 = from the
    // chat surface's section.
    Q_INVOKABLE bool openContact(const QString &contactId, int origin = -1); // false for groups and unknown ids
    Q_INVOKABLE void openOwn(int origin = -1);
    Q_INVOKABLE void openRequest(const QString &requestId, const QString &accountId, const QString &name,
                                 const QString &handle);
    Q_INVOKABLE void openHandle(const QString &handle);
    Q_INVOKABLE void openPerson(const QString &accountId, const QString &name, const QString &avatarKey);
    // Pushes the view's Top Friend `index`, or goes back to them when they are
    // already on the stack; a no-op for the page on screen and while editing.
    Q_INVOKABLE bool openTopFriend(int index);
    // Pops one entry (closing at depth 1). In the editor it leaves the editor.
    Q_INVOKABLE void back();
    Q_INVOKABLE void popTo(int index);
    Q_INVOKABLE void closeAll();
    // The view reports its scroll position; it is restored when this entry is
    // popped back to.
    Q_INVOKABLE void setEntryScrollY(qreal y);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void copyHandle();
    Q_INVOKABLE void copyText(const QString &text, const QString &notice);
    Q_INVOKABLE void clearNotice();

    // Own page editing
    Q_INVOKABLE bool beginEditing();
    Q_INVOKABLE void continueDraft();
    Q_INVOKABLE void startOver();
    Q_INVOKABLE bool publish();
    Q_INVOKABLE void discardChanges();
    Q_INVOKABLE void endEditing();
    Q_INVOKABLE void applyPreset(int preset);
    Q_INVOKABLE void setTryOnPreset(int preset);
    Q_INVOKABLE void resetToPreset();
    Q_INVOKABLE void surpriseMe();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void beginGesture(const QString &key);
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void importBackground(const QUrl &file);
    Q_INVOKABLE void removeBackgroundImage();
    Q_INVOKABLE void importSong(const QUrl &file);
    Q_INVOKABLE void setSongWindow(qint64 startMs);
    Q_INVOKABLE void removeSong();
    Q_INVOKABLE void cancelImports();
    Q_INVOKABLE bool addTopFriend(const QString &contactId);
    Q_INVOKABLE void removeTopFriend(int index);
    Q_INVOKABLE void moveTopFriend(int from, int to);
    Q_INVOKABLE void moveModule(int module, int column, int index);
    Q_INVOKABLE void setModuleVisible(int module, bool visible);
    Q_INVOKABLE void rememberColor(const QColor &color);
    Q_INVOKABLE QVariantMap contrastFor(int inkRole, const QColor &color) const;

    // C++ seams
    // Live mode: builds the ProfilePageSync over the session (after its
    // networking started) and drops the mock. All three are borrowed; the
    // sync is dropped when the engine goes, before the session locks.
    void setLiveServices(ProfileSession *session, SyncEngine *engine, ContactRequestService *requests);
    void setRelay(RelayClient *relay); // own-handle backfill, stranger handle lookups
    // Previews and tests: a mock contact's page, or the own page for "self".
    void setMockPage(const QString &contactId, const Profile::Page &page);
    // Makes a blob available to mock pages that name it; returns its ref.
    Profile::MediaRef addMockMedia(Profile::MediaKind kind, const QByteArray &bytes);
    void setMockLinkOnline(bool online);
    [[nodiscard]] ProfilePageSync *sync() const noexcept { return m_sync.get(); }

    [[nodiscard]] bool isOpen() const noexcept { return !m_stack.isEmpty(); }
    [[nodiscard]] int depth() const noexcept { return int(m_stack.size()); }
    [[nodiscard]] int origin() const noexcept { return int(m_origin); }
    [[nodiscard]] QString backLabel() const;
    [[nodiscard]] QVariantList history() const;
    [[nodiscard]] qreal entryScrollY() const noexcept { return m_entryScrollY; }

    [[nodiscard]] QString personId() const { return m_person.id; }
    [[nodiscard]] QString personAccountId() const { return m_person.accountHex; }
    [[nodiscard]] int relationship() const { return int(m_person.relationship); }
    [[nodiscard]] bool isOwnProfile() const { return m_person.relationship == Profile::Relationship::SelfPerson; }
    [[nodiscard]] QString personName() const { return m_person.name; }
    [[nodiscard]] QString personFirstName() const { return m_person.firstName; }
    [[nodiscard]] QString personHandle() const { return m_person.handle; }
    [[nodiscard]] bool personHandlePending() const { return m_person.handlePending; }
    [[nodiscard]] bool personBlocked() const { return m_person.blocked; }
    [[nodiscard]] bool personVerified() const { return m_person.verified; }
    [[nodiscard]] QString personAvatarKey() const { return m_person.avatarKey; }
    [[nodiscard]] int personPresence() const { return m_person.presence; }
    [[nodiscard]] bool personOnline() const { return m_person.online; }
    [[nodiscard]] QString personStatusLine() const { return m_person.statusLine; }
    [[nodiscard]] QString requestId() const { return m_person.requestId; }
    [[nodiscard]] QString referrerName() const { return m_person.referrerName; }
    [[nodiscard]] int pageState() const { return int(m_person.pageState); }
    [[nodiscard]] bool pageIncomplete() const { return m_person.pageIncomplete; }
    [[nodiscard]] bool pageLoading() const { return m_person.pageLoading; }

    [[nodiscard]] ProfilePageObject *view() noexcept { return &m_view; }
    [[nodiscard]] ProfilePageObject *draft() noexcept { return &m_draft; }
    [[nodiscard]] ProfilePageObject *tryOn() noexcept { return &m_tryOn; }

    [[nodiscard]] bool darkMode() const noexcept { return m_darkMode; }
    void setDarkMode(bool dark);
    [[nodiscard]] bool plainStyle() const noexcept { return m_plainStyle; }
    void setPlainStyle(bool plain);
    [[nodiscard]] bool callActive() const noexcept { return m_callActive; }
    void setCallActive(bool active);
    [[nodiscard]] bool linkOnline() const noexcept { return m_linkOnline; }

    [[nodiscard]] QString localAccountId() const;
    [[nodiscard]] QString localHandle() const { return m_localHandle; }

    [[nodiscard]] bool editing() const noexcept { return m_editing; }
    [[nodiscard]] bool draftDirty() const noexcept { return m_draftDirty; }
    [[nodiscard]] qint64 publishedRevision() const;
    [[nodiscard]] bool publishPending() const noexcept { return m_publishPending; }
    [[nodiscard]] qint64 survivingDraftAtMs() const noexcept { return m_survivingDraftAtMs; }
    [[nodiscard]] int lastTab() const noexcept { return m_lastTab; }
    void setLastTab(int tab);
    [[nodiscard]] bool canUndo() const noexcept { return m_history.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return m_history.canRedo(); }
    [[nodiscard]] int tryOnPreset() const noexcept { return m_tryOnPreset; }

    [[nodiscard]] bool backgroundImporting() const noexcept { return m_backgroundImporting; }
    [[nodiscard]] qreal backgroundImportProgress() const noexcept { return m_backgroundProgress; }
    [[nodiscard]] bool songImporting() const;
    [[nodiscard]] QVariantMap songSource() const;
    [[nodiscard]] QVariantList songPeaks() const;
    [[nodiscard]] qint64 songWindowStartMs() const { return shownSongSource().windowStartMs; }
    [[nodiscard]] qint64 songWindowMs() const;
    [[nodiscard]] qint64 songClipBytes() const;
    [[nodiscard]] QVariantList topFriendCandidates() const { return m_candidates; }
    [[nodiscard]] QVariantList recentColors() const;

    [[nodiscard]] static QVariantList presets();
    [[nodiscard]] static QVariantList fontChoices();
    [[nodiscard]] static QVariantList motifChoices();
    [[nodiscard]] static QVariantList nameEffectChoices();
    [[nodiscard]] static QVariantList flourishChoices();
    [[nodiscard]] static QVariantList ambientChoices();
    [[nodiscard]] static QVariantList zodiacChoices();
    [[nodiscard]] static QVariantList moodChoices();
    [[nodiscard]] static QVariantList hereForChoices();
    [[nodiscard]] static QVariantMap limits();
    [[nodiscard]] QString notice() const { return m_notice; }

signals:
    void navigationChanged();
    void personChanged();
    void viewerChanged();
    void localIdentityChanged();
    void editingChanged();
    void draftDirtyChanged();
    void publishedChanged();
    void historyChanged();
    void tryOnChanged();
    void importChanged();
    void topFriendCandidatesChanged();
    void recentColorsChanged();
    void noticeChanged();
    // After a Save: "Saved. Your contacts will see it…" / "…when you're back online."
    void published(bool offline);

private:
    // Who a stack entry is, as the opener named them. Resolution turns this
    // into the person on screen, again on every roster or page change, so a
    // stub whose account becomes a contact turns into their page in place.
    struct PersonRef final {
        enum class Kind { Own, Account, Request, Handle };
        Kind kind = Kind::Own;
        QString accountHex;   // Account; Request when known
        QString requestId;    // Request
        QString label;        // the name the opener had (a request's, a call tile's, a Top Friend label)
        QString handle;       // Request / Handle: relay-confirmed
        QString avatarKey;    // a call tile's picture
        QString referrerName; // Friend Space: the page owner's first name
        Profile::Origin reason = Profile::Origin::FromChat; // why a stub exists
    };
    struct Entry final {
        PersonRef person;
        qreal scrollY = 0;
    };
    // Everything the person* properties report.
    struct PersonState final {
        QString id, accountHex;
        Profile::Relationship relationship = Profile::Relationship::StrangerPerson;
        QString name, firstName, handle;
        bool handlePending = false, blocked = false, verified = false;
        QString avatarKey;
        int presence = int(Presence::Offline);
        bool online = false;
        QString statusLine, requestId, referrerName;
        Profile::PageState pageState = Profile::PageState::StubPage;
        bool pageIncomplete = false, pageLoading = false;
        friend bool operator==(const PersonState &, const PersonState &) = default;
    };
    // Where a page object's blobs come from.
    struct MediaSource final {
        enum class Owner { None, Own, Contact };
        Owner owner = Owner::None;
        QString accountHex;
        friend bool operator==(const MediaSource &, const MediaSource &) = default;
    };
    // What a page object currently holds of its media, so a refresh reads a
    // blob only when something about it changed.
    struct HeldMedia final {
        MediaSource source;
        QByteArray backgroundHash;
        bool backgroundPresent = false;
        bool imageShown = false;
        QString imageKey;
        QByteArray songHash;
        bool songPresent = false;
    };
    // The file a song was cut from, kept for re-trimming (editor-only).
    struct SongSource final {
        QString path, fileName, formatLabel;
        qint64 durationMs = 0;
        QVector<quint8> peaks;
        qint64 windowStartMs = 0;
        QByteArray songHash; // the encoded window it produced
    };

    // Navigation
    void startStack(const PersonRef &person, Profile::Origin origin);
    void pushEntry(const PersonRef &person);
    void truncateTo(int index);
    void navigated();
    [[nodiscard]] Profile::Origin originFromSection(int origin) const;
    [[nodiscard]] static bool samePerson(const PersonRef &a, const PersonRef &b);
    [[nodiscard]] PersonRef personForAccount(const AccountId &account, const QString &label,
                                             const QString &avatarKey, Profile::Origin reason) const;

    // Resolution
    void refreshPerson();
    [[nodiscard]] PersonState resolveIdentity(const PersonRef &ref) const;
    void loadViewPage(PersonState &state);
    [[nodiscard]] std::optional<AccountId> accountFor(const QString &idOrHex) const;
    [[nodiscard]] QString rosterIdFor(const AccountId &account) const;
    [[nodiscard]] std::optional<Contact> rosterContact(const QString &rosterId) const;
    [[nodiscard]] std::optional<ContactRecord> contactRecord(const AccountId &account) const;
    [[nodiscard]] QVector<ContactRecord> contactRecords() const;
    [[nodiscard]] QString localAccountHex() const;
    [[nodiscard]] QString localName() const;
    [[nodiscard]] QVariantList tilesFor(const QVector<Profile::TopFriend> &friends) const;
    void requestHandle(const AccountId &account);
    void requestOwnHandle();
    void onAccountResolved(const AccountId &account, const QString &handle);
    void onAccountResolutionFailed(const AccountId &account);
    void onRosterChanged();
    void updateLinkOnline();
    void updateCandidates();

    // Pages and media
    [[nodiscard]] const Profile::Page &ownPublishedPage() const noexcept { return m_published; }
    void reloadPublished();
    [[nodiscard]] std::optional<Profile::Page> contactPageFor(const QString &accountHex) const;
    void updateMedia(ProfilePageObject &object, const MediaSource &source, bool force = false);
    [[nodiscard]] HeldMedia &heldMediaFor(const ProfilePageObject &object);
    [[nodiscard]] QByteArray blob(const MediaSource &source, const QByteArray &sha256, Profile::MediaKind kind) const;
    void releaseImage(const QString &key);
    void releaseSong(const QByteArray &sha256);
    void onImageReady(const QString &key);
    [[nodiscard]] std::optional<QByteArray> storeLocalMedia(Profile::MediaKind kind, const QByteArray &bytes);
    void applyViewer();

    // Editing
    void onDraftEdited();
    // A controller-made change (preset, media, friends, modules): always a
    // step of its own, never merged into a gesture that happens to be open.
    void editDraft(const Profile::Page &page);
    void closeGesture();
    void emitHistoryIfChanged(bool couldUndo, bool couldRedo);
    void loadDraft(const Profile::Page &page);
    void leaveEditing();
    void resetDraftToPublished();
    void updateDirty();
    void refreshTryOn();
    void scheduleAutosave();
    void flushAutosave();
    void saveDraftNow();
    [[nodiscard]] bool publishNow();
    void finishPendingPublish(bool importSucceeded);
    [[nodiscard]] bool importsBusy() const;
    // Refs of the draft whose blob no longer resolves (collected while only
    // the undo history named it) are dropped, and the owner is told.
    [[nodiscard]] bool dropUnresolvedDraftMedia();
    void applyHistoryPage(const Profile::Page &page);
    [[nodiscard]] QString songSourceJson() const;
    void restoreSongSource(const QString &json);
    void followDraftSong();
    void setNotice(const QString &notice);

    // Imports
    [[nodiscard]] ProfileBackgroundImporter &backgroundImporter();
    [[nodiscard]] SongImporter &songImporter();
    void onBackgroundImported(const QByteArray &jpeg, QSize size);
    void onBackgroundFailed(const QString &message);
    void onSongAnalysed(const SongSourceInfo &info);
    void onSongEncoded(const QByteArray &container, qint64 durationMs, qint64 windowStartMs);
    void onSongFailed(const QString &message);
    void startSongWindowEncode();
    // The file the song tab shows: one being imported once it is analysed,
    // else the draft song's own.
    [[nodiscard]] const SongSource &shownSongSource() const;

    [[nodiscard]] bool isLive() const noexcept { return m_live; }
    void dropSync();

    ChatController &m_chats;

    // Live services (null in mock mode).
    bool m_live = false;
    ProfileSession *m_session = nullptr;
    QPointer<SyncEngine> m_engine;
    QPointer<ContactRequestService> m_requests;
    QPointer<RelayClient> m_relay;
    std::unique_ptr<ProfilePageSync> m_sync;

    // The reference mock.
    QHash<QString, Profile::Page> m_mockPages; // by mock contact id
    QHash<QByteArray, QByteArray> m_mockMedia; // by SHA-256
    Profile::Page m_mockPublished;
    std::optional<Profile::Page> m_mockDraft;
    QString m_mockDraftSource;
    qint64 m_mockDraftAtMs = 0;
    bool m_mockLinkOnline = true;

    // Navigation and the person on screen.
    QVector<Entry> m_stack;
    Profile::Origin m_origin = Profile::Origin::FromChat;
    qreal m_entryScrollY = 0;
    PersonState m_person;
    quint64 m_navigation = 0;       // bumped by every move on the stack
    quint64 m_viewedNavigation = 0; // the move whose contact page was marked viewed
    QTimer m_refreshTimer;

    ProfilePageObject m_view;
    ProfilePageObject m_draft;
    ProfilePageObject m_tryOn;
    HeldMedia m_viewMedia, m_draftMedia, m_tryOnMedia;
    Profile::Page m_published; // the own page as last published (defaultPage() before)

    // Viewer
    bool m_darkMode = false;
    bool m_plainStyle = false;
    bool m_callActive = false;
    bool m_linkOnline = true;
    QString m_localHandle;

    // Editing
    bool m_editing = false;
    bool m_draftDirty = false;
    bool m_publishPending = false;
    qint64 m_survivingDraftAtMs = 0;
    int m_lastTab = 0;
    int m_tryOnPreset = -1;
    Profile::Page m_draftShadow; // the draft as last recorded: the next change's "before"
    bool m_applyingHistory = false;
    ProfileEditHistory m_history;
    QTimer m_autosaveTimer;

    // Imports
    std::unique_ptr<ProfileBackgroundImporter> m_backgroundImporter;
    bool m_backgroundImporting = false;
    qreal m_backgroundProgress = 0;
    std::unique_ptr<SongImporter> m_songImporter;
    bool m_songAnalysing = false;
    bool m_songEncoding = false;
    bool m_songIsNewImport = false; // the next encode also takes the file's title and artist
    SongSource m_songSource;        // the draft's song's file
    SongSource m_songImport;        // a file being imported, until its first encode lands
    QString m_songTitleTag, m_songArtistTag;
    QHash<QByteArray, SongSource> m_songSources; // by song hash, so undo brings the file back
    QTimer m_songWindowTimer;

    // Relay handle lookups, by account hex.
    QHash<QString, QString> m_resolvedHandles;
    QSet<QString> m_pendingLookups;
    QHash<QString, qint64> m_failedLookups;
    bool m_ownHandleRequested = false;

    QVariantList m_candidates;
    QStringList m_recentColors;
    QString m_notice;
    std::function<int(int)> m_pick; // uniform in [0, bound)
};

} // namespace OpenChat

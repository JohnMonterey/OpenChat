// ProfileController, its page objects and the editor's history, through the
// instance ChatController owns (ARCH §9.6): on the reference mock for
// navigation, identity, the page objects, editing, undo, imports and the
// catalogues, and over two real peers (tests/PageSyncTestSupport.h) for
// publishing, receiving, drafts across a restart, the relay's handles and
// call deferral.

#include "PageSyncTestSupport.h"
#include "ProfileQmlHarness.h"

#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "controllers/ChatController.h"
#include "controllers/ProfileController.h"
#include "controllers/ProfilePageObject.h"
#include "controllers/ProfileReferencePages.h"
#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "models/Contact.h"
#include "models/ContactListModel.h"
#include "network/RelayClient.h"
#include "profile/ProfileBackgroundImage.h"
#include "profile/ProfileFonts.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfileReadability.h"
#include "profile/SongImport.h"
#include "profile/SongPlayer.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QMetaProperty>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>

using namespace OpenChat;
using Profile::Origin;
using Profile::PageState;
using Profile::Relationship;

namespace {

namespace Reference = ProfileReferencePages;

// Requests an hour away and a quick pump: only the traffic a test causes.
[[nodiscard]] ProfileController::SyncTuning quietTuning()
{
    return {{}, [](qint64 bound) { return bound - 1; }, PageSyncTest::quietLimits()};
}

// Live tests run the controller's sync on the fixture's clock and random.
void useFixtureTime(PageSyncTest::TwoPeerFixture &fixture)
{
    ProfileController::setSyncTuningForTesting(
        ProfileController::SyncTuning{[&fixture] { return fixture.clock.nowMs; }, fixture.random, fixture.limits});
}

[[nodiscard]] QString mockHex(const QString &id)
{
    return Reference::mockAccountFor(id).toHex();
}

[[nodiscard]] QByteArray mockBytes(const QString &id)
{
    return Reference::mockAccountFor(id).bytes();
}

[[nodiscard]] QColor rgb(quint32 value)
{
    return ProfileReadability::rgb(value);
}

[[nodiscard]] QVariantMap tile(const ProfilePageObject &page, int index)
{
    return page.topFriends().value(index).toMap();
}

// A published own page (revision > 0), so its view is a CustomPage.
[[nodiscard]] Profile::Page publishedPage(const QString &headline)
{
    Profile::Page page = Profile::defaultPage();
    page.revision = 5;
    page.publishedAtMs = 5;
    page.content.headline = headline;
    return page;
}

// A small but real baseline JPEG, which the media store decodes.
[[nodiscard]] QByteArray realJpeg(const QColor &colour, const QSize &size = QSize(64, 48))
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y, colour.lighter(100 + (x + y) % 40));
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 85);
    return bytes;
}

// A picture file as the owner would pick one.
[[nodiscard]] QString writePicture(const QTemporaryDir &dir, const QString &name, const QSize &size)
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x)
            image.setPixel(x, y, qRgb((x * 255) / size.width(), (y * 255) / size.height(), 140));
    }
    const QString path = dir.filePath(name);
    return image.save(path, "PNG") ? path : QString();
}

void appendLe16(QByteArray &out, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void appendLe32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

void appendChunk(QByteArray &out, const char *id, const QByteArray &payload)
{
    out.append(id, 4);
    appendLe32(out, quint32(payload.size()));
    out.append(payload);
    if (payload.size() % 2 != 0)
        out.append('\0');
}

// A 48 kHz mono 16-bit WAV: `silence` seconds of nothing, then a tone whose
// level swells, so the waveform has a shape; tagged with a title and artist.
[[nodiscard]] QString writeSong(const QTemporaryDir &dir, const QString &name, double seconds, double silence)
{
    constexpr int rate = 48'000;
    const qsizetype frames = qsizetype(seconds * rate);
    QByteArray samples;
    samples.reserve(frames * 2);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const double t = double(frame) / rate;
        const double level = t < silence ? 0.0 : 0.35 * (0.6 + 0.4 * std::sin(2 * std::numbers::pi * 0.5 * t));
        appendLe16(samples, quint16(qint16(std::lround(level * std::sin(2 * std::numbers::pi * 440 * t) * 32767))));
    }
    QByteArray format;
    appendLe16(format, 1); // PCM
    appendLe16(format, 1);
    appendLe32(format, rate);
    appendLe32(format, rate * 2);
    appendLe16(format, 2);
    appendLe16(format, 16);
    QByteArray info("INFO");
    appendChunk(info, "INAM", QByteArray("Paper Planes") + '\0');
    appendChunk(info, "IART", QByteArray("M.I.A.") + '\0');
    QByteArray body("WAVE");
    appendChunk(body, "fmt ", format);
    appendChunk(body, "LIST", info);
    appendChunk(body, "data", samples);
    QByteArray file("RIFF");
    appendLe32(file, quint32(body.size()));
    file.append(body);

    const QString path = dir.filePath(name);
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(file) != file.size())
        return {};
    return path;
}

// Profile pages over a real peer, wired as the app wires them: the peer's
// session and engine, its request service, and the chat surface that owns the
// profile controller. Declared after the fixture, so it goes first.
struct LiveChat final {
    explicit LiveChat(PageSyncTest::Peer &peer)
        : requests(*peer.session, peer.engine())
    {
        chat.setLiveServices(peer.session.get(), &peer.engine(), &requests);
    }
    [[nodiscard]] ProfileController &profiles() { return *chat.profiles(); }

    ContactRequestService requests;
    ChatController chat;
};

// A contact row as the roster would show a person.
[[nodiscard]] Contact person(const QString &id, const QString &name)
{
    Contact contact;
    contact.id = id;
    contact.name = name;
    contact.presence = Presence::Available;
    contact.avatarKey = QStringLiteral("userpfp_none");
    return contact;
}

// Records `handle` on `owner` as someone who is not (or not yet) a contact.
[[nodiscard]] std::optional<AccountId> addRequestRow(PageSyncTest::Peer &owner, const QString &handle,
                                                    ContactState state, qint64 nowMs,
                                                    ConversationId conversation = ConversationId::generate())
{
    const AccountId account = AccountId::generate();
    ContactRecord record{account, handle, QString(), ContactState::PendingOutgoing, conversation, nowMs, nowMs};
    record.peerDeviceId = DeviceId::generate();
    bool stored = false;
    switch (state) {
    case ContactState::PendingOutgoing:
        stored = owner.contacts().recordOutgoingRequest(record).hasValue();
        break;
    case ContactState::PendingIncoming:
        record.state = ContactState::PendingIncoming;
        stored = owner.contacts().recordIncomingRequest(record).hasValue();
        break;
    case ContactState::Blocked:
        record.state = ContactState::PendingIncoming;
        stored = owner.contacts().recordIncomingRequest(record).hasValue()
                 && owner.contacts().block(account, nowMs).hasValue();
        break;
    case ContactState::Accepted:
        break;
    }
    return stored ? std::optional<AccountId>(account) : std::nullopt;
}

} // namespace

class ProfileControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        ProfileQmlHarness::resetProfileSingletons();
        QSettings().clear();
        ProfileController::setSyncTuningForTesting(quietTuning());
    }

    void cleanup()
    {
        // A live test's tuning captured its fixture, which is gone now.
        ProfileController::setSyncTuningForTesting(quietTuning());
    }

    // --- Fonts (first: the suite starts with the faces unregistered) -------

    void fontsRegisterBeforeTheFirstOpen()
    {
        if (ProfileFonts::isRegistered())
            QSKIP("An earlier test in this run already opened a profile page.");
        ChatController chat;
        ProfileController &profiles = *chat.profiles();
        // Building the controller and its page objects loads no face.
        QVERIFY(!ProfileFonts::isRegistered());

        // By the time `open` turns true (and QML creates the page), the faces
        // the page names are in the font database.
        bool registeredWhenOpened = false;
        connect(&profiles, &ProfileController::navigationChanged, this, [&] {
            if (profiles.isOpen())
                registeredWhenOpened = ProfileFonts::isRegistered();
        });
        QVERIFY(profiles.openContact(QStringLiteral("jessica")));
        QVERIFY(registeredWhenOpened);
        const QStringList families = QFontDatabase::families();
        for (const QString &family : ProfileFonts::bundledFamilies())
            QVERIFY2(families.contains(family), qPrintable(family));
        // The faces Scene Queen's page names ("" is the interface font) are there.
        const ProfileRenderStyle &render = *profiles.view()->render();
        int bundled = 0;
        for (const QString &family : {render.headingFamily(), render.bodyFamily(), render.nameFamily()}) {
            if (family.isEmpty())
                continue;
            ++bundled;
            QVERIFY2(families.contains(family), qPrintable(family));
        }
        QVERIFY(bundled > 0);
    }

    // --- Opening and resolving -------------------------------------------------

    void opensAContactFromTheRoster()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(!p.isOpen());
        QSignalSpy navigation(&p, &ProfileController::navigationChanged);
        QSignalSpy person(&p, &ProfileController::personChanged);

        QVERIFY(p.openContact(QStringLiteral("michael")));
        QVERIFY(p.isOpen());
        QCOMPARE(p.depth(), 1);
        QCOMPARE(p.origin(), int(Origin::FromChat));
        QCOMPARE(p.backLabel(), QStringLiteral("Chat"));
        QCOMPARE(navigation.count(), 1);
        QCOMPARE(person.count(), 1);
        QCOMPARE(p.personId(), QStringLiteral("michael"));
        QCOMPARE(p.personAccountId(), mockHex(QStringLiteral("michael")));
        QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
        QVERIFY(!p.isOwnProfile());
        QCOMPARE(p.personName(), QStringLiteral("Michael"));
        QCOMPARE(p.personFirstName(), QStringLiteral("Michael"));
        QCOMPARE(p.personHandle(), QStringLiteral("michael"));
        QVERIFY(!p.personHandlePending());
        QVERIFY(!p.personBlocked());
        QVERIFY(!p.personVerified());
        QCOMPARE(p.personAvatarKey(), QStringLiteral("michael"));
        QCOMPARE(p.personPresence(), int(Presence::Available));
        QVERIFY(p.personOnline());
        QCOMPARE(p.requestId(), QString());
        QCOMPARE(p.referrerName(), QString());
        QCOMPARE(p.pageState(), int(PageState::CustomPage));
        QVERIFY(!p.pageIncomplete());
        QVERIFY(!p.pageLoading());
        // The page on screen is theirs, read-only, and only what has content.
        ProfilePageObject &view = *p.view();
        QVERIFY(view.readOnly());
        QVERIFY(!view.showEmptyModules());
        QCOMPARE(view.headline(), QStringLiteral("Shoot film. Drink coffee. Repeat."));
        QCOMPARE(view.preset(), int(Profile::Preset::AeroSkyPreset));

        // Presence is the roster's: Sarah is away, so not "Online Now!".
        chat.setNavSection(ChatController::NavSection::Settings);
        QVERIFY(p.openContact(QStringLiteral("sarah")));
        QCOMPARE(p.depth(), 1);
        QCOMPARE(p.origin(), int(Origin::FromSettings));
        QCOMPARE(p.backLabel(), QStringLiteral("Settings"));
        QCOMPARE(p.personPresence(), int(Presence::Away));
        QVERIFY(!p.personOnline());
        // Tom never published a page: the default page, not a stub.
        QVERIFY(p.openContact(QStringLiteral("tom"), int(Origin::FromSearch)));
        QCOMPARE(p.origin(), int(Origin::FromSearch));
        QCOMPARE(p.pageState(), int(PageState::DefaultPage));
        QCOMPARE(p.personPresence(), int(Presence::Offline));

        // Nobody the roster knows: nothing opens, nothing changes.
        QVERIFY(!p.openContact(QStringLiteral("nobody-at-all")));
        QVERIFY(!p.openContact(QString()));
        QCOMPARE(p.personId(), QStringLiteral("tom"));
        // A roster id and an account both name a person, and your own id is
        // your own profile.
        p.openPerson(mockHex(QStringLiteral("ryan")), QStringLiteral("what the call said"), QString());
        QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
        QCOMPARE(p.personName(), QStringLiteral("Ryan"));
        p.openPerson(QStringLiteral("alex"), QString(), QString());
        QCOMPARE(p.personId(), QStringLiteral("alex"));
        QVERIFY(p.openContact(p.localAccountId()));
        QVERIFY(p.isOwnProfile());
    }

    void ignoresGroupChats()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        chat.addToGroup(QStringLiteral("sarah")); // Michael + Sarah: a mock group
        const QString group = chat.currentContactId();
        QVERIFY(group.startsWith(QStringLiteral("group:")));
        QVERIFY(chat.contacts()->contactById(group).has_value());

        QVERIFY(!p.openContact(group));
        QVERIFY(!p.isOpen());
        // A group is never a Top Friend either.
        p.openOwn();
        QVERIFY(p.beginEditing());
        for (const QVariant &candidate : p.topFriendCandidates())
            QVERIFY(candidate.toMap().value(QStringLiteral("contactId")).toString() != group);
        QVERIFY(!p.addTopFriend(group));
        QVERIFY(p.draft()->page().topFriends.isEmpty());
    }

    void opensOwnProfileWithLocalIdentity()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        chat.setLocalUserName(QStringLiteral("Daniel Reyes"));
        chat.setLocalStatusText(QStringLiteral("Recording all week"));
        chat.setLocalPresence(int(Presence::Busy));

        chat.setNavSection(ChatController::NavSection::Call);
        p.openOwn();
        QCOMPARE(p.origin(), int(Origin::FromCall));
        QCOMPARE(p.relationship(), int(Relationship::SelfPerson));
        QVERIFY(p.isOwnProfile());
        QCOMPARE(p.personId(), Reference::selfId());
        QCOMPARE(p.personAccountId(), p.localAccountId());
        QCOMPARE(p.localAccountId(), mockHex(Reference::selfId()));
        QCOMPARE(p.personName(), QStringLiteral("Daniel Reyes"));
        QCOMPARE(p.personFirstName(), QStringLiteral("Daniel"));
        QCOMPARE(p.personAvatarKey(), chat.localAvatarKey());
        QCOMPARE(p.personPresence(), int(Presence::Busy));
        QVERIFY(!p.personOnline());
        QCOMPARE(p.personStatusLine(), QStringLiteral("Recording all week"));
        // Never saved: the default page.
        QCOMPARE(p.pageState(), int(PageState::DefaultPage));
        QCOMPARE(p.publishedRevision(), 0);

        // Identity changes elsewhere show at once.
        QSignalSpy person(&p, &ProfileController::personChanged);
        chat.setLocalPresence(int(Presence::Available));
        QCOMPARE(p.personPresence(), int(Presence::Available));
        QVERIFY(p.personOnline());
        QVERIFY(person.count() >= 1);

        // Once published, the page on screen is the owner's own.
        p.setMockPage(Reference::selfId(), Reference::ownReferencePage());
        QCOMPARE(p.pageState(), int(PageState::CustomPage));
        QCOMPARE(p.view()->headline(), QStringLiteral("Music is the answer."));
        QCOMPARE(p.view()->preset(), int(Profile::Preset::HeadlinerPreset));
    }

    void mockIdentityResolvesContactsSelfAndStrangers_data()
    {
        QTest::addColumn<int>("index");
        QTest::addColumn<int>("relationship");
        QTest::addColumn<QString>("personId");
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("pageState");
        QTest::addColumn<QString>("referrer");
        QTest::addColumn<QString>("avatarKey");

        QTest::newRow("contact") << 0 << int(Relationship::ContactPerson) << QStringLiteral("jessica")
                                 << QStringLiteral("Jessica") << int(PageState::CustomPage) << QString()
                                 << QStringLiteral("jessica");
        QTest::newRow("self") << 1 << int(Relationship::SelfPerson) << Reference::selfId() << QStringLiteral("Daniel")
                              << int(PageState::DefaultPage) << QString() << QStringLiteral("userpfp_none");
        QTest::newRow("stranger") << 2 << int(Relationship::StrangerPerson) << QStringLiteral("dana-whitfield")
                                  << QStringLiteral("Dana Whitfield") << int(PageState::StubPage)
                                  << QStringLiteral("Michael") << QStringLiteral("userpfp_none");
    }

    void mockIdentityResolvesContactsSelfAndStrangers()
    {
        QFETCH(int, index);
        QFETCH(int, relationship);
        QFETCH(QString, personId);
        QFETCH(QString, name);
        QFETCH(int, pageState);
        QFETCH(QString, referrer);
        QFETCH(QString, avatarKey);

        ChatController chat;
        chat.setLocalUserName(QStringLiteral("Daniel"));
        ProfileController &p = *chat.profiles();
        Profile::Page michael = *Reference::seededPage(QStringLiteral("michael"));
        // The owner's labels; the viewer's own names win for people they know.
        michael.topFriends = {{mockBytes(QStringLiteral("jessica")), QStringLiteral("Jess")},
                              {mockBytes(Reference::selfId()), QStringLiteral("Danny")},
                              {mockBytes(QStringLiteral("dana-whitfield")), QStringLiteral("Dana Whitfield")}};
        p.setMockPage(QStringLiteral("michael"), michael);
        QVERIFY(p.openContact(QStringLiteral("michael")));

        QVERIFY(p.openTopFriend(index));
        QCOMPARE(p.depth(), 2);
        QCOMPARE(p.relationship(), relationship);
        QCOMPARE(p.personId(), personId);
        QCOMPARE(p.personAccountId(), mockHex(personId));
        QCOMPARE(p.personName(), name);
        QCOMPARE(p.pageState(), pageState);
        QCOMPARE(p.referrerName(), referrer);
        QCOMPARE(p.personAvatarKey(), avatarKey);
        QCOMPARE(p.backLabel(), QStringLiteral("Michael"));
        if (relationship == int(Relationship::StrangerPerson)) {
            // A stub carries no page data and no presence, and its handle
            // could only come from the relay (none here).
            QCOMPARE(p.personHandle(), QString());
            QVERIFY(!p.personHandlePending());
            QCOMPARE(p.personPresence(), int(Presence::Offline));
            QVERIFY(!p.personOnline());
            QVERIFY(p.view()->topFriends().isEmpty());
            QVERIFY(p.view()->render()->plain());
        }
    }

    void openHandleFindsRosterAndRequestRows()
    {
        {
            // Mock: a roster handle opens that contact, from Search.
            ChatController chat;
            ProfileController &p = *chat.profiles();
            p.openHandle(QStringLiteral("  @Michael "));
            QCOMPARE(p.origin(), int(Origin::FromSearch));
            QCOMPARE(p.backLabel(), QStringLiteral("Search"));
            QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
            QCOMPARE(p.personId(), QStringLiteral("michael"));
            QCOMPARE(p.pageState(), int(PageState::CustomPage));
            // Anyone else found by handle: a handle-only stub.
            p.openHandle(QStringLiteral("zed.q"));
            QCOMPARE(p.relationship(), int(Relationship::StrangerPerson));
            QCOMPARE(p.personId(), QString());
            QCOMPARE(p.personAccountId(), QString());
            QCOMPARE(p.personName(), QStringLiteral("zed.q"));
            QCOMPARE(p.personHandle(), QStringLiteral("zed.q"));
            QCOMPARE(p.pageState(), int(PageState::StubPage));
            // Not a handle at all: nothing happens.
            p.openHandle(QStringLiteral("not a handle!"));
            QCOMPARE(p.personHandle(), QStringLiteral("zed.q"));
        }

        // Live: the roster's rows by handle, whatever their state.
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        PageSyncTest::Peer &alice = fixture.a();
        const qint64 now = fixture.clock.nowMs;
        const ConversationId graceConversation = ConversationId::generate();
        const auto grace =
            addRequestRow(alice, QStringLiteral("grace"), ContactState::PendingIncoming, now, graceConversation);
        const auto olivia = addRequestRow(alice, QStringLiteral("olivia"), ContactState::PendingOutgoing, now);
        const auto mallory = addRequestRow(alice, QStringLiteral("mallory"), ContactState::Blocked, now);
        QVERIFY(grace && olivia && mallory);
        QVERIFY(alice.session->setHandle(QStringLiteral("alice")).hasValue());
        useFixtureTime(fixture);
        LiveChat live(alice);
        ProfileController &p = live.profiles();

        p.openHandle(QStringLiteral("bob"));
        QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
        QCOMPARE(p.personId(), fixture.b().account.toHex());
        QCOMPARE(p.personHandle(), QStringLiteral("bob"));

        // An incoming request found in Search & Find offers Accept/Decline.
        p.openHandle(QStringLiteral("grace"));
        QCOMPARE(p.relationship(), int(Relationship::IncomingRequestPerson));
        QCOMPARE(p.requestId(), graceConversation.toHex());
        QCOMPARE(p.personAccountId(), grace->toHex());
        QCOMPARE(p.personName(), QStringLiteral("grace"));
        QCOMPARE(p.pageState(), int(PageState::StubPage));

        p.openHandle(QStringLiteral("olivia"));
        QCOMPARE(p.relationship(), int(Relationship::OutgoingRequestPerson));
        QCOMPARE(p.personAccountId(), olivia->toHex());

        p.openHandle(QStringLiteral("mallory"));
        QCOMPARE(p.relationship(), int(Relationship::StrangerPerson));
        QVERIFY(p.personBlocked());

        // Your own handle is your own profile.
        p.openHandle(QStringLiteral("@alice"));
        QVERIFY(p.isOwnProfile());
        QCOMPARE(p.localHandle(), QStringLiteral("alice"));
    }

    void requestAndStrangerOpenStubs()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();

        p.openRequest(QStringLiteral("req-1"), mockHex(QStringLiteral("grace-liu")), QStringLiteral("Grace Liu"),
                      QStringLiteral("@grace.liu"));
        QCOMPARE(p.origin(), int(Origin::FromRequests));
        QCOMPARE(p.backLabel(), QStringLiteral("Requests"));
        QCOMPARE(p.relationship(), int(Relationship::IncomingRequestPerson));
        QCOMPARE(p.requestId(), QStringLiteral("req-1"));
        QCOMPARE(p.personName(), QStringLiteral("Grace Liu"));
        QCOMPARE(p.personFirstName(), QStringLiteral("Grace"));
        QCOMPARE(p.personHandle(), QStringLiteral("grace.liu"));
        QCOMPARE(p.personAvatarKey(), QStringLiteral("userpfp_none"));
        QCOMPARE(p.pageState(), int(PageState::StubPage));
        QCOMPARE(p.personPresence(), int(Presence::Offline));
        QVERIFY(!p.personOnline());
        QCOMPARE(p.personStatusLine(), QString());
        // A stub shows no page: the view holds the default page, plain.
        QCOMPARE(p.view()->headline(), QString());
        QVERIFY(p.view()->render()->plain());

        // A request whose account is not known here yet still opens its stub.
        p.openRequest(QStringLiteral("req-2"), QString(), QStringLiteral("Someone"), QString());
        QCOMPARE(p.relationship(), int(Relationship::IncomingRequestPerson));
        QCOMPARE(p.requestId(), QStringLiteral("req-2"));
        QCOMPARE(p.personName(), QStringLiteral("Someone"));
        QCOMPARE(p.personAccountId(), QString());

        // A call participant who is not a contact keeps the picture the call
        // already shows, and a stub.
        p.openPerson(mockHex(QStringLiteral("kenji-mori")), QStringLiteral("Kenji Mori"), QStringLiteral("callpic"));
        QCOMPARE(p.origin(), int(Origin::FromCall));
        QCOMPARE(p.backLabel(), QStringLiteral("Call"));
        QCOMPARE(p.relationship(), int(Relationship::StrangerPerson));
        QCOMPARE(p.personName(), QStringLiteral("Kenji Mori"));
        QCOMPARE(p.personAvatarKey(), QStringLiteral("callpic"));
        QCOMPARE(p.pageState(), int(PageState::StubPage));
        QCOMPARE(p.referrerName(), QString());
        QCOMPARE(p.personHandle(), QString());
    }

    // --- The back stack ----------------------------------------------------------

    void backStackCapsAtTenAndTruncatesLoops()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        // Twelve contacts, each with the next one as their Top Friend; the
        // last names the fifth.
        const QStringList names{QStringLiteral("Ada"), QStringLiteral("Ben"), QStringLiteral("Cy"),
                                QStringLiteral("Di"),  QStringLiteral("Eve"), QStringLiteral("Fox"),
                                QStringLiteral("Gus"), QStringLiteral("Hal"), QStringLiteral("Ivy"),
                                QStringLiteral("Jo"),  QStringLiteral("Kai"), QStringLiteral("Lu")};
        QVector<Contact> rows;
        for (int i = 0; i < names.size(); ++i)
            rows.append(person(QStringLiteral("c%1").arg(i), names.at(i)));
        chat.contacts()->setContacts(rows);
        for (int i = 0; i < names.size(); ++i) {
            Profile::Page page = publishedPage(names.at(i) + QStringLiteral("'s page"));
            const int next = i + 1 < names.size() ? i + 1 : 4;
            page.topFriends = {{mockBytes(QStringLiteral("c%1").arg(next)), names.at(next)}};
            p.setMockPage(QStringLiteral("c%1").arg(i), page);
        }

        QVERIFY(p.openContact(QStringLiteral("c0")));
        for (int step = 1; step < names.size(); ++step) {
            QVERIFY(p.openTopFriend(0));
            QCOMPARE(p.personId(), QStringLiteral("c%1").arg(step));
            QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
        }
        // Twelve visits, ten entries: the two oldest went.
        QCOMPARE(p.depth(), ProfileController::maxStackDepth);
        QCOMPARE(p.depth(), 10);
        const QVariantList history = p.history();
        QCOMPARE(history.size(), 9);
        QCOMPARE(history.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Kai"));
        QCOMPARE(history.last().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Cy"));
        // The origin survives the oldest entries being dropped.
        QCOMPARE(p.origin(), int(Origin::FromChat));

        // Lu names Eve, who is on the stack: back to her, no loop.
        QVERIFY(p.openTopFriend(0));
        QCOMPARE(p.personId(), QStringLiteral("c4"));
        QCOMPARE(p.depth(), 3); // Cy, Di, Eve
        QCOMPARE(p.backLabel(), QStringLiteral("Di"));

        // Clicking the page you are on does nothing.
        Profile::Page self = publishedPage(QStringLiteral("Eve's page"));
        self.topFriends = {{mockBytes(QStringLiteral("c4")), QStringLiteral("Me")}};
        p.setMockPage(QStringLiteral("c4"), self);
        QSignalSpy navigation(&p, &ProfileController::navigationChanged);
        QVERIFY(!p.openTopFriend(0));
        QVERIFY(!p.openTopFriend(3));
        QVERIFY(!p.openTopFriend(-1));
        QCOMPARE(p.depth(), 3);
        QCOMPARE(navigation.count(), 0);
    }

    void backLabelNamesTheDestination()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.openContact(QStringLiteral("michael"), int(Origin::FromChat)));
        QCOMPARE(p.backLabel(), QStringLiteral("Chat"));
        QVERIFY(p.openTopFriend(0)); // Jessica
        QCOMPARE(p.personId(), QStringLiteral("jessica"));
        QCOMPARE(p.backLabel(), QStringLiteral("Michael"));
        QVERIFY(p.openTopFriend(1)); // Jessica's Alex
        QCOMPARE(p.personId(), QStringLiteral("alex"));
        QCOMPARE(p.backLabel(), QStringLiteral("Jessica"));
        p.back();
        QCOMPARE(p.backLabel(), QStringLiteral("Michael"));
        p.back();
        QCOMPARE(p.backLabel(), QStringLiteral("Chat"));
        p.back();
        QVERIFY(!p.isOpen());

        p.openOwn(int(Origin::FromCall));
        QCOMPARE(p.backLabel(), QStringLiteral("Call"));
        p.openContact(QStringLiteral("sarah"), int(Origin::FromSettings));
        QCOMPARE(p.backLabel(), QStringLiteral("Settings"));
        p.openHandle(QStringLiteral("somebody"));
        QCOMPARE(p.backLabel(), QStringLiteral("Search"));
        p.openRequest(QStringLiteral("r"), QString(), QStringLiteral("R"), QString());
        QCOMPARE(p.backLabel(), QStringLiteral("Requests"));

        // In the editor, Back leads to the profile.
        p.openOwn(int(Origin::FromChat));
        QSignalSpy navigation(&p, &ProfileController::navigationChanged);
        QVERIFY(p.beginEditing());
        QCOMPARE(p.backLabel(), QStringLiteral("Profile"));
        QVERIFY(navigation.count() >= 1);
        p.back();
        QVERIFY(!p.editing());
        QVERIFY(p.isOpen());
        QCOMPARE(p.backLabel(), QStringLiteral("Chat"));
    }

    void historyPopTo()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.openContact(QStringLiteral("michael")));
        QVERIFY(p.history().isEmpty());
        QVERIFY(p.openTopFriend(0)); // Jessica
        QVERIFY(p.openTopFriend(1)); // Alex
        // Newest first; the page on screen is not a destination.
        const QVariantList history = p.history();
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toMap(), (QVariantMap{{QStringLiteral("index"), 1},
                                                     {QStringLiteral("name"), QStringLiteral("Jessica")},
                                                     {QStringLiteral("avatarKey"), QStringLiteral("jessica")}}));
        QCOMPARE(history.at(1).toMap(), (QVariantMap{{QStringLiteral("index"), 0},
                                                     {QStringLiteral("name"), QStringLiteral("Michael")},
                                                     {QStringLiteral("avatarKey"), QStringLiteral("michael")}}));

        QSignalSpy navigation(&p, &ProfileController::navigationChanged);
        p.popTo(0);
        QCOMPARE(p.depth(), 1);
        QCOMPARE(p.personId(), QStringLiteral("michael"));
        QVERIFY(p.history().isEmpty());
        QCOMPARE(navigation.count(), 1);
        // The page on screen and anything out of range stay put.
        p.popTo(0);
        p.popTo(7);
        p.popTo(-1);
        QCOMPARE(p.depth(), 1);
        QCOMPARE(navigation.count(), 1);

        // Choosing a row from the editor leaves the editor.
        Profile::Page michael = *Reference::seededPage(QStringLiteral("michael"));
        michael.topFriends.prepend({mockBytes(Reference::selfId()), QStringLiteral("Dan")});
        p.setMockPage(QStringLiteral("michael"), michael);
        QVERIFY(p.openTopFriend(0));
        QVERIFY(p.isOwnProfile());
        QVERIFY(p.beginEditing());
        p.popTo(0);
        QVERIFY(!p.editing());
        QCOMPARE(p.personId(), QStringLiteral("michael"));
    }

    void entryScrollRestoresOnPopOnly()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.openContact(QStringLiteral("michael")));
        QCOMPARE(p.entryScrollY(), 0.0);
        QSignalSpy navigation(&p, &ProfileController::navigationChanged);
        // Reports are remembered, not echoed back: the view already shows them.
        p.setEntryScrollY(420);
        QCOMPARE(p.entryScrollY(), 0.0);
        QCOMPARE(navigation.count(), 0);

        QVERIFY(p.openTopFriend(0)); // a push starts at the top
        QCOMPARE(p.entryScrollY(), 0.0);
        p.setEntryScrollY(90);
        QVERIFY(p.openTopFriend(1));
        QCOMPARE(p.entryScrollY(), 0.0);

        p.back();
        QCOMPARE(p.entryScrollY(), 90.0);
        p.back();
        QCOMPARE(p.entryScrollY(), 420.0);
        // A person visited again is a new entry: the top again.
        QVERIFY(p.openTopFriend(0));
        QCOMPARE(p.entryScrollY(), 0.0);
        p.popTo(0);
        QCOMPARE(p.entryScrollY(), 420.0);
        // A new stack from outside starts at the top.
        QVERIFY(p.openContact(QStringLiteral("michael")));
        QCOMPARE(p.entryScrollY(), 0.0);
    }

    void topFriendTilesUseTheViewersNamesForContacts()
    {
        ChatController chat;
        chat.setLocalUserName(QStringLiteral("Daniel"));
        ProfileController &p = *chat.profiles();
        Profile::Page michael = *Reference::seededPage(QStringLiteral("michael"));
        michael.topFriends = {{mockBytes(QStringLiteral("jessica")), QStringLiteral("Jess the Mess")},
                              {mockBytes(Reference::selfId()), QStringLiteral("My Bassist")},
                              {mockBytes(QStringLiteral("dana-whitfield")), QStringLiteral("Dana Whitfield")},
                              {mockBytes(QStringLiteral("tom")), QStringLiteral("T")}};
        p.setMockPage(QStringLiteral("michael"), michael);
        QVERIFY(p.openContact(QStringLiteral("michael")));
        const ProfilePageObject &view = *p.view();
        QCOMPARE(view.topFriends().size(), 4);

        // A contact: the viewer's name and picture for them, never the label.
        QCOMPARE(tile(view, 0), (QVariantMap{{QStringLiteral("index"), 0},
                                             {QStringLiteral("accountId"), mockHex(QStringLiteral("jessica"))},
                                             {QStringLiteral("name"), QStringLiteral("Jessica")},
                                             {QStringLiteral("initials"), QStringLiteral("J")},
                                             {QStringLiteral("avatarKey"), QStringLiteral("jessica")},
                                             {QStringLiteral("isContact"), true},
                                             {QStringLiteral("isSelf"), false}}));
        // The viewer themself: their own name and picture.
        QCOMPARE(tile(view, 1).value(QStringLiteral("name")).toString(), QStringLiteral("Daniel"));
        QCOMPARE(tile(view, 1).value(QStringLiteral("avatarKey")).toString(), chat.localAvatarKey());
        QCOMPARE(tile(view, 1).value(QStringLiteral("isSelf")).toBool(), true);
        QCOMPARE(tile(view, 1).value(QStringLiteral("isContact")).toBool(), false);
        // Anyone else: the owner's label, over a monogram only.
        QCOMPARE(tile(view, 2), (QVariantMap{{QStringLiteral("index"), 2},
                                             {QStringLiteral("accountId"), mockHex(QStringLiteral("dana-whitfield"))},
                                             {QStringLiteral("name"), QStringLiteral("Dana Whitfield")},
                                             {QStringLiteral("initials"), QStringLiteral("DW")},
                                             {QStringLiteral("avatarKey"), QString()},
                                             {QStringLiteral("isContact"), false},
                                             {QStringLiteral("isSelf"), false}}));
        QCOMPARE(tile(view, 3).value(QStringLiteral("name")).toString(), QStringLiteral("Tom"));
        QCOMPARE(tile(view, 3).value(QStringLiteral("avatarKey")).toString(), QStringLiteral("mono"));

        // The roster renaming a contact renames their tile.
        QSignalSpy lists(p.view(), &ProfilePageObject::listsChanged);
        QVector<Contact> rows;
        for (int row = 0; row < chat.contacts()->rowCount(); ++row) {
            Contact contact = *chat.contacts()->contactAt(row);
            if (contact.id == QStringLiteral("jessica"))
                contact.name = QStringLiteral("Jessica M.");
            rows.append(contact);
        }
        chat.contacts()->setContacts(rows);
        QCOMPARE(tile(view, 0).value(QStringLiteral("name")).toString(), QStringLiteral("Jessica M."));
        QCOMPARE(tile(view, 0).value(QStringLiteral("initials")).toString(), QStringLiteral("JM"));
        QCOMPARE(lists.count(), 1);
    }

    void stubNameIsTheRelayHandleOnceKnown()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        // Directory endpoints left unset: every lookup the controller starts
        // is refused on the spot, which tells the test that it asked.
        RelayClient relay(DeviceId::generate(), AccountId::generate(), RelayEndpoints{}, RelayCredentials{});
        int asked = 0;
        connect(&relay, &RelayClient::transportError, this, [&asked] { ++asked; });
        p.setRelay(&relay);
        QCOMPARE(asked, 0); // mock mode knows its own handle story

        QVERIFY(p.openContact(QStringLiteral("michael")));
        QCOMPARE(asked, 0); // a contact's handle is the roster's
        QVERIFY(p.openTopFriend(4)); // Dana Whitfield, not a contact
        QCOMPARE(p.relationship(), int(Relationship::StrangerPerson));
        QCOMPARE(asked, 1);
        QCOMPARE(p.personName(), QStringLiteral("Dana Whitfield"));
        QCOMPARE(p.personHandle(), QString());
        QVERIFY(p.personHandlePending());
        p.refresh();
        QCOMPARE(asked, 1); // one lookup at a time

        QSignalSpy person(&p, &ProfileController::personChanged);
        relay.accountResolved(Reference::mockAccountFor(QStringLiteral("dana-whitfield")), QStringLiteral("Dana.W"));
        QCOMPARE(p.personHandle(), QStringLiteral("dana.w"));
        QCOMPARE(p.personName(), QStringLiteral("dana.w"));
        QCOMPARE(p.personFirstName(), QStringLiteral("dana.w"));
        QVERIFY(!p.personHandlePending());
        QCOMPARE(person.count(), 1);
        // Someone else's answer changes nothing here.
        relay.accountResolved(AccountId::generate(), QStringLiteral("someone"));
        QCOMPARE(p.personHandle(), QStringLiteral("dana.w"));
        QCOMPARE(person.count(), 1);

        // A failed lookup keeps the label, and is not retried at once.
        p.back();
        QVERIFY(p.openTopFriend(5)); // Priya Nair
        QCOMPARE(asked, 2);
        relay.accountResolutionFailed(Reference::mockAccountFor(QStringLiteral("priya-nair")),
                                      RelayDirectoryError::NotFound);
        QVERIFY(!p.personHandlePending());
        QCOMPARE(p.personName(), QStringLiteral("Priya Nair"));
        QCOMPARE(p.personHandle(), QString());
        p.refresh();
        QCOMPARE(asked, 2);
    }

    void editingIsAPropertyOfTheTopEntry()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(!p.beginEditing()); // nothing open
        QVERIFY(p.openContact(QStringLiteral("michael")));
        QVERIFY(!p.beginEditing()); // not yours
        QVERIFY(!p.editing());

        Profile::Page own = publishedPage(QStringLiteral("Mine"));
        own.topFriends = {{mockBytes(QStringLiteral("jessica")), QStringLiteral("Jessica")}};
        p.setMockPage(Reference::selfId(), own);
        p.openOwn();
        QSignalSpy editing(&p, &ProfileController::editingChanged);
        QVERIFY(p.beginEditing());
        QVERIFY(p.editing());
        QCOMPARE(editing.count(), 1);
        QVERIFY(p.beginEditing()); // already
        QCOMPARE(editing.count(), 1);
        // Friend links are inert while editing.
        QVERIFY(!p.openTopFriend(0));
        QCOMPARE(p.depth(), 1);
        QVERIFY(p.isOwnProfile());
        // Back leaves the editor for the page, which stays open.
        p.back();
        QVERIFY(!p.editing());
        QVERIFY(p.isOpen());
        QVERIFY(p.isOwnProfile());
        // Outside the editor the same link works, and the pushed entry is
        // not editable.
        QVERIFY(p.openTopFriend(0));
        QCOMPARE(p.personId(), QStringLiteral("jessica"));
        QVERIFY(!p.beginEditing());
        p.back();
        // A new stack from outside ends editing; so does closing.
        QVERIFY(p.beginEditing());
        QVERIFY(p.openContact(QStringLiteral("sarah")));
        QVERIFY(!p.editing());
        p.openOwn();
        QVERIFY(p.beginEditing());
        p.closeAll();
        QVERIFY(!p.editing());
        QVERIFY(!p.isOpen());
    }

    // --- Page objects ------------------------------------------------------------

    void settersDoNotEmitWhenUnchanged()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        QVERIFY(!d.readOnly());
        QVERIFY(d.showEmptyModules());
        QSignalSpy style(&d, &ProfilePageObject::styleChanged);
        QSignalSpy content(&d, &ProfilePageObject::contentChanged);
        QSignalSpy lists(&d, &ProfilePageObject::listsChanged);
        QSignalSpy media(&d, &ProfilePageObject::mediaChanged);
        QSignalSpy edited(&d, &ProfilePageObject::edited);

        d.setHeadline(QStringLiteral("Hello"));
        QCOMPARE(content.count(), 1);
        QCOMPARE(edited.count(), 1);
        d.setHeadline(QStringLiteral("Hello"));
        // A typed bidi control is removed as it is typed, so this is no change.
        d.setHeadline(QStringLiteral("Hel") + QChar(0x202E) + QStringLiteral("lo"));
        QCOMPARE(content.count(), 1);
        QCOMPARE(edited.count(), 1);
        // A trailing space survives typing (the next word follows), and is a change.
        d.setHeadline(QStringLiteral("Hello "));
        QCOMPARE(d.headline(), QStringLiteral("Hello "));
        QCOMPARE(content.count(), 2);
        // Blank lines survive in paragraphs while typing.
        d.setAboutMe(QStringLiteral("x\n\n"));
        QCOMPARE(d.aboutMe(), QStringLiteral("x\n\n"));
        QCOMPARE(content.count(), 3);
        // Text is capped at its bound.
        d.setDisplayName(QString(80, QLatin1Char('n')));
        QCOMPARE(d.displayName().size(), Profile::TextBounds::displayName);
        const int contentWrites = content.count();
        d.setDisplayName(QString(90, QLatin1Char('n')));
        QCOMPARE(content.count(), contentWrites);

        // Numbers clamp, and the clamped value again is no change.
        d.setMotifOpacity(200);
        QCOMPARE(d.motifOpacity(), 80);
        QCOMPARE(style.count(), 1);
        d.setMotifOpacity(95);
        d.setMotifOpacity(80);
        QCOMPARE(style.count(), 1);
        // Values the model forbids are refused.
        d.setMotif(99);
        d.setHeadingFont(int(Profile::Font::PixelFont));
        d.setBodyFont(int(Profile::Font::ScriptFont));
        d.setMood(Profile::maxMood + 1);
        d.setZodiac(13);
        QCOMPARE(style.count(), 1);
        QCOMPARE(d.headingFont(), int(Profile::Font::InterfaceFont));
        QCOMPARE(d.bodyFont(), int(Profile::Font::InterfaceFont));
        QCOMPARE(content.count(), contentWrites);
        // An adaptive page's shown colour written back is no change, and the
        // page keeps following the viewer's mode.
        QVERIFY(d.adaptive());
        d.setBoxFill(d.boxFill());
        d.setBoxOpacity(d.boxOpacity());
        QVERIFY(d.adaptive());
        QCOMPARE(style.count(), 1);
        d.setBoxFill(QColor());
        QCOMPARE(style.count(), 1);
        QCOMPARE(media.count(), 0);
        const int editedWrites = edited.count();

        // The view and the try-on ignore writes.
        ProfilePageObject &view = *p.view();
        QVERIFY(view.readOnly());
        QSignalSpy viewContent(&view, &ProfilePageObject::contentChanged);
        QSignalSpy viewStyle(&view, &ProfilePageObject::styleChanged);
        QSignalSpy viewEdited(&view, &ProfilePageObject::edited);
        const QString shown = view.headline();
        view.setHeadline(QStringLiteral("not mine to change"));
        view.setBoxFill(Qt::red);
        view.setLayout(int(Profile::Layout::SingleLayout));
        view.edit(Profile::defaultPage());
        QCOMPARE(view.headline(), shown);
        QCOMPARE(viewContent.count(), 0);
        QCOMPARE(viewStyle.count(), 0);
        QCOMPARE(viewEdited.count(), 0);
        p.tryOn()->setHeadline(QStringLiteral("nor this"));
        QVERIFY(p.tryOn()->headline() != QStringLiteral("nor this"));
        QCOMPARE(edited.count(), editedWrites);
    }

    void splitNotifyGroupsFireIndependently()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        QSignalSpy style(&d, &ProfilePageObject::styleChanged);
        QSignalSpy content(&d, &ProfilePageObject::contentChanged);
        QSignalSpy lists(&d, &ProfilePageObject::listsChanged);
        QSignalSpy media(&d, &ProfilePageObject::mediaChanged);
        QSignalSpy render(d.render(), &ProfileRenderStyle::changed);

        // Words: content, and the lists only when a box appears.
        d.setAboutMe(QStringLiteral("I play bass."));
        QCOMPARE(content.count(), 1);
        QCOMPARE(lists.count(), 1); // hasBlurbs
        d.setAboutMe(QStringLiteral("I play bass!"));
        QCOMPARE(content.count(), 2);
        QCOMPARE(lists.count(), 1);
        d.setInterestMusic(QStringLiteral("Paramore"));
        QCOMPARE(content.count(), 3);
        QCOMPARE(lists.count(), 2); // interestRows
        QCOMPARE(d.interestRows(), (QVariantList{QVariantMap{{QStringLiteral("label"), QStringLiteral("Music")},
                                                             {QStringLiteral("value"), QStringLiteral("Paramore")}}}));
        QCOMPARE(d.filledInterestCount(), 1);
        d.setHereFor(Profile::HereForFriends | Profile::HereForNetworking);
        QCOMPARE(d.detailRows().first().toMap().value(QStringLiteral("value")).toString(),
                 QStringLiteral("Friends, Networking"));
        // A keystroke never re-resolves colours.
        QCOMPARE(style.count(), 0);
        QCOMPARE(render.count(), 0);
        QCOMPARE(media.count(), 0);

        // Style: the style group and the resolved style, not the words.
        const int contentBefore = content.count();
        d.setBoxFill(QColor(QStringLiteral("#FFEECC")));
        QCOMPARE(style.count(), 1);
        QVERIFY(render.count() >= 1);
        QCOMPARE(content.count(), contentBefore);
        QCOMPARE(media.count(), 0);
        QCOMPARE(d.paletteSwatches().at(3).value<QColor>(), QColor(QStringLiteral("#FFEECC")));
        // The layout is style too, but it moves no colour.
        const int renderBefore = render.count();
        const int listsBefore = lists.count();
        d.setLayout(int(Profile::Layout::SingleLayout));
        QCOMPARE(style.count(), 2);
        QCOMPARE(render.count(), renderBefore);
        QCOMPARE(lists.count(), listsBefore);

        // Media: its own group.
        Profile::Page withSong = d.page();
        withSong.song = p.addMockMedia(Profile::MediaKind::SongMedia, PageSyncTest::testSong());
        QVERIFY(withSong.song.isSet());
        d.edit(withSong);
        QVERIFY(media.count() >= 1);
        QCOMPARE(style.count(), 2);
        QCOMPARE(content.count(), contentBefore);
        QVERIFY(d.hasSong());
        QVERIFY(!d.songPending());
        QCOMPARE(d.songKey(), QString::fromLatin1(withSong.song.sha256.toHex()));
    }

    void viewLoadOfAnEqualPageEmitsNothing()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.openContact(QStringLiteral("michael")));
        ProfilePageObject &view = *p.view();
        QSignalSpy style(&view, &ProfilePageObject::styleChanged);
        QSignalSpy content(&view, &ProfilePageObject::contentChanged);
        QSignalSpy lists(&view, &ProfilePageObject::listsChanged);
        QSignalSpy media(&view, &ProfilePageObject::mediaChanged);
        QSignalSpy render(view.render(), &ProfileRenderStyle::changed);
        QSignalSpy person(&p, &ProfileController::personChanged);

        const Profile::Page same = *Reference::seededPage(QStringLiteral("michael"));
        p.setMockPage(QStringLiteral("michael"), same);
        view.load(view.page());
        p.setDarkMode(p.darkMode());
        p.setPlainStyle(p.plainStyle());
        for (QSignalSpy *spy : {&style, &content, &lists, &media, &render, &person})
            QCOMPARE(spy->count(), 0);

        // A new revision that only changes words changes only the words.
        Profile::Page changed = same;
        changed.content.headline = QStringLiteral("New headline");
        p.setMockPage(QStringLiteral("michael"), changed);
        QCOMPARE(view.headline(), QStringLiteral("New headline"));
        QCOMPARE(content.count(), 1);
        QCOMPARE(style.count(), 0);
        QCOMPARE(render.count(), 0);
        QCOMPARE(media.count(), 0);
        QCOMPARE(person.count(), 0);
    }

    void refreshDoesNotReemitAnUnchangedPerson()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.openContact(QStringLiteral("michael")));
        ProfilePageObject &view = *p.view();
        QSignalSpy person(&p, &ProfileController::personChanged);
        QSignalSpy navigation(&p, &ProfileController::navigationChanged);
        QSignalSpy content(&view, &ProfilePageObject::contentChanged);
        QSignalSpy media(&view, &ProfilePageObject::mediaChanged);

        p.refresh();
        // Roster traffic about someone else (selection, unread) re-resolves
        // and finds nothing new.
        QVERIFY(chat.selectContact(QStringLiteral("sarah")));
        QVERIFY(chat.selectContact(QStringLiteral("michael")));
        chat.setSearchQuery(QStringLiteral("s"));
        chat.setSearchQuery(QString());
        QCOMPARE(person.count(), 0);
        QCOMPARE(navigation.count(), 0);
        QCOMPARE(content.count(), 0);
        QCOMPARE(media.count(), 0);

        // A change about them is one emission.
        QVector<Contact> rows;
        for (int row = 0; row < chat.contacts()->rowCount(); ++row) {
            Contact contact = *chat.contacts()->contactAt(row);
            if (contact.id == QStringLiteral("michael"))
                contact.presence = Presence::Busy;
            rows.append(contact);
        }
        chat.contacts()->setContacts(rows);
        QCOMPARE(person.count(), 1);
        QCOMPARE(p.personPresence(), int(Presence::Busy));
        p.refresh();
        QCOMPARE(person.count(), 1);
    }

    // --- Editing -----------------------------------------------------------------

    void dirtyTracksNormalizedDraftAgainstPublished()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.setMockPage(Reference::selfId(), publishedPage(QStringLiteral("Hi")));
        p.openOwn();
        QVERIFY(p.beginEditing());
        QVERIFY(!p.draftDirty());
        QSignalSpy dirty(&p, &ProfileController::draftDirtyChanged);
        ProfilePageObject &d = *p.draft();

        // What would be published is the same: not dirty.
        d.setHeadline(QStringLiteral("Hi "));
        d.setHeadline(QStringLiteral(" Hi  "));
        QVERIFY(!p.draftDirty());
        QCOMPARE(dirty.count(), 0);
        d.setHeadline(QStringLiteral("Hi there"));
        QVERIFY(p.draftDirty());
        QCOMPARE(dirty.count(), 1);
        d.setHeadline(QStringLiteral("Hi"));
        QVERIFY(!p.draftDirty());
        QCOMPARE(dirty.count(), 2);
        // A knob counts as much as a word.
        d.setBoxRadius(int(Profile::BoxRadius::SquareCorners));
        QVERIFY(p.draftDirty());
        p.undo();
        QVERIFY(!p.draftDirty());
    }

    void publishAssignsRevisionAndLeavesEditing()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(!p.publish()); // only from the editor
        QVERIFY(p.beginEditing());
        p.draft()->setHeadline(QStringLiteral("Fresh"));
        QSignalSpy published(&p, &ProfileController::published);
        QSignalSpy publishedChanged(&p, &ProfileController::publishedChanged);
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(p.publish());
        const qint64 after = QDateTime::currentMSecsSinceEpoch();

        const qint64 revision = p.publishedRevision();
        QVERIFY(revision >= before && revision <= after); // a clock revision
        QVERIFY(!p.editing());
        QVERIFY(!p.draftDirty());
        QVERIFY(!p.publishPending());
        QCOMPARE(published.count(), 1);
        QCOMPARE(published.first().first().toBool(), false); // online
        QVERIFY(publishedChanged.count() >= 1);
        // The page on screen is the one just published.
        QVERIFY(p.isOwnProfile());
        QCOMPARE(p.pageState(), int(PageState::CustomPage));
        QCOMPARE(p.view()->headline(), QStringLiteral("Fresh"));
        QCOMPARE(p.view()->revision(), revision);

        // Every publish is a newer revision, and nothing is left to offer.
        QVERIFY(p.beginEditing());
        QCOMPARE(p.survivingDraftAtMs(), 0);
        p.draft()->setHeadline(QStringLiteral("Again"));
        QVERIFY(p.publish());
        QVERIFY(p.publishedRevision() > revision);
        QCOMPARE(p.view()->headline(), QStringLiteral("Again"));
    }

    void publishDoesNotChangeTheSessionName()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        PageSyncTest::Peer &alice = fixture.a();
        QVERIFY(alice.session->setDisplayName(QStringLiteral("alice")).hasValue());
        useFixtureTime(fixture);
        LiveChat live(alice);
        ProfileController &p = live.profiles();
        const QString chatName = live.chat.localUserName();

        p.openOwn();
        QVERIFY(p.beginEditing());
        p.draft()->setDisplayName(QStringLiteral("Glitter Queen"));
        QVERIFY(p.publish());
        QVERIFY(p.publishedRevision() > 0);
        // The page carries the name; the account's name, which group rosters
        // send to people who are not contacts, is untouched.
        QCOMPARE(p.sync()->publishedPage().content.displayName, QStringLiteral("Glitter Queen"));
        QCOMPARE(p.view()->displayName(), QStringLiteral("Glitter Queen"));
        QCOMPARE(alice.session->displayName(), QStringLiteral("alice"));
        QCOMPARE(live.chat.localUserName(), chatName);
    }

    void publishWaitsForARunningImport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writePicture(dir, QStringLiteral("stage.png"), QSize(640, 400));
        QVERIFY(!picture.isEmpty());
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        p.draft()->setHeadline(QStringLiteral("With a picture"));
        QSignalSpy published(&p, &ProfileController::published);

        p.importBackground(QUrl::fromLocalFile(picture));
        QVERIFY(p.backgroundImporting());
        // Results land from the event loop, so the import is still running.
        QVERIFY(p.publish());
        QVERIFY(p.publishPending());
        QVERIFY(p.editing());
        QCOMPARE(p.publishedRevision(), 0);
        QCOMPARE(published.count(), 0);

        QTRY_VERIFY_WITH_TIMEOUT(!p.publishPending(), 20'000);
        QCOMPARE(published.count(), 1);
        QVERIFY(!p.editing());
        QVERIFY(p.publishedRevision() > 0);
        QCOMPARE(p.view()->headline(), QStringLiteral("With a picture"));
        QVERIFY(p.view()->hasBackgroundImage());
        QCOMPARE(p.view()->backgroundKind(), int(Profile::BackgroundKind::ImageBackground));

        // A save waiting on an import that fails does not happen.
        const QString junk = dir.filePath(QStringLiteral("junk.png"));
        QFile file(junk);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(4096, '\x07'));
        file.close();
        const qint64 revision = p.publishedRevision();
        QVERIFY(p.beginEditing());
        p.draft()->setHeadline(QStringLiteral("Second try"));
        p.importBackground(QUrl::fromLocalFile(junk));
        QVERIFY(p.publish());
        QVERIFY(p.publishPending());
        QTRY_VERIFY_WITH_TIMEOUT(!p.publishPending(), 20'000);
        QCOMPARE(published.count(), 1);
        QCOMPARE(p.publishedRevision(), revision);
        QVERIFY(p.editing());
        QVERIFY(!p.notice().isEmpty());
    }

    void publishedSignalReportsOffline()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.linkOnline());
        QSignalSpy viewer(&p, &ProfileController::viewerChanged);
        p.setMockLinkOnline(false);
        QVERIFY(!p.linkOnline());
        QCOMPARE(viewer.count(), 1);
        QSignalSpy published(&p, &ProfileController::published);

        p.openOwn();
        QVERIFY(p.beginEditing());
        p.draft()->setHeadline(QStringLiteral("Written on a plane"));
        QVERIFY(p.publish());
        QCOMPARE(published.count(), 1);
        QCOMPARE(published.at(0).first().toBool(), true); // "…when you're back online."

        p.setMockLinkOnline(true);
        QVERIFY(p.beginEditing());
        p.draft()->setHeadline(QStringLiteral("Landed"));
        QVERIFY(p.publish());
        QCOMPARE(published.count(), 2);
        QCOMPARE(published.at(1).first().toBool(), false);
    }

    void discardRestoresPublished()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.setMockPage(Reference::selfId(), publishedPage(QStringLiteral("Kept")));
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        d.setHeadline(QStringLiteral("Changed"));
        d.setMotif(int(Profile::Motif::Stars));
        p.endEditing(); // stored as a draft
        QVERIFY(p.beginEditing());
        QVERIFY(p.survivingDraftAtMs() > 0);
        d.setMeet(QStringLiteral("more"));
        QVERIFY(p.canUndo());

        p.discardChanges();
        QVERIFY(p.editing()); // stays in the editor
        QCOMPARE(d.headline(), QStringLiteral("Kept"));
        QCOMPARE(d.motif(), int(Profile::Motif::Bubbles));
        QCOMPARE(d.meet(), QString());
        QVERIFY(!p.draftDirty());
        QVERIFY(!p.canUndo());
        QCOMPARE(p.survivingDraftAtMs(), 0);
        // The stored draft went with it.
        p.endEditing();
        QVERIFY(p.beginEditing());
        QCOMPARE(p.survivingDraftAtMs(), 0);
        QCOMPARE(d.headline(), QStringLiteral("Kept"));
    }

    void survivingDraftIsOfferedOnReopen()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.setMockPage(Reference::selfId(), publishedPage(QStringLiteral("Published")));
        p.openOwn();
        QVERIFY(p.beginEditing());
        QCOMPARE(p.survivingDraftAtMs(), 0);
        ProfilePageObject &d = *p.draft();
        d.setHeadline(QStringLiteral("Unsaved thought"));
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        p.endEditing();
        QVERIFY(!p.editing());
        // Contacts (and the page on screen) still see the published version.
        QCOMPARE(p.view()->headline(), QStringLiteral("Published"));

        QSignalSpy editing(&p, &ProfileController::editingChanged);
        QVERIFY(p.beginEditing());
        QVERIFY(p.survivingDraftAtMs() >= before);
        QVERIFY(p.survivingDraftAtMs() <= QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(d.headline(), QStringLiteral("Unsaved thought"));
        QVERIFY(p.draftDirty());
        // Continue only hides the notice.
        const int editingBefore = editing.count();
        p.continueDraft();
        QCOMPARE(p.survivingDraftAtMs(), 0);
        QCOMPARE(editing.count(), editingBefore + 1);
        QCOMPARE(d.headline(), QStringLiteral("Unsaved thought"));
        p.endEditing();
        QVERIFY(p.beginEditing());
        QVERIFY(p.survivingDraftAtMs() > 0);
        // Start over goes back to the published page and forgets the draft.
        p.startOver();
        QCOMPARE(p.survivingDraftAtMs(), 0);
        QCOMPARE(d.headline(), QStringLiteral("Published"));
        QVERIFY(!p.draftDirty());
        p.endEditing();
        QVERIFY(p.beginEditing());
        QCOMPARE(p.survivingDraftAtMs(), 0);
        // A draft that would publish the same page is nothing to offer.
        d.setHeadline(QStringLiteral("Published "));
        p.endEditing();
        QVERIFY(p.beginEditing());
        QCOMPARE(p.survivingDraftAtMs(), 0);
    }

    void closeAllFlushesThePendingDraft()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        // No event loop runs between the keystroke and closing, so the 400 ms
        // autosave never fired: only the flush can have saved it.
        d.setHeadline(QStringLiteral("Typed just now"));
        p.closeAll();
        QVERIFY(!p.isOpen());
        QVERIFY(!p.editing());
        p.openOwn();
        QVERIFY(p.beginEditing());
        QCOMPARE(d.headline(), QStringLiteral("Typed just now"));
        QVERIFY(p.survivingDraftAtMs() > 0);

        // Back out of the editor flushes too.
        d.setHeadline(QStringLiteral("Typed again"));
        p.back();
        QVERIFY(!p.editing());
        QVERIFY(p.beginEditing());
        QCOMPARE(d.headline(), QStringLiteral("Typed again"));

        // So does starting a new stack from outside.
        d.setHeadline(QStringLiteral("Typed a third time"));
        QVERIFY(p.openContact(QStringLiteral("michael")));
        p.openOwn();
        QVERIFY(p.beginEditing());
        QCOMPARE(d.headline(), QStringLiteral("Typed a third time"));
    }

    void destructionFlushesThePendingDraft()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        PageSyncTest::Peer &alice = fixture.a();
        // The draft as the profile database holds it.
        const auto storedHeadline = [&alice]() -> QString {
            const QByteArray core = alice.pages().localPage().value().draftCore;
            const std::optional<Profile::Page> page = core.isEmpty() ? std::nullopt : decodePageCore(core);
            return page ? page->content.headline : QStringLiteral("<none>");
        };
        auto live = std::make_unique<LiveChat>(alice);
        ProfileController &p = live->profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());

        // Left alone, the draft saves itself shortly after the last change.
        p.draft()->setHeadline(QStringLiteral("Autosaved"));
        QCOMPARE(storedHeadline(), QStringLiteral("<none>"));
        QTRY_COMPARE_WITH_TIMEOUT(storedHeadline(), QStringLiteral("Autosaved"), 5'000);

        // The app quits within the autosave delay: the destructor saves it.
        p.draft()->setHeadline(QStringLiteral("Before quitting"));
        QCOMPARE(storedHeadline(), QStringLiteral("Autosaved"));
        live.reset();

        ProfilePageSync &sync = fixture.startSync(alice);
        const std::optional<Profile::Page> draft = sync.draft();
        QVERIFY(draft.has_value());
        QCOMPARE(draft->content.headline, QStringLiteral("Before quitting"));
        QCOMPARE(sync.publishedRevision(), 0);
    }

    void undoRedoAndGestureCoalescing()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        QVERIFY(!p.canUndo());
        QVERIFY(!p.canRedo());
        QSignalSpy history(&p, &ProfileController::historyChanged);

        const QString original = d.headline();
        d.setHeadline(QStringLiteral("One"));
        QVERIFY(p.canUndo());
        QVERIFY(history.count() >= 1);
        d.setHeadline(QStringLiteral("Two"));
        p.undo();
        QCOMPARE(d.headline(), QStringLiteral("One"));
        QVERIFY(p.canRedo());
        p.undo();
        QCOMPARE(d.headline(), original);
        QVERIFY(!p.canUndo());
        p.redo();
        QCOMPARE(d.headline(), QStringLiteral("One"));
        p.redo();
        QCOMPARE(d.headline(), QStringLiteral("Two"));
        QVERIFY(!p.canRedo());
        // A new change drops the undone future.
        p.undo();
        d.setHeadline(QStringLiteral("Three"));
        QVERIFY(!p.canRedo());

        // A drag is one step.
        const int opacity = d.motifOpacity();
        p.beginGesture(QStringLiteral("motifOpacity"));
        d.setMotifOpacity(20);
        d.setMotifOpacity(30);
        d.setMotifOpacity(40);
        p.endGesture();
        p.undo();
        QCOMPARE(d.motifOpacity(), opacity);
        QCOMPARE(d.headline(), QStringLiteral("Three"));
        p.redo();
        QCOMPARE(d.motifOpacity(), 40);
        // A drag that ends where it started leaves no step.
        p.beginGesture(QStringLiteral("motifOpacity"));
        d.setMotifOpacity(70);
        d.setMotifOpacity(40);
        p.endGesture();
        p.undo();
        QCOMPARE(d.motifOpacity(), opacity); // the earlier drag, not a no-op step
        p.redo();

        // A text field's focus period is one step.
        p.beginGesture(QStringLiteral("field:headline"));
        d.setHeadline(QStringLiteral("T"));
        d.setHeadline(QStringLiteral("Th"));
        d.setHeadline(QStringLiteral("The end"));
        p.endGesture();
        p.undo();
        QCOMPARE(d.headline(), QStringLiteral("Three"));
        p.redo();
        QCOMPARE(d.headline(), QStringLiteral("The end"));

        // Another gesture's key closes the open one.
        p.beginGesture(QStringLiteral("a"));
        d.setMotifOpacity(33);
        p.beginGesture(QStringLiteral("b"));
        d.setMotifOpacity(44);
        p.endGesture();
        p.undo();
        QCOMPARE(d.motifOpacity(), 33);
        p.undo();
        QCOMPARE(d.motifOpacity(), 40);

        // A controller edit inside an open gesture is a step of its own.
        p.beginGesture(QStringLiteral("c"));
        d.setMotifOpacity(21);
        p.applyPreset(int(Profile::Preset::LinenPreset));
        d.setHeadline(QStringLiteral("After the preset"));
        p.endGesture();
        p.undo();
        QCOMPARE(d.headline(), QStringLiteral("The end"));
        QCOMPARE(d.preset(), int(Profile::Preset::LinenPreset));
        p.undo();
        QCOMPARE(d.preset(), int(Profile::Preset::AeroSkyPreset));
        QCOMPARE(d.motifOpacity(), 21);
        p.undo();
        QCOMPARE(d.motifOpacity(), 40);

        // Undo and redo do nothing outside the editor.
        p.endEditing();
        p.undo();
        p.redo();

        // The history keeps the last 100 steps.
        QVERIFY(p.beginEditing());
        QVERIFY(!p.canUndo());
        for (int step = 1; step <= 105; ++step)
            d.setHeadline(QString::number(step));
        int undone = 0;
        while (p.canUndo() && undone < 200) {
            p.undo();
            ++undone;
        }
        QCOMPARE(undone, 100);
        QCOMPARE(d.headline(), QStringLiteral("5"));
    }

    void tryOnLeavesTheDraftUntouched()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        p.setTryOnPreset(int(Profile::Preset::ChromeY2KPreset)); // outside the editor: nothing
        QCOMPARE(p.tryOnPreset(), -1);
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        d.setHeadline(QStringLiteral("Mine"));
        const Profile::Page before = d.page();
        const bool couldUndo = p.canUndo();
        const bool wasDirty = p.draftDirty();
        QSignalSpy tryOn(&p, &ProfileController::tryOnChanged);
        QSignalSpy edited(&d, &ProfilePageObject::edited);

        p.setTryOnPreset(int(Profile::Preset::ChromeY2KPreset));
        QCOMPARE(p.tryOnPreset(), int(Profile::Preset::ChromeY2KPreset));
        QCOMPARE(tryOn.count(), 1);
        ProfilePageObject &t = *p.tryOn();
        QVERIFY(t.readOnly());
        QVERIFY(t.showEmptyModules());
        QCOMPARE(t.preset(), int(Profile::Preset::ChromeY2KPreset));
        QCOMPARE(t.headline(), QStringLiteral("Mine"));
        QCOMPARE(t.nameEffect(), int(Profile::presetTheme(Profile::Preset::ChromeY2KPreset).nameEffect));
        QCOMPARE(d.page(), before);
        QCOMPARE(edited.count(), 0);
        QCOMPARE(p.canUndo(), couldUndo);
        QCOMPARE(p.draftDirty(), wasDirty);
        p.setTryOnPreset(int(Profile::Preset::ChromeY2KPreset));
        QCOMPARE(tryOn.count(), 1);

        // Words typed meanwhile show through the try-on.
        d.setHeadline(QStringLiteral("Mine too"));
        QCOMPARE(t.headline(), QStringLiteral("Mine too"));
        QCOMPARE(t.preset(), int(Profile::Preset::ChromeY2KPreset));

        p.setTryOnPreset(-1);
        QCOMPARE(p.tryOnPreset(), -1);
        QCOMPARE(tryOn.count(), 2);
        p.setTryOnPreset(int(Profile::Preset::CustomPreset)); // not a preset one can try
        QCOMPARE(p.tryOnPreset(), -1);

        // Applying ends the try-on and is one step.
        p.setTryOnPreset(int(Profile::Preset::NeonZebraPreset));
        p.applyPreset(int(Profile::Preset::NeonZebraPreset));
        QCOMPARE(p.tryOnPreset(), -1);
        QCOMPARE(d.preset(), int(Profile::Preset::NeonZebraPreset));
        QCOMPARE(d.headline(), QStringLiteral("Mine too"));
        p.undo();
        QCOMPARE(d.preset(), int(Profile::Preset::AeroSkyPreset));
        // Leaving the editor ends a try-on.
        p.setTryOnPreset(int(Profile::Preset::LinenPreset));
        p.endEditing();
        QCOMPARE(p.tryOnPreset(), -1);
    }

    void resetToPresetAndSurpriseMeAreUndoable()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        d.setHeadline(QStringLiteral("Words stay"));

        p.applyPreset(int(Profile::Preset::SafetyPinPreset));
        QCOMPARE(d.preset(), int(Profile::Preset::SafetyPinPreset));
        QCOMPARE(d.layout(), int(Profile::Layout::FlippedLayout));
        QVERIFY(!d.styleEditedSincePreset());
        // The owner rearranges, then changes a knob.
        d.setLayout(int(Profile::Layout::ClassicLayout));
        d.setBoxFill(QColor(QStringLiteral("#123456")));
        QVERIFY(d.styleEditedSincePreset());

        p.resetToPreset();
        QVERIFY(!d.styleEditedSincePreset());
        QCOMPARE(d.page().theme, Profile::presetTheme(Profile::Preset::SafetyPinPreset));
        QCOMPARE(d.layout(), int(Profile::Layout::ClassicLayout)); // the arrangement stays
        QCOMPARE(d.headline(), QStringLiteral("Words stay"));
        p.undo();
        QCOMPARE(d.boxFill(), QColor(QStringLiteral("#123456")));
        QVERIFY(d.styleEditedSincePreset());
        p.redo();
        QVERIFY(!d.styleEditedSincePreset());

        // Surprise me: another preset, a motif of its family, the words kept.
        for (int attempt = 0; attempt < 12; ++attempt) {
            const int previous = d.preset();
            p.surpriseMe();
            QVERIFY(d.preset() != previous);
            const auto preset = Profile::Preset(d.preset());
            QVERIFY(Profile::motifFamily(preset).contains(Profile::Motif(d.motif())));
            QCOMPARE(d.headline(), QStringLiteral("Words stay"));
            p.undo();
            QCOMPARE(d.preset(), previous);
            p.redo();
        }
    }

    // --- Viewer settings -------------------------------------------------------

    void plainStyleAppliesToOthersOnly()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.setMockPage(Reference::selfId(), Reference::ownReferencePage());
        // Jessica's Scene Queen page, with a picture.
        Profile::Page jessica = *Reference::seededPage(QStringLiteral("jessica"));
        jessica.background = p.addMockMedia(Profile::MediaKind::BackgroundImageMedia, realJpeg(Qt::magenta));
        QVERIFY(jessica.background.isSet());
        jessica.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        p.setMockPage(QStringLiteral("jessica"), jessica);
        QVERIFY(p.openContact(QStringLiteral("jessica")));
        ProfilePageObject &view = *p.view();
        ProfileRenderStyle &render = *view.render();
        QVERIFY(!render.plain());
        QVERIFY(!view.backgroundImageKey().isEmpty());
        QCOMPARE(render.imageKey(), view.backgroundImageKey());
        const QString headline = view.headline();
        const QVariantList friends = view.topFriends();

        QSignalSpy viewer(&p, &ProfileController::viewerChanged);
        p.setPlainStyle(true);
        QCOMPARE(viewer.count(), 1);
        const Profile::Theme aero = Profile::aeroSkyTheme(false);
        QVERIFY(render.plain());
        QCOMPARE(render.backgroundKind(), int(aero.backgroundKind));
        QCOMPARE(render.color1(), rgb(aero.backgroundColor1));
        QCOMPARE(render.motif(), int(Profile::Motif::Bubbles));
        QCOMPARE(render.headingFamily(), QString()); // Standard
        QCOMPARE(render.nameFlourish(), int(Profile::Flourish::NoFlourish));
        QCOMPARE(render.ambient(), int(Profile::Ambient::NoAmbient));
        QCOMPARE(render.tableStyle(), int(Profile::TableStyle::CellTable));
        // The picture is neither shown nor decoded.
        QCOMPARE(render.imageKey(), QString());
        QCOMPARE(view.backgroundImageKey(), QString());
        // Everything else stays: the words, the friends, the page state.
        QCOMPARE(view.headline(), headline);
        QCOMPARE(view.topFriends(), friends);
        QCOMPARE(p.pageState(), int(PageState::CustomPage));

        // Never your own page, nor the editor.
        p.openOwn();
        QVERIFY(!view.render()->plain());
        QCOMPARE(view.render()->nameFamily(),
                 ProfileFonts::family(Profile::presetTheme(Profile::Preset::HeadlinerPreset).nameFont,
                                      ProfileFonts::Role::Name));
        QVERIFY(p.beginEditing());
        QVERIFY(!p.draft()->render()->plain());
        p.setTryOnPreset(int(Profile::Preset::SceneQueenPreset));
        QVERIFY(!p.tryOn()->render()->plain());
        p.endEditing();

        // Stubs always are, the setting off or not.
        p.setPlainStyle(false);
        p.openPerson(mockHex(QStringLiteral("dana-whitfield")), QStringLiteral("Dana"), QString());
        QVERIFY(view.render()->plain());
        // And turning it off brings the picture back on their page.
        QVERIFY(p.openContact(QStringLiteral("jessica")));
        QVERIFY(!render.plain());
        QVERIFY(!view.backgroundImageKey().isEmpty());
    }

    void closedPagesLetGoOfTheirMedia()
    {
        // Low memory mode frees a released picture at once, which makes the
        // release visible.
        ProfileMediaStore &store = ProfileMediaStore::instance();
        store.setKeepDecoded(false);
        ChatController chat;
        ProfileController &p = *chat.profiles();
        Profile::Page jessica = *Reference::seededPage(QStringLiteral("jessica"));
        jessica.background = p.addMockMedia(Profile::MediaKind::BackgroundImageMedia, realJpeg(Qt::darkMagenta));
        jessica.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        jessica.song = p.addMockMedia(Profile::MediaKind::SongMedia, PageSyncTest::testSong());
        QVERIFY(jessica.background.isSet() && jessica.song.isSet());
        p.setMockPage(QStringLiteral("jessica"), jessica);
        ProfilePageObject &view = *p.view();

        QVERIFY(p.openContact(QStringLiteral("jessica")));
        const QString imageKey = view.backgroundImageKey();
        const QString songKey = view.songKey();
        QVERIFY(!imageKey.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(store.image(imageKey).has_value(), 5'000);
        QVERIFY(!SongLibrary::instance().get(songKey).isEmpty());

        // The close fade still shows the page whole; then its media goes.
        p.closeAll();
        QCOMPARE(view.backgroundImageKey(), imageKey);
        QVERIFY(store.image(imageKey).has_value());
        QTRY_VERIFY_WITH_TIMEOUT(!store.image(imageKey).has_value(), 5'000);
        QCOMPARE(view.backgroundImageKey(), QString());
        QVERIFY(SongLibrary::instance().get(songKey).isEmpty());
        QCOMPARE(view.headline(), jessica.content.headline);

        // Opened again, it is all back.
        QVERIFY(p.openContact(QStringLiteral("jessica")));
        QCOMPARE(view.backgroundImageKey(), imageKey);
        QVERIFY(!view.backgroundPending());
        QVERIFY(!view.songPending());
        QVERIFY(!SongLibrary::instance().get(songKey).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(store.image(imageKey).has_value(), 5'000);
        // Reopened within the delay, nothing is let go.
        p.closeAll();
        QVERIFY(p.openContact(QStringLiteral("jessica")));
        QTest::qWait(ProfileController::idleMediaReleaseMs + 300);
        QCOMPARE(view.backgroundImageKey(), imageKey);
        QVERIFY(store.image(imageKey).has_value());

        // The editor's copy of your own picture goes when the editor closes.
        Profile::Page own = publishedPage(QStringLiteral("Mine"));
        own.background = p.addMockMedia(Profile::MediaKind::BackgroundImageMedia, realJpeg(Qt::darkCyan));
        own.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        p.setMockPage(Reference::selfId(), own);
        p.openOwn();
        QVERIFY(p.beginEditing());
        const QString draftKey = p.draft()->backgroundImageKey();
        QVERIFY(!draftKey.isEmpty());
        p.endEditing();
        QTRY_COMPARE_WITH_TIMEOUT(p.draft()->backgroundImageKey(), QString(), 5'000);
        QCOMPARE(p.view()->backgroundImageKey(), draftKey); // the page on screen keeps it
        QVERIFY(p.beginEditing());
        QCOMPARE(p.draft()->backgroundImageKey(), draftKey);
    }

    void adaptiveThemeFollowsDarkMode()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QVERIFY(p.openContact(QStringLiteral("michael"))); // Aero Sky
        ProfilePageObject &view = *p.view();
        QVERIFY(view.adaptive());
        const Profile::Theme light = Profile::aeroSkyTheme(false);
        const Profile::Theme dark = Profile::aeroSkyTheme(true);
        QCOMPARE(view.backgroundColor1(), rgb(light.backgroundColor1));
        QCOMPARE(view.render()->color1(), rgb(light.backgroundColor1));
        QSignalSpy style(&view, &ProfilePageObject::styleChanged);
        QSignalSpy render(view.render(), &ProfileRenderStyle::changed);

        p.setDarkMode(true);
        QVERIFY(p.darkMode());
        QCOMPARE(view.backgroundColor1(), rgb(dark.backgroundColor1));
        QCOMPARE(view.boxFill(), rgb(dark.boxFill));
        QCOMPARE(view.boxOpacity(), int(dark.boxOpacity));
        QCOMPARE(view.render()->color1(), rgb(dark.backgroundColor1));
        QVERIFY(view.render()->boxDark());
        QCOMPARE(style.count(), 1);
        QVERIFY(render.count() >= 1);
        // Knobs that are the owner's stay the owner's.
        QCOMPARE(view.motif(), int(view.page().theme.motif));

        // A page with its own colours keeps them in the dark.
        QVERIFY(p.openContact(QStringLiteral("jessica")));
        const Profile::Theme scene = Profile::presetTheme(Profile::Preset::SceneQueenPreset);
        QVERIFY(!view.adaptive());
        QCOMPARE(view.render()->color1(), rgb(scene.backgroundColor1));
    }

    void detachingAdaptiveKeepsTheShownPalette()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.setDarkMode(true);
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        QVERIFY(d.adaptive());
        const Profile::Theme dark = Profile::aeroSkyTheme(true);
        QCOMPARE(d.boxFill(), rgb(dark.boxFill));

        // A knob that is not a colour never detaches.
        d.setMotifScale(int(Profile::MotifScale::LargeMotif));
        QVERIFY(d.adaptive());

        d.setLinkColor(QColor(QStringLiteral("#FF8800")));
        QVERIFY(!d.adaptive());
        QCOMPARE(d.linkColor(), QColor(QStringLiteral("#FF8800")));
        // What was on screen is now the page's own palette.
        Profile::Theme expected = dark;
        expected.linkColor = 0xFF8800;
        expected.motifScale = Profile::MotifScale::LargeMotif;
        const Profile::Theme stored = d.page().theme;
        QCOMPARE(stored.backgroundColor1, expected.backgroundColor1);
        QCOMPARE(stored.backgroundColor2, expected.backgroundColor2);
        QCOMPARE(stored.motifInk, expected.motifInk);
        QCOMPARE(stored.boxFill, expected.boxFill);
        QCOMPARE(stored.boxOpacity, expected.boxOpacity);
        QCOMPARE(stored.borderColor, expected.borderColor);
        QCOMPARE(stored.headerFill, expected.headerFill);
        QCOMPARE(stored.headerText, expected.headerText);
        QCOMPARE(stored.bodyColor, expected.bodyColor);
        QCOMPARE(stored.labelColor, expected.labelColor);
        QCOMPARE(stored.nameColor, expected.nameColor);
        QCOMPARE(stored.linkColor, expected.linkColor);
        QCOMPARE(stored.motifScale, expected.motifScale);

        // So the viewer's mode no longer moves it.
        p.setDarkMode(false);
        QCOMPARE(d.backgroundColor1(), rgb(dark.backgroundColor1));
        QCOMPARE(d.render()->color1(), rgb(dark.backgroundColor1));
        // Undo follows the mode again.
        p.undo();
        QVERIFY(d.adaptive());
        QCOMPARE(d.backgroundColor1(), rgb(Profile::aeroSkyTheme(false).backgroundColor1));
        // Opting back in hands the colours to Aero Sky.
        p.redo();
        d.setAdaptive(true);
        QVERIFY(d.adaptive());
        QCOMPARE(d.linkColor(), rgb(Profile::aeroSkyTheme(false).linkColor));
    }

    // --- Media -------------------------------------------------------------------

    void importBackgroundSetsImageKind()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writePicture(dir, QStringLiteral("sunset.png"), QSize(900, 600));
        QVERIFY(!picture.isEmpty());
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        p.importBackground(QUrl::fromLocalFile(picture)); // outside the editor: nothing
        QVERIFY(!p.backgroundImporting());
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        const int kindBefore = d.backgroundKind();

        p.importBackground(QUrl::fromLocalFile(picture));
        QVERIFY(p.backgroundImporting());
        QCOMPARE(p.backgroundImportProgress(), 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(!p.backgroundImporting(), 20'000);
        QCOMPARE(p.backgroundImportProgress(), 1.0);
        QCOMPARE(p.notice(), QString());
        QCOMPARE(d.backgroundKind(), int(Profile::BackgroundKind::ImageBackground));
        QVERIFY(d.hasBackgroundImage());
        QVERIFY(!d.backgroundPending());
        // Never enlarged, never cropped, within one message.
        const Profile::MediaRef ref = d.page().background;
        QCOMPARE(ref.width, quint16(900));
        QCOMPARE(ref.height, quint16(600));
        QVERIFY(ref.bytes > 0 && ref.bytes <= quint32(maxBackgroundImageBytes));
        // The editor shows it (never in plain style).
        QVERIFY(!d.backgroundImageKey().isEmpty());
        QCOMPARE(d.render()->imageKey(), d.backgroundImageKey());
        QVERIFY(p.draftDirty());

        // One step to undo.
        p.undo();
        QVERIFY(!d.hasBackgroundImage());
        QCOMPARE(d.backgroundKind(), kindBefore);
        p.redo();
        QVERIFY(d.hasBackgroundImage());
        QVERIFY(!d.backgroundPending());
        // Removing it leaves a plain colour.
        p.removeBackgroundImage();
        QVERIFY(!d.hasBackgroundImage());
        QCOMPARE(d.backgroundKind(), int(Profile::BackgroundKind::SolidBackground));
        QCOMPARE(d.backgroundImageKey(), QString());
    }

    void importBackgroundRefusalSetsNotice()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString junk = dir.filePath(QStringLiteral("holiday.jpg"));
        QFile file(junk);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("definitely not a picture").repeated(64));
        file.close();
        const auto expected = processProfileBackgroundFile(junk, Qt::white);
        QVERIFY(!expected.hasValue());

        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        const Profile::Page before = d.page();
        QSignalSpy notice(&p, &ProfileController::noticeChanged);
        p.importBackground(QUrl::fromLocalFile(junk));
        QTRY_VERIFY_WITH_TIMEOUT(!p.backgroundImporting(), 20'000);
        QCOMPARE(p.notice(), profileBackgroundErrorText(expected.error()));
        QVERIFY(!p.notice().isEmpty());
        QCOMPARE(notice.count(), 1);
        QCOMPARE(d.page(), before);
        QVERIFY(!p.canUndo());
        QVERIFY(!p.draftDirty());

        // A file that is not there says so too.
        p.clearNotice();
        const QString missing = dir.filePath(QStringLiteral("gone.png"));
        const auto gone = processProfileBackgroundFile(missing, Qt::white);
        QVERIFY(!gone.hasValue());
        p.importBackground(QUrl::fromLocalFile(missing));
        QTRY_VERIFY_WITH_TIMEOUT(!p.backgroundImporting(), 20'000);
        QCOMPARE(p.notice(), profileBackgroundErrorText(gone.error()));
        QVERIFY(p.notice() != profileBackgroundErrorText(expected.error()));
        QCOMPARE(d.page(), before);
    }

    void importSongAnalysesThenEncodesTheDefaultWindow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeSong(dir, QStringLiteral("song.wav"), 52.0, 3.0);
        QVERIFY(!path.isEmpty());
        const std::optional<SongSourceInfo> reference = analyse(path);
        QVERIFY(reference.has_value());
        QVERIFY(reference->defaultWindowStartMs > 0);

        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        QVERIFY(!p.songImporting());
        QVERIFY(p.songSource().isEmpty());

        p.importSong(QUrl::fromLocalFile(path));
        QVERIFY(p.songImporting());
        QTRY_VERIFY_WITH_TIMEOUT(!p.songImporting(), 90'000);
        QCOMPARE(p.notice(), QString());

        // The file card and waveform are the analysis'.
        const QVariantMap source = p.songSource();
        QCOMPARE(source.value(QStringLiteral("fileName")).toString(), reference->fileName);
        QCOMPARE(source.value(QStringLiteral("formatLabel")).toString(), reference->formatLabel);
        QCOMPARE(source.value(QStringLiteral("durationMs")).toLongLong(), reference->durationMs);
        QVERIFY(source.value(QStringLiteral("available")).toBool());
        QCOMPARE(p.songPeaks().size(), reference->peaks.size());
        for (int i = 0; i < reference->peaks.size(); ++i)
            QCOMPARE(p.songPeaks().at(i).toInt(), int(reference->peaks.at(i)));
        // The default window: where the importer says, 45 s long.
        QCOMPARE(p.songWindowStartMs(), reference->defaultWindowStartMs);
        QCOMPARE(p.songWindowMs(), SongContainer::maxDurationMs);
        // The encoded window is the draft's song, its bytes ready for the player.
        QVERIFY(d.hasSong());
        QVERIFY(!d.songPending());
        QVERIFY(d.songBytes() > 0 && d.songBytes() <= maxSongBytes);
        QCOMPARE(p.songClipBytes(), d.songBytes());
        QVERIFY(d.songDurationMs() >= 44'000 && d.songDurationMs() <= qint64(Profile::maxSongRefDurationMs));
        const QByteArray container = SongLibrary::instance().get(d.songKey());
        QCOMPARE(container.size(), d.songBytes());
        QCOMPARE(QString::fromLatin1(pageMediaHash(container).toHex()), d.songKey());
        // A new file brings its title and artist from its tags.
        QCOMPARE(d.songTitle(), QStringLiteral("Paper Planes"));
        QCOMPARE(d.songArtist(), QStringLiteral("M.I.A."));
        QVERIFY(p.canUndo());
        QVERIFY(p.draftDirty());

        // Removing it clears the song and its words.
        p.removeSong();
        QVERIFY(!d.hasSong());
        QCOMPARE(d.songTitle(), QString());
        QCOMPARE(p.songClipBytes(), 0);
        QVERIFY(p.songSource().isEmpty());
        p.undo();
        QVERIFY(d.hasSong());
        QCOMPARE(p.songSource().value(QStringLiteral("fileName")).toString(), reference->fileName);
    }

    void songWindowReencodesAfterTheDebounce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeSong(dir, QStringLiteral("drag.wav"), 52.0, 1.0);
        QVERIFY(!path.isEmpty());
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        p.importSong(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!p.songImporting(), 90'000);
        QVERIFY(d.hasSong());
        const QString firstKey = d.songKey();
        d.setSongTitle(QStringLiteral("My cut"));

        // Dragging the window: each move restarts the wait, and nothing is
        // encoded until the handle rests.
        p.setSongWindow(1'000);
        QTest::qWait(250);
        p.setSongWindow(2'000);
        QTest::qWait(250);
        p.setSongWindow(4'000);
        QElapsedTimer rested;
        rested.start();
        QCOMPARE(p.songWindowStartMs(), 4'000);
        QVERIFY(p.songImporting()); // "Preparing…"
        QCOMPARE(d.songKey(), firstKey);
        QTRY_VERIFY_WITH_TIMEOUT(d.songKey() != firstKey, 90'000);
        QVERIFY(rested.elapsed() >= ProfileController::songWindowDelayMs);
        QTRY_VERIFY_WITH_TIMEOUT(!p.songImporting(), 90'000);
        const QString cutAt4 = d.songKey();
        QCOMPARE(p.songWindowStartMs(), 4'000);
        // A new window of the same file keeps what the owner typed.
        QCOMPARE(d.songTitle(), QStringLiteral("My cut"));

        // Past the end, the window is where a whole window still fits: the
        // importer's clamp, reported back.
        p.setSongWindow(50'000);
        QTRY_VERIFY_WITH_TIMEOUT(d.songKey() != cutAt4 && !p.songImporting(), 90'000);
        const qint64 duration = p.songSource().value(QStringLiteral("durationMs")).toLongLong();
        QCOMPARE(p.songWindowStartMs(), duration - SongContainer::maxDurationMs);

        // Undo brings the previous cut back, and the song tab with it.
        p.undo();
        QCOMPARE(d.songKey(), cutAt4);
        QCOMPARE(p.songWindowStartMs(), 4'000);
    }

    // --- Top Friends and layout -------------------------------------------------

    void topFriendCandidatesAreAcceptedPeopleOnly()
    {
        {
            ChatController chat;
            chat.addToGroup(QStringLiteral("sarah"));
            ProfileController &p = *chat.profiles();
            p.openOwn();
            QVERIFY(p.beginEditing());
            QStringList ids;
            for (const QVariant &candidate : p.topFriendCandidates()) {
                const QVariantMap entry = candidate.toMap();
                ids.append(entry.value(QStringLiteral("contactId")).toString());
                QCOMPARE(entry.value(QStringLiteral("placedIndex")).toInt(), -1);
            }
            ids.sort();
            QCOMPARE(ids, (QStringList{QStringLiteral("alex"), QStringLiteral("jessica"), QStringLiteral("michael"),
                                       QStringLiteral("ryan"), QStringLiteral("sarah"), QStringLiteral("tom")}));
            QSignalSpy candidates(&p, &ProfileController::topFriendCandidatesChanged);
            QVERIFY(p.addTopFriend(QStringLiteral("jessica")));
            QCOMPARE(candidates.count(), 1);
            for (const QVariant &candidate : p.topFriendCandidates()) {
                const QVariantMap entry = candidate.toMap();
                if (entry.value(QStringLiteral("contactId")).toString() == QStringLiteral("jessica")) {
                    QCOMPARE(entry.value(QStringLiteral("placedIndex")).toInt(), 0);
                    QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("Jessica"));
                    QCOMPARE(entry.value(QStringLiteral("avatarKey")).toString(), QStringLiteral("jessica"));
                }
            }
        }

        // Live: requests either way and blocked people are not candidates.
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        const qint64 now = fixture.clock.nowMs;
        const auto grace = addRequestRow(fixture.a(), QStringLiteral("grace"), ContactState::PendingIncoming, now);
        const auto olivia = addRequestRow(fixture.a(), QStringLiteral("olivia"), ContactState::PendingOutgoing, now);
        const auto mallory = addRequestRow(fixture.a(), QStringLiteral("mallory"), ContactState::Blocked, now);
        QVERIFY(grace && olivia && mallory);
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        QCOMPARE(p.topFriendCandidates().size(), 1);
        const QVariantMap bob = p.topFriendCandidates().first().toMap();
        QCOMPARE(bob.value(QStringLiteral("contactId")).toString(), fixture.b().account.toHex());
        QCOMPARE(bob.value(QStringLiteral("name")).toString(), QStringLiteral("bob"));
        for (const auto &someone : {grace, olivia, mallory})
            QVERIFY(!p.addTopFriend(someone->toHex()));
        QVERIFY(p.addTopFriend(fixture.b().account.toHex()));
        QCOMPARE(p.draft()->page().topFriends.first().accountId, fixture.b().account.bytes());
    }

    void topFriendsCapAtEight()
    {
        ChatController chat;
        QVector<Contact> rows;
        for (int i = 0; i < 10; ++i)
            rows.append(person(QStringLiteral("f%1").arg(i), QStringLiteral("Friend %1").arg(i)));
        chat.contacts()->setContacts(rows);
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(!p.addTopFriend(QStringLiteral("f0"))); // outside the editor
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();

        for (int i = 0; i < Profile::maxTopFriends; ++i)
            QVERIFY(p.addTopFriend(QStringLiteral("f%1").arg(i)));
        QCOMPARE(d.page().topFriends.size(), 8);
        // The label on the wire is the owner's name for them.
        QCOMPARE(d.page().topFriends.at(3).name, QStringLiteral("Friend 3"));
        QCOMPARE(d.page().topFriends.at(3).accountId, mockBytes(QStringLiteral("f3")));
        QCOMPARE(tile(d, 3).value(QStringLiteral("isContact")).toBool(), true);

        QVERIFY(!p.addTopFriend(QStringLiteral("f8")));
        QVERIFY(!p.notice().isEmpty());
        QVERIFY(!p.addTopFriend(QStringLiteral("f2"))); // already placed
        QVERIFY(!p.addTopFriend(p.localAccountId()));  // not yourself
        QVERIFY(!p.addTopFriend(QStringLiteral("stranger")));
        QCOMPARE(d.page().topFriends.size(), 8);

        // Remove, reorder: each one step.
        p.removeTopFriend(0);
        QCOMPARE(d.page().topFriends.size(), 7);
        QVERIFY(p.addTopFriend(QStringLiteral("f8")));
        QCOMPARE(d.page().topFriends.last().accountId, mockBytes(QStringLiteral("f8")));
        p.moveTopFriend(7, 0);
        QCOMPARE(d.page().topFriends.first().accountId, mockBytes(QStringLiteral("f8")));
        QCOMPARE(tile(d, 0).value(QStringLiteral("name")).toString(), QStringLiteral("Friend 8"));
        p.moveTopFriend(0, 99); // clamped to the end
        QCOMPARE(d.page().topFriends.last().accountId, mockBytes(QStringLiteral("f8")));
        p.removeTopFriend(42);
        QCOMPARE(d.page().topFriends.size(), 8);
        p.undo(); // the clamped move
        QCOMPARE(d.page().topFriends.first().accountId, mockBytes(QStringLiteral("f8")));
        p.undo(); // the move to the front
        QCOMPARE(d.page().topFriends.last().accountId, mockBytes(QStringLiteral("f8")));
        p.undo(); // the add
        QCOMPARE(d.page().topFriends.size(), 7);
        p.undo(); // the removal
        QCOMPARE(d.page().topFriends.first().accountId, mockBytes(QStringLiteral("f0")));
    }

    void moduleArrangementEdits()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        const auto moduleIds = [](std::initializer_list<Profile::Module> modules) {
            QVariantList ids;
            for (const Profile::Module module : modules)
                ids.append(int(module));
            return ids;
        };
        using Profile::Module;
        // The editor shows every module, empty or not.
        QCOMPARE(d.narrowModules(), moduleIds({Module::HandleModule, Module::SongModule, Module::InterestsModule,
                                               Module::DetailsModule}));
        QCOMPARE(d.wideModules(), moduleIds({Module::BlurbsModule, Module::TopFriendsModule}));
        // A viewer only what has content (the handle box is the app's).
        QCOMPARE(p.view()->narrowModules(), moduleIds({Module::HandleModule}));
        QCOMPARE(p.view()->wideModules(), QVariantList());

        p.moveModule(int(Module::SongModule), int(Profile::Column::WideColumn), 0);
        QCOMPARE(d.wideModules(), moduleIds({Module::SongModule, Module::BlurbsModule, Module::TopFriendsModule}));
        QCOMPARE(d.narrowModules(),
                 moduleIds({Module::HandleModule, Module::InterestsModule, Module::DetailsModule}));
        const QVariantMap song = d.moduleArrangement().at(3).toMap();
        QCOMPARE(song.value(QStringLiteral("module")).toInt(), int(Module::SongModule));
        QCOMPARE(song.value(QStringLiteral("column")).toInt(), int(Profile::Column::WideColumn));
        QCOMPARE(song.value(QStringLiteral("name")).toString(), Profile::moduleName(Module::SongModule));
        QCOMPARE(song.value(QStringLiteral("hasContent")).toBool(), false);

        p.setModuleVisible(int(Module::InterestsModule), false);
        QCOMPARE(d.narrowModules(), moduleIds({Module::HandleModule, Module::DetailsModule}));
        for (const QVariant &entry : d.moduleArrangement()) {
            const QVariantMap map = entry.toMap();
            if (map.value(QStringLiteral("module")).toInt() == int(Module::InterestsModule))
                QCOMPARE(map.value(QStringLiteral("visible")).toBool(), false);
        }

        // Nothing that would not change anything is a step.
        QSignalSpy history(&p, &ProfileController::historyChanged);
        const Profile::Page arranged = d.page();
        p.moveModule(99, 0, 0);
        p.moveModule(int(Module::SongModule), 5, 0);
        p.moveModule(int(Module::SongModule), int(Profile::Column::WideColumn), 0);
        p.setModuleVisible(int(Module::InterestsModule), false);
        QCOMPARE(d.page(), arranged);
        QCOMPARE(history.count(), 0);

        p.undo();
        QCOMPARE(d.narrowModules(), moduleIds({Module::HandleModule, Module::InterestsModule,
                                               Module::DetailsModule}));
        p.undo();
        QCOMPARE(d.narrowModules(), moduleIds({Module::HandleModule, Module::SongModule, Module::InterestsModule,
                                               Module::DetailsModule}));

        // Published, the viewer sees the arrangement with content.
        p.moveModule(int(Module::InterestsModule), int(Profile::Column::WideColumn), 0);
        d.setInterestBooks(QStringLiteral("The Road"));
        QVERIFY(p.publish());
        QCOMPARE(p.view()->wideModules(), moduleIds({Module::InterestsModule}));
        QCOMPARE(p.view()->narrowModules(), moduleIds({Module::HandleModule}));
    }

    // --- Actions and helpers ---------------------------------------------------

    void copyHandleAndCopyTextUseTheClipboard()
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QVERIFY(clipboard != nullptr);
        clipboard->clear();
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QSignalSpy notice(&p, &ProfileController::noticeChanged);

        QVERIFY(p.openContact(QStringLiteral("michael")));
        p.copyHandle();
        QCOMPARE(clipboard->text(), QStringLiteral("@michael"));
        QCOMPARE(p.notice(), QStringLiteral("Copied @michael"));
        QCOMPARE(notice.count(), 1);
        p.clearNotice();
        QCOMPARE(p.notice(), QString());
        QCOMPARE(notice.count(), 2);

        p.copyText(QStringLiteral("openchat://invite/abc"), QStringLiteral("Copied invite link"));
        QCOMPARE(clipboard->text(), QStringLiteral("openchat://invite/abc"));
        QCOMPARE(p.notice(), QStringLiteral("Copied invite link"));
        // Nothing to copy: nothing happens.
        p.copyText(QString(), QStringLiteral("Copied nothing"));
        QCOMPARE(p.notice(), QStringLiteral("Copied invite link"));
        p.openPerson(mockHex(QStringLiteral("kenji-mori")), QStringLiteral("Kenji"), QString());
        QCOMPARE(p.personHandle(), QString());
        p.copyHandle();
        QCOMPARE(clipboard->text(), QStringLiteral("openchat://invite/abc"));
    }

    void contrastForMatchesTheEngine()
    {
        ChatController chat;
        ProfileController &p = *chat.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        p.applyPreset(int(Profile::Preset::LinenPreset));
        const Profile::Theme linen = Profile::presetTheme(Profile::Preset::LinenPreset);
        const QVector<QColor> samples = ProfileReadability::pageSamples(linen, std::nullopt);
        const QList<QColor> colours{QColor(QStringLiteral("#777777")), QColor(Qt::white),
                                    QColor(QStringLiteral("#1F4E79"))};
        for (int role = int(Profile::InkRole::BodyInk); role <= int(Profile::InkRole::AltHeaderTextInk); ++role) {
            for (const QColor &colour : colours) {
                const QVariantMap shown = p.contrastFor(role, colour);
                const ProfileReadability::ContrastReport engine =
                    ProfileReadability::contrastFor(linen, samples, Profile::InkRole(role), colour);
                QCOMPARE(shown.value(QStringLiteral("ratio")).toDouble(), engine.ratio);
                QCOMPARE(shown.value(QStringLiteral("passes")).toBool(), engine.passes);
                QCOMPARE(shown.value(QStringLiteral("shown")).value<QColor>(), engine.shown);
            }
        }
        // White on Linen's pale boxes is unreadable, and is shown darker.
        const QVariantMap white = p.contrastFor(int(Profile::InkRole::BodyInk), Qt::white);
        QVERIFY(!white.value(QStringLiteral("passes")).toBool());
        QVERIFY(white.value(QStringLiteral("shown")).value<QColor>() != QColor(Qt::white));

        // An adaptive page is measured against the palette the viewer's mode
        // shows: white link text reads on dark Aero Sky boxes, not on light.
        p.undo();
        QVERIFY(p.draft()->adaptive());
        const auto measure = [](const Profile::Theme &theme) {
            return ProfileReadability::contrastFor(theme, ProfileReadability::pageSamples(theme, std::nullopt),
                                                   Profile::InkRole::LinkInk, Qt::white);
        };
        const auto light = measure(ProfileRenderStyle::withAeroSkyColours(Profile::defaultTheme(), false));
        const auto dark = measure(ProfileRenderStyle::withAeroSkyColours(Profile::defaultTheme(), true));
        QVERIFY(light.ratio != dark.ratio);
        const auto linkWhite = [&p] {
            return p.contrastFor(int(Profile::InkRole::LinkInk), Qt::white).value(QStringLiteral("ratio")).toDouble();
        };
        QCOMPARE(linkWhite(), light.ratio);
        p.setDarkMode(true);
        QCOMPARE(linkWhite(), dark.ratio);
        // Nonsense asks for nothing.
        QVERIFY(p.contrastFor(42, Qt::white).isEmpty());
        QVERIFY(p.contrastFor(int(Profile::InkRole::BodyInk), QColor()).isEmpty());
    }

    void recentColorsPersistPerViewer()
    {
        {
            ChatController chat;
            ProfileController &p = *chat.profiles();
            QVERIFY(p.recentColors().isEmpty());
            QSignalSpy recent(&p, &ProfileController::recentColorsChanged);
            p.rememberColor(QColor(QStringLiteral("#ff0000")));
            p.rememberColor(QColor(QStringLiteral("#00ff00")));
            p.rememberColor(QColor(QStringLiteral("#FF0000"))); // to the front, once
            QCOMPARE(p.recentColors(), (QVariantList{QColor(QStringLiteral("#FF0000")),
                                                     QColor(QStringLiteral("#00FF00"))}));
            QCOMPARE(recent.count(), 3);
            p.rememberColor(QColor(QStringLiteral("#ff0000"))); // already first: no change
            p.rememberColor(QColor());
            QCOMPARE(recent.count(), 3);
            for (int i = 0; i < 10; ++i)
                p.rememberColor(QColor::fromRgb(10 * i, 20, 30));
            QCOMPARE(p.recentColors().size(), ProfileController::maxRecentColors);
            QCOMPARE(p.recentColors().first().value<QColor>(), QColor::fromRgb(90, 20, 30));
            p.setLastTab(int(Profile::EditorTab::SongTab));
            p.setLastTab(99);
            QCOMPARE(p.lastTab(), int(Profile::EditorTab::SongTab));
        }
        // The next session of this viewer finds them.
        ChatController chat;
        ProfileController &p = *chat.profiles();
        QCOMPARE(p.recentColors().size(), ProfileController::maxRecentColors);
        QCOMPARE(p.recentColors().first().value<QColor>(), QColor::fromRgb(90, 20, 30));
        QCOMPARE(p.lastTab(), int(Profile::EditorTab::SongTab));
    }

    void personVerifiedFollowsTheContactRecord()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        QVERIFY(p.openContact(fixture.b().account.toHex()));
        QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
        QVERIFY(!p.personVerified());
        QSignalSpy person(&p, &ProfileController::personChanged);
        // The Safety Number dialog marks the contact verified.
        QVERIFY(fixture.a().contacts().setVerified(fixture.b().account, true, fixture.clock.nowMs).hasValue());
        p.refresh();
        QVERIFY(p.personVerified());
        QCOMPARE(person.count(), 1);
        QVERIFY(fixture.a().contacts().setVerified(fixture.b().account, false, fixture.clock.nowMs).hasValue());
        p.refresh();
        QVERIFY(!p.personVerified());
        QCOMPARE(person.count(), 2);
    }

    void pageLoadingWhileAwaiting()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        ProfilePageSync &bob = fixture.startSync(fixture.b());
        QVERIFY(p.openContact(fixture.b().account.toHex()));
        QCOMPARE(p.pageState(), int(PageState::DefaultPage));
        QVERIFY(!p.pageLoading()); // "…hasn't shared a profile page yet."

        // Just accepted: their page is being asked for.
        live.requests.contactAccepted(fixture.b().account);
        QVERIFY(p.pageLoading()); // "Getting bob's page…"
        QCOMPARE(p.pageState(), int(PageState::DefaultPage));

        QVERIFY(PageSyncTest::publishPage(bob, QStringLiteral("Hi from Bob")) > 0);
        fixture.settle();
        QCOMPARE(p.pageState(), int(PageState::CustomPage));
        QVERIFY(!p.pageLoading());
        QCOMPARE(p.view()->headline(), QStringLiteral("Hi from Bob"));
    }

    void strangerUpgradesWhenTheyBecomeAContact()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        PageSyncTest::Peer *carol = fixture.addPeer(QStringLiteral("carol"));
        QVERIFY(carol != nullptr);
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();

        p.openPerson(carol->account.toHex(), QStringLiteral("Carol from the call"), QString());
        QCOMPARE(p.relationship(), int(Relationship::StrangerPerson));
        QCOMPARE(p.pageState(), int(PageState::StubPage));
        QCOMPARE(p.personName(), QStringLiteral("Carol from the call"));

        // Carol and Alice become contacts: the stub turns into her page in place.
        QVERIFY(fixture.link(fixture.a(), *carol).has_value());
        live.requests.contactAccepted(carol->account);
        QCOMPARE(p.depth(), 1);
        QCOMPARE(p.relationship(), int(Relationship::ContactPerson));
        QCOMPARE(p.personId(), carol->account.toHex());
        QCOMPARE(p.personName(), QStringLiteral("carol")); // the roster's name, not the label
        QCOMPARE(p.personHandle(), QStringLiteral("carol"));
        QCOMPARE(p.pageState(), int(PageState::DefaultPage));
        QVERIFY(p.pageLoading());

        ProfilePageSync &carolSync = fixture.startSync(*carol);
        QVERIFY(PageSyncTest::publishPage(carolSync, QStringLiteral("Carol's page")) > 0);
        fixture.settle();
        QCOMPARE(p.pageState(), int(PageState::CustomPage));
        QCOMPARE(p.view()->headline(), QStringLiteral("Carol's page"));
        QCOMPARE(p.depth(), 1);
    }

    // --- Live ------------------------------------------------------------------

    void livePublishReachesThePeer()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        QVERIFY(p.sync() != nullptr);
        ProfilePageSync &bob = fixture.startSync(fixture.b());
        fixture.settle();
        // Nothing goes out before the first publish.
        QCOMPARE(PageSyncTest::TwoPeerFixture::envelopesTo(fixture.a(), fixture.b().device), 0);

        p.openOwn();
        QVERIFY(p.beginEditing());
        p.applyPreset(int(Profile::Preset::GlitterGirlPreset));
        p.draft()->setHeadline(QStringLiteral("Live from Alice"));
        QSignalSpy published(&p, &ProfileController::published);
        QVERIFY(p.publish());
        QCOMPARE(published.count(), 1);
        QCOMPARE(published.first().first().toBool(), false); // the link is up
        const qint64 revision = p.publishedRevision();
        QCOMPARE(revision, fixture.clock.nowMs); // the sync's clock

        fixture.settle();
        const auto received = bob.contactPage(fixture.a().account);
        QVERIFY(received.has_value());
        QCOMPARE(received->page.revision, revision);
        QCOMPARE(received->page.content.headline, QStringLiteral("Live from Alice"));
        QCOMPARE(received->page.preset, Profile::Preset::GlitterGirlPreset);
        QCOMPARE(PageSyncTest::TwoPeerFixture::payloadsFrom(fixture.b(), fixture.a(), ProfilePayloadKind::PageCore)
                     .size(),
                 1);
        QVERIFY(!fixture.a().engine().isFailedClosed());
    }

    void liveReceivedPageShowsWhenOpen()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        ProfilePageSync &bob = fixture.startSync(fixture.b());
        QVERIFY(p.openContact(fixture.b().account.toHex()));
        QCOMPARE(p.pageState(), int(PageState::DefaultPage));
        QSignalSpy person(&p, &ProfileController::personChanged);

        // Bob's page names a picture. Alice has never sent a page message, so
        // Bob pushes only the core: the page shows, its picture still missing.
        const QByteArray jpeg = realJpeg(QColor(QStringLiteral("#3366AA")));
        QVERIFY(PageSyncTest::publishPage(bob, QStringLiteral("Bob's page"), jpeg) > 0);
        fixture.settle();
        QCOMPARE(p.pageState(), int(PageState::CustomPage));
        QCOMPARE(p.view()->headline(), QStringLiteral("Bob's page"));
        QVERIFY(p.view()->hasBackgroundImage());
        QVERIFY(p.view()->backgroundPending());
        QVERIFY(p.pageIncomplete());
        QCOMPARE(p.view()->backgroundImageKey(), QString());
        QVERIFY(person.count() >= 1);

        // Alice publishes: now Bob knows she understands pages and pushes the
        // picture, which fills in on the open page.
        p.openOwn();
        QVERIFY(p.beginEditing());
        p.draft()->setHeadline(QStringLiteral("Alice's page"));
        QVERIFY(p.publish());
        QVERIFY(p.openContact(fixture.b().account.toHex()));
        fixture.settle();
        QVERIFY(!p.view()->backgroundPending());
        QVERIFY(!p.pageIncomplete());
        QCOMPARE(p.view()->backgroundImageKey(), ProfileMediaStore::imageKeyFor(pageMediaHash(jpeg)));
        QCOMPARE(p.view()->render()->imageKey(), p.view()->backgroundImageKey());
        QCOMPARE(p.view()->render()->backgroundKind(), int(Profile::BackgroundKind::ImageBackground));
    }

    void draftSurvivesRestart()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        const qint64 writtenAt = fixture.clock.nowMs;
        {
            LiveChat live(fixture.a());
            ProfileController &p = live.profiles();
            p.openOwn();
            QVERIFY(p.beginEditing());
            p.draft()->setHeadline(QStringLiteral("Half-written"));
            p.draft()->setBoxRadius(int(Profile::BoxRadius::RoundCorners));
            p.endEditing();
        }
        fixture.clock.advance(60'000);
        QVERIFY(fixture.reopen(fixture.a()));

        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        QCOMPARE(p.publishedRevision(), 0);
        p.openOwn();
        QCOMPARE(p.view()->headline(), QString()); // never published
        QVERIFY(p.beginEditing());
        QCOMPARE(p.survivingDraftAtMs(), writtenAt); // "You have unsaved changes from …"
        QCOMPARE(p.draft()->headline(), QStringLiteral("Half-written"));
        QCOMPARE(p.draft()->boxRadius(), int(Profile::BoxRadius::RoundCorners));
        QVERIFY(p.draftDirty());
    }

    void ownHandleIsBackfilledFromTheRelay()
    {
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        PageSyncTest::Peer &alice = fixture.a();
        QCOMPARE(alice.session->handle(), QString());
        useFixtureTime(fixture);
        {
            // Directory endpoints left unset: a lookup the controller starts
            // is refused on the spot, which tells the test that it asked.
            RelayClient relay(alice.device, alice.account, RelayEndpoints{}, RelayCredentials{});
            int asked = 0;
            connect(&relay, &RelayClient::transportError, this, [&asked] { ++asked; });
            LiveChat live(alice);
            ProfileController &p = live.profiles();
            QCOMPARE(p.localHandle(), QString());
            QSignalSpy identity(&p, &ProfileController::localIdentityChanged);

            live.chat.setPresenceRelay(&relay); // as the app hands over its relay
            QCOMPARE(asked, 1);
            relay.accountResolved(AccountId::generate(), QStringLiteral("mallory")); // not ours
            QCOMPARE(p.localHandle(), QString());
            relay.accountResolved(alice.account, QStringLiteral("Alice"));
            QCOMPARE(p.localHandle(), QStringLiteral("alice"));
            QCOMPARE(identity.count(), 1);
            QCOMPARE(alice.session->handle(), QStringLiteral("alice"));
            p.openOwn();
            QCOMPARE(p.personHandle(), QStringLiteral("alice"));
            p.refresh();
            QCOMPARE(asked, 1); // once is enough
        }
        // The profile remembers it: a later session does not ask.
        QVERIFY(fixture.reopen(alice));
        RelayClient relay(alice.device, alice.account, RelayEndpoints{}, RelayCredentials{});
        int asked = 0;
        connect(&relay, &RelayClient::transportError, this, [&asked] { ++asked; });
        LiveChat live(alice);
        live.chat.setPresenceRelay(&relay);
        QCOMPARE(live.profiles().localHandle(), QStringLiteral("alice"));
        QCOMPARE(asked, 0);
    }

    void undoToACollectedPictureDropsItAndSaysSo()
    {
        // A picture the draft let go of is kept for the collection's grace
        // (ten minutes), so undo can bring it back; an undo reaching further
        // may find it gone. The draft then drops it and tells the owner,
        // rather than keeping a reference nothing can publish.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writePicture(dir, QStringLiteral("old.png"), QSize(320, 200));
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        PageSyncTest::Peer &alice = fixture.a();
        LiveChat live(alice);
        ProfileController &p = live.profiles();
        p.openOwn();
        QVERIFY(p.beginEditing());
        ProfilePageObject &d = *p.draft();
        p.importBackground(QUrl::fromLocalFile(picture));
        QTRY_VERIFY_WITH_TIMEOUT(!p.backgroundImporting(), 20'000);
        const QByteArray hash = d.page().background.sha256;
        QVERIFY(!p.sync()->localMedia(hash).isEmpty());

        p.removeBackgroundImage();
        QTRY_VERIFY_WITH_TIMEOUT(!alice.pages().localPage().value().draftBackground.has_value(), 5'000);
        // Within the grace, undo brings it back whole.
        p.undo();
        QVERIFY(d.hasBackgroundImage());
        QVERIFY(!d.backgroundPending());
        QCOMPARE(p.notice(), QString());
        p.redo();
        QVERIFY(!d.hasBackgroundImage());
        QTRY_VERIFY_WITH_TIMEOUT(!alice.pages().localPage().value().draftBackground.has_value(), 5'000);

        // Past it, a collection frees the blob, and undo cannot restore it.
        fixture.clock.advance(PageSyncTest::quietLimits().localBlobGraceMs + 60'000);
        p.sync()->collectGarbage();
        QVERIFY(p.sync()->localMedia(hash).isEmpty());
        p.undo();
        QVERIFY(!d.hasBackgroundImage());
        QCOMPARE(d.backgroundKind(), int(Profile::BackgroundKind::SolidBackground));
        QCOMPARE(p.notice(), QStringLiteral("Your background picture is no longer available. Choose it again."));
        // What is saved and published never names the missing blob.
        QVERIFY(p.publish());
        QVERIFY(!p.sync()->publishedPage().background.isSet());
    }

    void callActiveDefersMedia()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writePicture(dir, QStringLiteral("stage.png"), QSize(480, 320));
        PageSyncTest::TwoPeerFixture fixture;
        QVERIFY(fixture.setUp());
        useFixtureTime(fixture);
        LiveChat live(fixture.a());
        ProfileController &p = live.profiles();
        ProfilePageSync &bob = fixture.startSync(fixture.b());
        // Bob's page tells Alice his client understands pages.
        QVERIFY(PageSyncTest::publishPage(bob, QStringLiteral("Bob")) > 0);
        fixture.settle();

        QSignalSpy viewer(&p, &ProfileController::viewerChanged);
        p.setCallActive(true); // main.cpp: callChanged → inCall
        QVERIFY(p.callActive());
        QCOMPARE(viewer.count(), 1);
        p.setCallActive(true);
        QCOMPARE(viewer.count(), 1);

        p.openOwn();
        QVERIFY(p.beginEditing());
        p.importBackground(QUrl::fromLocalFile(picture));
        QTRY_VERIFY_WITH_TIMEOUT(!p.backgroundImporting(), 20'000);
        p.draft()->setHeadline(QStringLiteral("On a call"));
        QVERIFY(p.publish());
        fixture.settle();
        using PageSyncTest::TwoPeerFixture;
        // During the call only the core leaves.
        QCOMPARE(TwoPeerFixture::payloadsFrom(fixture.b(), fixture.a(), ProfilePayloadKind::PageCore).size(), 1);
        QCOMPARE(TwoPeerFixture::payloadsFrom(fixture.b(), fixture.a(), ProfilePayloadKind::PageMedia).size(), 0);
        QVERIFY(bob.contactPage(fixture.a().account).has_value());
        QVERIFY(!bob.contactPage(fixture.a().account)->backgroundPresent);

        p.setCallActive(false);
        fixture.settle();
        const QVector<QByteArray> media =
            TwoPeerFixture::payloadsFrom(fixture.b(), fixture.a(), ProfilePayloadKind::PageMedia);
        QCOMPARE(media.size(), 1);
        const auto decoded = decodePageMedia(media.first());
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->sha256, p.view()->page().background.sha256);
        QVERIFY(bob.contactPage(fixture.a().account)->backgroundPresent);
    }

private:
    // What the importer reports for a file, run on its own.
    std::optional<SongSourceInfo> analyse(const QString &path)
    {
        SongImporter importer;
        std::optional<SongSourceInfo> info;
        bool failed = false;
        connect(&importer, &SongImporter::analysed, this, [&info](const SongSourceInfo &analysed) { info = analysed; });
        connect(&importer, &SongImporter::failed, this, [&failed] { failed = true; });
        importer.analyse(path);
        if (!QTest::qWaitFor([&] { return info.has_value() || failed; }, 60'000))
            qWarning("The reference analysis of %s timed out", qPrintable(path));
        return info;
    }
};

OPENCHAT_PROFILE_QML_TEST_MAIN(ProfileControllerTest, "profile-controller", false)

#include "tst_profilecontroller.moc"

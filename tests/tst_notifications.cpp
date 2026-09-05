#include "notify/NotificationService.h"
#include "render/AvatarPainter.h"

#include <QSignalSpy>
#include <QTest>
#include <QtGlobal>

#include <memory>

// The freedesktop hint encoding is only compiled on the desktops that use it.
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#    define OPENCHAT_TEST_FREEDESKTOP 1
#    include "notify/FreedesktopNotifier.h"
#else
#    define OPENCHAT_TEST_FREEDESKTOP 0
#endif

using OpenChat::AvatarPainter::render;
using OpenChat::Notification;
using OpenChat::NotificationBackend;
using OpenChat::NotificationCategory;
using OpenChat::NotificationService;

namespace {

// A backend that records what the policy decided to post instead of talking to
// a desktop, so every rule below is checked without one.
class RecordingBackend final : public NotificationBackend
{
public:
    struct Posted final {
        QString key;
        QString title;
        QString body;
        bool hasImage = false;
    };

    [[nodiscard]] bool isAvailable() const override { return available; }
    [[nodiscard]] QString name() const override { return QStringLiteral("recording"); }

    void show(const Notification &notification) override
    {
        posted.append(Posted{notification.key, notification.title, notification.body,
                             !notification.image.isNull()});
    }
    void withdraw(const QString &key) override { withdrawn.append(key); }
    void withdrawAll() override { withdrewAll = true; }

    // Stands in for the desktop clicking through to the application.
    void click(const QString &key) { emit activated(key); }

    bool available = true;
    QList<Posted> posted;
    QStringList withdrawn;
    bool withdrewAll = false;
};

// Builds a service over a backend the test keeps a borrowed pointer to.
std::unique_ptr<NotificationService> makeService(RecordingBackend **backendOut)
{
    auto backend = std::make_unique<RecordingBackend>();
    *backendOut = backend.get();
    return std::make_unique<NotificationService>(std::move(backend));
}

} // namespace

class TestNotifications final : public QObject
{
    Q_OBJECT

private slots:
    void announcesAMessageWithSenderBodyAndPicture();
    void suppressesTheConversationTheUserIsReading();
    void announcesAnOpenConversationWhenTheWindowIsNotFocused();
    void announcesOtherConversationsWhileFocused();
    void opensTheConversationThatWasClicked();
    void takesBackTheNotificationWhenTheConversationIsOpened();
    void elidesLongNamesAndMessages();
    void standsInForAMessageWithNoText();
    void replacingAConversationDoesNotSpendTheBurstBudget();
    void collapsesABurstOfConversations();
    void postsNothingWhenTurnedOff();
    void postsNothingWithoutAWorkingDesktop();
#if OPENCHAT_TEST_FREEDESKTOP
    void picturePixelsSurviveTheFreedesktopHint();
#endif
};

void TestNotifications::announcesAMessageWithSenderBodyAndPicture()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    service->postMessage(QStringLiteral("michael-id"), QStringLiteral("Michael"),
                         QStringLiteral("Are we still on for tonight?"),
                         QStringLiteral("michael"));

    QCOMPARE(backend->posted.size(), 1);
    QCOMPARE(backend->posted.at(0).key, QStringLiteral("michael-id"));
    QCOMPARE(backend->posted.at(0).title, QStringLiteral("Michael"));
    QCOMPARE(backend->posted.at(0).body, QStringLiteral("Are we still on for tonight?"));
    QVERIFY(backend->posted.at(0).hasImage);
}

void TestNotifications::suppressesTheConversationTheUserIsReading()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);
    service->setWindowActive(true);
    service->setActiveConversation(QStringLiteral("michael-id"));

    service->postMessage(QStringLiteral("michael-id"), QStringLiteral("Michael"),
                         QStringLiteral("hello"), QStringLiteral("michael"));

    QVERIFY(backend->posted.isEmpty());
}

void TestNotifications::announcesAnOpenConversationWhenTheWindowIsNotFocused()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);
    // The conversation is on screen, but the window is behind something else.
    service->setActiveConversation(QStringLiteral("michael-id"));
    service->setWindowActive(false);

    service->postMessage(QStringLiteral("michael-id"), QStringLiteral("Michael"),
                         QStringLiteral("hello"), QStringLiteral("michael"));

    QCOMPARE(backend->posted.size(), 1);
}

void TestNotifications::announcesOtherConversationsWhileFocused()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);
    service->setWindowActive(true);
    service->setActiveConversation(QStringLiteral("michael-id"));

    service->postMessage(QStringLiteral("jessica-id"), QStringLiteral("Jessica"),
                         QStringLiteral("hello"), QStringLiteral("jessica"));

    QCOMPARE(backend->posted.size(), 1);
    QCOMPARE(backend->posted.at(0).key, QStringLiteral("jessica-id"));
}

void TestNotifications::opensTheConversationThatWasClicked()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);
    QSignalSpy activated(service.get(), &NotificationService::conversationActivated);

    service->postMessage(QStringLiteral("michael-id"), QStringLiteral("Michael"),
                         QStringLiteral("hello"), QStringLiteral("michael"));
    backend->click(QStringLiteral("michael-id"));

    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), QStringLiteral("michael-id"));
}

void TestNotifications::takesBackTheNotificationWhenTheConversationIsOpened()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    service->postMessage(QStringLiteral("michael-id"), QStringLiteral("Michael"),
                         QStringLiteral("hello"), QStringLiteral("michael"));
    QCOMPARE(backend->posted.size(), 1);

    service->setActiveConversation(QStringLiteral("michael-id"));
    QCOMPARE(backend->withdrawn, QStringList{QStringLiteral("michael-id")});

    // Only once: the notification is gone.
    service->setActiveConversation(QStringLiteral("michael-id"));
    QCOMPARE(backend->withdrawn.size(), 1);
}

void TestNotifications::elidesLongNamesAndMessages()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    const QString longName(200, QLatin1Char('n'));
    const QString longBody = QStringLiteral("word ").repeated(200);
    service->postMessage(QStringLiteral("id"), longName, longBody, QStringLiteral("userpfp_none"));

    QCOMPARE(backend->posted.size(), 1);
    const auto &posted = backend->posted.at(0);
    QCOMPARE(posted.title.size(), NotificationService::maxTitleLength);
    QVERIFY(posted.title.endsWith(QChar(0x2026)));
    QVERIFY(posted.body.size() <= NotificationService::maxBodyLength);
    QVERIFY(posted.body.endsWith(QChar(0x2026)));
}

void TestNotifications::standsInForAMessageWithNoText()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    // What the controller sends when the session state withholds plaintext.
    service->postMessage(QStringLiteral("id"), QStringLiteral("Michael"), QString(),
                         QStringLiteral("michael"));

    QCOMPARE(backend->posted.size(), 1);
    QCOMPARE(backend->posted.at(0).title, QStringLiteral("Michael"));
    QVERIFY(!backend->posted.at(0).body.isEmpty());
}

void TestNotifications::replacingAConversationDoesNotSpendTheBurstBudget()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    // One chatty contact, well past the burst limit: every message replaces the
    // one before it and none of them is collapsed into a summary.
    for (int i = 0; i < NotificationService::burstLimit * 3; ++i) {
        service->postMessage(QStringLiteral("michael-id"), QStringLiteral("Michael"),
                             QStringLiteral("message %1").arg(i), QStringLiteral("michael"));
    }

    QCOMPARE(backend->posted.size(), NotificationService::burstLimit * 3);
    for (const auto &posted : backend->posted)
        QCOMPARE(posted.key, QStringLiteral("michael-id"));
}

void TestNotifications::collapsesABurstOfConversations()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    const int total = NotificationService::burstLimit + 3;
    for (int i = 0; i < total; ++i) {
        service->postMessage(QStringLiteral("contact-%1").arg(i), QStringLiteral("Contact %1").arg(i),
                             QStringLiteral("hello"), QStringLiteral("userpfp_none"));
    }

    QCOMPARE(backend->posted.size(), total);
    // The first few are announced as themselves; the rest share one summary
    // that replaces itself.
    for (int i = 0; i < NotificationService::burstLimit; ++i)
        QCOMPARE(backend->posted.at(i).key, QStringLiteral("contact-%1").arg(i));
    const QString summaryKey = backend->posted.at(NotificationService::burstLimit).key;
    for (int i = NotificationService::burstLimit; i < total; ++i)
        QCOMPARE(backend->posted.at(i).key, summaryKey);

    // Clicking the summary opens the most recent chat it stood in for.
    QSignalSpy activated(service.get(), &NotificationService::conversationActivated);
    backend->click(summaryKey);
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toString(),
             QStringLiteral("contact-%1").arg(total - 1));
}

void TestNotifications::postsNothingWhenTurnedOff()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);

    service->postMessage(QStringLiteral("id"), QStringLiteral("Michael"),
                         QStringLiteral("hello"), QStringLiteral("michael"));
    QCOMPARE(backend->posted.size(), 1);

    service->setEnabled(false);
    QVERIFY(backend->withdrewAll);
    service->postMessage(QStringLiteral("id"), QStringLiteral("Michael"),
                         QStringLiteral("hello again"), QStringLiteral("michael"));
    QCOMPARE(backend->posted.size(), 1);
}

void TestNotifications::postsNothingWithoutAWorkingDesktop()
{
    RecordingBackend *backend = nullptr;
    const auto service = makeService(&backend);
    backend->available = false;

    service->postMessage(QStringLiteral("id"), QStringLiteral("Michael"),
                         QStringLiteral("hello"), QStringLiteral("michael"));

    QVERIFY(backend->posted.isEmpty());
    QVERIFY(!service->isAvailable());
}

#if OPENCHAT_TEST_FREEDESKTOP
void TestNotifications::picturePixelsSurviveTheFreedesktopHint()
{
    // The avatar the interface draws is the avatar the desktop receives: same
    // size, straight (non-premultiplied) RGBA, rows packed tight.
    const QImage avatar = render(QStringLiteral("michael"), NotificationService::avatarPixels,
                                 NotificationService::avatarPixels * 0.125);
    QVERIFY(!avatar.isNull());
    QCOMPARE(avatar.width(), NotificationService::avatarPixels);

    const OpenChat::FreedesktopImageData data = OpenChat::FreedesktopNotifier::toImageData(avatar);
    QCOMPARE(data.width, avatar.width());
    QCOMPARE(data.height, avatar.height());
    QCOMPARE(data.channels, 4);
    QCOMPARE(data.bitsPerSample, 8);
    QVERIFY(data.hasAlpha);
    QCOMPARE(data.rowStride, avatar.width() * 4);
    QCOMPARE(data.pixels.size(), qsizetype(data.rowStride) * data.height);

    // A corner is outside the rounded silhouette and must stay transparent;
    // the middle is the picture and must not be.
    const auto alphaAt = [&data](int x, int y) {
        return uchar(data.pixels.at(qsizetype(y) * data.rowStride + qsizetype(x) * 4 + 3));
    };
    QCOMPARE(alphaAt(0, 0), uchar(0));
    QCOMPARE(alphaAt(data.width / 2, data.height / 2), uchar(255));
}
#endif // OPENCHAT_TEST_FREEDESKTOP

QTEST_MAIN(TestNotifications)
#include "tst_notifications.moc"

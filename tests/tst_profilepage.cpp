#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"

#include <QtTest>

using namespace OpenChat;

class ProfilePageTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultPageIsAdaptiveAeroSkyWithDefaultModules()
    {
        const Profile::Page page = Profile::defaultPage();
        QVERIFY(page.theme.adaptive);
        QCOMPARE(page.modules, Profile::defaultModules());
    }
};

QTEST_GUILESS_MAIN(ProfilePageTest)

#include "tst_profilepage.moc"

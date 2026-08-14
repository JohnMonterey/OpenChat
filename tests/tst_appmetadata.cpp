#include <QtTest>

#include "app/AppMetadata.h"

class AppMetadataTest final : public QObject
{
    Q_OBJECT

private slots:
    void approvedIdentity()
    {
        QCOMPARE(OpenChat::AppMetadata::name, QStringView(u"OpenChat"));
        QCOMPARE(OpenChat::AppMetadata::defaultWidth, 860);
        QCOMPARE(OpenChat::AppMetadata::defaultHeight, 680);
        QCOMPARE(OpenChat::AppMetadata::minimumWidth, 720);
        QCOMPARE(OpenChat::AppMetadata::minimumHeight, 560);
    }
};

QTEST_MAIN(AppMetadataTest)

#include "tst_appmetadata.moc"

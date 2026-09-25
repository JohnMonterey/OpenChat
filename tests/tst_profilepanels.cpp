#include "domain/ClipContainer.h"
#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QtTest>

using namespace OpenChat;
using namespace Qt::StringLiterals;
namespace P = OpenChat::Profile;

namespace {

QByteArray hashOf(char byte)
{
    return QByteArray(32, byte);
}

QCborMap bodyOf(const QByteArray &payload)
{
    return QCborValue::fromCbor(payload.mid(1)).toMap();
}

QByteArray tagged(const QCborMap &map)
{
    return QByteArray(1, '\xFF') + QCborValue(map).toCbor();
}

P::MediaRef imageRef(char byte, quint32 bytes = 50'000)
{
    return {hashOf(byte), bytes, 800, 600, 0};
}

P::MediaRef segmentRef(char byte, quint32 bytes = 150'000)
{
    return {hashOf(byte), bytes, 480, 270, 6'000};
}

// A page with one panel of every block kind, already in normal form.
P::Page panelPage()
{
    P::Page page = P::defaultPage();
    P::Panel games;
    games.id = 3;
    games.title = u"Favorite games"_s;
    games.icon = P::PanelIcon::GamepadIcon;
    games.look = {true, 0x112233, 0xFFEEDD, 0xFAFAFA, 0x101010, false};
    P::Block list;
    list.id = 7;
    list.kind = P::BlockKind::ListBlock;
    list.listStyle = P::ListStyle::GameList;
    list.items = {{u"Halo 3"_s, u"Xbox 360"_s, 5, P::GameStatus::AllTimeFavorite, imageRef('\x31')},
                  {u"Tetris"_s, QString(), 0, P::GameStatus::NoGameStatus, {}}};
    games.blocks.push_back(list);

    P::Panel mixed;
    mixed.id = 1;
    mixed.title = u"Stuff"_s;
    P::Block text;
    text.id = 2;
    text.kind = P::BlockKind::TextBlock;
    text.text = u"Hello\n\nworld"_s;
    text.textStyle = P::TextStyle::QuoteText;
    text.align = P::TextAlign::CenterAlign;
    P::Block pictures;
    pictures.id = 4;
    pictures.kind = P::BlockKind::ImageBlock;
    pictures.gallery = P::GalleryStyle::StripGallery;
    pictures.frame = P::ImageFrame::CircleFrame;
    pictures.images = {{imageRef('\x41'), u"Beach"_s}, {imageRef('\x42'), QString()}};
    P::Block video;
    video.id = 5;
    video.kind = P::BlockKind::VideoBlock;
    video.video = {{segmentRef('\x51'), segmentRef('\x52')}, imageRef('\x53')};
    video.loop = true;
    video.caption = u"Skate"_s;
    P::Block divider;
    divider.id = 6;
    divider.kind = P::BlockKind::DividerBlock;
    divider.divider = P::DividerStyle::HeartsDivider;
    mixed.blocks = {text, pictures, video, divider};

    page.panels = {games, mixed};
    page.modules.push_back({P::Module::CustomPanelModule, P::Column::NarrowColumn, false, 1});
    page.modules.push_back({P::Module::CustomPanelModule, P::Column::WideColumn, true, 3});
    return P::normalized(page);
}

ClipContainer smallClip(int channels = 1)
{
    ClipContainer clip;
    clip.width = 320;
    clip.height = 180;
    clip.fps = 10;
    clip.durationMs = 1'000;
    for (int i = 0; i < 10; ++i)
        clip.frames.push_back({i == 0, QByteArray(40, char('a' + i))});
    clip.audioChannels = channels;
    if (channels > 0) {
        clip.audioPreSkip = 312;
        const qint64 packets = (clip.audioSamples() + clip.audioPreSkip + 959) / 960;
        for (qint64 i = 0; i < packets; ++i)
            clip.audioPackets.push_back(QByteArray(30, 'z'));
    }
    return clip;
}

} // namespace

class ProfilePanelsTest final : public QObject
{
    Q_OBJECT

private slots:
    void pageWithoutPanelsHasNoPanelKeys()
    {
        const QCborMap body = bodyOf(encodePageCore(P::defaultPage()));
        QVERIFY(!body.contains(10));
        for (const QCborValue &entry : body.value(5).toMap().value(2).toArray())
            QVERIFY(!entry.toMap().contains(4));
    }

    void panelsRoundTrip()
    {
        const P::Page page = panelPage();
        QCOMPARE(page.panels.size(), 2);
        QCOMPARE(page.panels.at(1).blocks.size(), 4);
        const QByteArray payload = encodePageCore(page);
        const auto decoded = decodePageCore(payload);
        QVERIFY(decoded);
        QVERIFY(*decoded == page);
        QCOMPARE(encodePageCore(*decoded), payload);
        // The placements survived with their panel ids and columns.
        int placed = 0;
        for (const P::ModulePlacement &placement : decoded->modules) {
            if (placement.module != P::Module::CustomPanelModule)
                continue;
            ++placed;
            if (placement.panel == 1) {
                QCOMPARE(placement.column, P::Column::NarrowColumn);
                QCOMPARE(placement.visible, false);
            }
        }
        QCOMPARE(placed, 2);
    }

    void oldClientSeesTheRestOfThePage()
    {
        // What a 0.2.9 decoder keeps: it ignores key 10 and drops layout
        // entries of an unknown module. The rest must be the same page.
        const P::Page page = panelPage();
        QCborMap body = bodyOf(encodePageCore(page));
        body.remove(10);
        QCborMap layout = body.value(5).toMap();
        QCborArray kept;
        for (const QCborValue &entry : layout.value(2).toArray()) {
            if (entry.toMap().value(1).toInteger() != 7)
                kept.append(entry);
        }
        layout[2] = kept;
        body[5] = layout;
        const auto decoded = decodePageCore(tagged(body));
        QVERIFY(decoded);
        P::Page expected = page;
        expected.panels.clear();
        QVERIFY(*decoded == P::normalized(expected));
        QCOMPARE(decoded->modules.size(), 6);
    }

    void normalizeRepairsIdsAndPlacements()
    {
        P::Page page = P::defaultPage();
        P::Panel a, b, c;
        a.id = 5;
        b.id = 5; // repeated
        c.id = 0; // missing
        P::Block x, y;
        x.id = 9;
        y.id = 9;
        a.blocks = {x, y};
        page.panels = {a, b, c};
        page.modules.push_back({P::Module::CustomPanelModule, P::Column::NarrowColumn, true, 5});
        page.modules.push_back({P::Module::CustomPanelModule, P::Column::WideColumn, true, 5}); // repeat
        page.modules.push_back({P::Module::CustomPanelModule, P::Column::WideColumn, true, 77}); // no such panel
        page.modules.push_back({P::Module::HandleModule, P::Column::NarrowColumn, true, 4});   // stray id

        const P::Page clean = P::normalized(page);
        QCOMPARE(clean.panels.at(0).id, quint8(5));
        QCOMPARE(clean.panels.at(1).id, quint8(1));
        QCOMPARE(clean.panels.at(2).id, quint8(2));
        QCOMPARE(clean.panels.at(0).blocks.at(0).id, quint16(9));
        QCOMPARE(clean.panels.at(0).blocks.at(1).id, quint16(1));
        QVector<quint8> placed;
        for (const P::ModulePlacement &placement : clean.modules) {
            if (placement.module == P::Module::CustomPanelModule)
                placed.push_back(placement.panel);
            else
                QCOMPARE(placement.panel, quint8(0));
        }
        QCOMPARE(placed, (QVector<quint8>{5, 1, 2}));
        QVERIFY(P::normalized(clean) == clean);
    }

    void normalizeKeepsOnlyTheBlocksOwnFields()
    {
        P::Page page = P::defaultPage();
        P::Panel panel;
        P::Block text;
        text.kind = P::BlockKind::TextBlock;
        text.text = u"hi"_s;
        text.images = {{imageRef('\x01'), u"x"_s}};
        text.items = {{}};
        text.caption = u"nope"_s;
        P::Block unknown;
        unknown.kind = static_cast<P::BlockKind>(42);
        P::Block badEnums;
        badEnums.kind = P::BlockKind::ListBlock;
        badEnums.listStyle = static_cast<P::ListStyle>(99);
        badEnums.items = {{u"a"_s, {}, 9, static_cast<P::GameStatus>(77), {}}};
        panel.blocks = {text, unknown, badEnums};
        panel.icon = static_cast<P::PanelIcon>(200);
        page.panels = {panel};
        const P::Page clean = P::normalized(page);
        const P::Panel &kept = clean.panels.at(0);
        QCOMPARE(kept.icon, P::PanelIcon::NoPanelIcon);
        QCOMPARE(kept.blocks.size(), 2);
        QVERIFY(kept.blocks.at(0).images.isEmpty());
        QVERIFY(kept.blocks.at(0).items.isEmpty());
        QVERIFY(kept.blocks.at(0).caption.isEmpty());
        QCOMPARE(kept.blocks.at(1).listStyle, P::ListStyle::BulletList);
        QCOMPARE(kept.blocks.at(1).items.at(0).rating, quint8(5));
        QCOMPARE(kept.blocks.at(1).items.at(0).status, P::GameStatus::NoGameStatus); // not a game list
    }

    void normalizeAppliesPageWideBudgets()
    {
        P::Page page = P::defaultPage();
        // Text: 12 panels × a 2000-unit block exceed 16 000 units.
        for (int i = 0; i < 12; ++i) {
            P::Panel panel;
            P::Block block;
            block.text = QString(2000, u'a');
            panel.blocks = {block};
            page.panels.push_back(panel);
        }
        P::Page clean = P::normalized(page);
        QCOMPARE(clean.panels.size(), P::PanelBounds::maxPanels);
        int units = 0;
        for (const P::Panel &panel : clean.panels)
            units += panel.blocks.at(0).text.size();
        QCOMPARE(units, P::PanelBounds::textBudget);
        QVERIFY(clean.panels.at(8).blocks.at(0).text.isEmpty());
        QVERIFY(P::normalized(clean) == clean);

        // Media: 30 pictures ask for more than 24 refs.
        page = P::defaultPage();
        P::Panel pictures;
        for (int b = 0; b < 5; ++b) {
            P::Block block;
            block.kind = P::BlockKind::ImageBlock;
            for (int i = 0; i < 6; ++i)
                block.images.push_back({imageRef(char(b * 6 + i + 1), 10'000), {}});
            pictures.blocks.push_back(block);
        }
        page.panels = {pictures};
        clean = P::normalized(page);
        QCOMPARE(P::mediaRefs(clean).size(), P::PanelBounds::maxMedia);

        // Bytes: twenty 224 KiB pictures exceed 4 MiB.
        for (P::Block &block : page.panels[0].blocks) {
            for (P::PanelImage &image : block.images)
                image.ref.bytes = quint32(maxPanelImageBytes);
        }
        clean = P::normalized(page);
        QCOMPARE(P::mediaRefs(clean).size(), int(P::PanelBounds::maxMediaBytes / maxPanelImageBytes));
        QVERIFY(P::normalized(clean) == clean);
    }

    void clipStandsOrFallsWhole()
    {
        P::Page page = panelPage();
        P::Block &video = page.panels[1].blocks[2];
        QCOMPARE(video.video.durationMs(), quint32(12'000));
        video.video.segments[1].durationMs = P::PanelBounds::maxSegmentDurationMs + 1;
        const P::Page clean = P::normalized(page);
        QVERIFY(clean.panels.at(1).blocks.at(2).video.segments.isEmpty());
        QVERIFY(clean.panels.at(1).blocks.at(2).video.poster.isSet());
    }

    void mediaRefsListEveryBlobOnce()
    {
        P::Page page = panelPage();
        page.panels[1].blocks[1].images[1].ref = imageRef('\x41'); // the same picture twice
        page = P::normalized(page);
        const auto refs = P::mediaRefs(page);
        QVector<P::MediaKind> kinds;
        for (const P::NamedMedia &named : refs)
            kinds.push_back(named.kind);
        using K = P::MediaKind;
        QCOMPARE(kinds, (QVector<K>{K::PanelImageMedia, K::PanelImageMedia, K::PanelImageMedia,
                                    K::VideoSegmentMedia, K::VideoSegmentMedia}));
    }

    void templatesMakeNormalPanels()
    {
        P::Page page = panelPage();
        for (int t = 0; t <= int(P::PanelTemplate::TopListPanel); ++t) {
            const P::Panel panel = P::panelFromTemplate(page, P::PanelTemplate(t));
            QVERIFY(panel.id != 0);
            QCOMPARE(P::panelIndex(page, panel.id), -1);
            QVERIFY(!panel.title.isEmpty());
            QVERIFY(!panel.blocks.isEmpty());
            for (const P::Block &block : panel.blocks)
                QCOMPARE(P::blockIndex(page, block.id).first, -1);
            page.panels.push_back(panel);
            const P::Page clean = P::normalized(page);
            QVERIFY2(clean.panels.last() == panel, qPrintable(P::panelTemplateName(P::PanelTemplate(t))));
            page = clean;
        }
    }

    void largestPanelPageFitsTheCoreCap()
    {
        const auto wide = [](int length) { return QString(length, QChar(0x4E2D)); };
        P::Page page = P::defaultPage();
        page.content.aboutMe = wide(P::TextBounds::aboutMe);
        page.content.meet = wide(P::TextBounds::meet);
        for (int p = 0; p < P::PanelBounds::maxPanels; ++p) {
            P::Panel panel;
            panel.title = wide(P::TextBounds::panelTitle);
            for (int b = 0; b < 4; ++b) {
                P::Block list;
                list.kind = P::BlockKind::ListBlock;
                list.listStyle = P::ListStyle::GameList;
                for (int i = 0; i < 3; ++i)
                    list.items.push_back({wide(80), wide(80), 5, P::GameStatus::PlayingNow,
                                          imageRef(char(p * 12 + b * 3 + i + 1), 1000)});
                panel.blocks.push_back(list);
            }
            page.panels.push_back(panel);
        }
        page = P::normalized(page);
        const QByteArray payload = encodePageCore(page);
        QVERIFY2(payload.size() <= maxPageCoreBytes, qPrintable(QString::number(payload.size())));
        QVERIFY(decodePageCore(payload));
    }

    void decodeToleratesFuturePanelFields()
    {
        QCborMap body = bodyOf(encodePageCore(panelPage()));
        QCborArray panels = body.value(10).toArray();
        QCborMap panel = panels.at(0).toMap();
        panel.insert(40, u"future"_s);
        QCborArray blocks = panel.value(5).toArray();
        blocks.append(QCborMap{{1, 99}, {2, 17}, {50, true}}); // a kind from the future
        panel[5] = blocks;
        panels[0] = panel;
        body[10] = panels;
        const auto decoded = decodePageCore(tagged(body));
        QVERIFY(decoded);
        QVERIFY(*decoded == panelPage());

        // A known key with the wrong type is a malformed core.
        panel[2] = 5;
        panels[0] = panel;
        body[10] = panels;
        QVERIFY(!decodePageCore(tagged(body)));
    }

    void panelMediaKindsAreChecked()
    {
        const QByteArray clip = encodeClipContainer(smallClip());
        QVERIFY(!clip.isEmpty());
        const auto segment = encodePageMedia({P::MediaKind::VideoSegmentMedia, pageMediaHash(clip), clip});
        QCOMPARE(classifyProfilePayload(segment), ProfilePayloadKind::PageMedia);
        QVERIFY(decodePageMedia(segment));

        const QByteArray garbage("OCCLnot really a clip");
        QVERIFY(!decodePageMedia(encodePageMedia({P::MediaKind::VideoSegmentMedia, pageMediaHash(garbage), garbage})));
        // A clip is not a picture.
        QVERIFY(!decodePageMedia(encodePageMedia({P::MediaKind::PanelImageMedia, pageMediaHash(clip), clip})));
    }

    void clipContainerRoundTripsAndRejects()
    {
        for (int channels : {0, 1, 2}) {
            const ClipContainer clip = smallClip(channels);
            const QByteArray bytes = encodeClipContainer(clip);
            QVERIFY(!bytes.isEmpty());
            const auto decoded = decodeClipContainer(bytes);
            QVERIFY(decoded);
            QVERIFY(*decoded == clip);
            QVERIFY(!decodeClipContainer(bytes.left(bytes.size() - 1)));
            QVERIFY(!decodeClipContainer(bytes + 'x'));
        }
        ClipContainer bad = smallClip();
        bad.frames[0].key = false;
        QVERIFY(encodeClipContainer(bad).isEmpty());
        bad = smallClip();
        bad.width = 641;
        QVERIFY(encodeClipContainer(bad).isEmpty());
        bad = smallClip();
        bad.frames.push_back({false, "x"});
        bad.frames.push_back({false, "x"});
        QVERIFY(encodeClipContainer(bad).isEmpty()); // more frames than 1 s at 10 fps allows
        bad = smallClip();
        bad.audioPackets.removeLast();
        QVERIFY(encodeClipContainer(bad).isEmpty());
        bad = smallClip();
        bad.durationMs = ClipContainer::maxDurationMs + 1;
        QVERIFY(encodeClipContainer(bad).isEmpty());

        // A forged frame count is refused before any frame is read.
        QByteArray forged = encodeClipContainer(smallClip());
        forged[18] = char(0xFF);
        forged[19] = char(0xFF);
        QVERIFY(!decodeClipContainer(forged));
    }
};

QTEST_GUILESS_MAIN(ProfilePanelsTest)
#include "tst_profilepanels.moc"

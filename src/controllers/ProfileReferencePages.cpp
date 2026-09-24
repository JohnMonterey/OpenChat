#include "controllers/ProfileReferencePages.h"

#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"

#include <QCryptographicHash>
#include <QHash>

#include <utility>

namespace OpenChat::ProfileReferencePages {

namespace {

using Rows = QList<std::pair<QString, QString>>;

// One mock person's page as the mockups' People.js writes it.
struct Spec final {
    QString id;
    Profile::Preset preset = Profile::Preset::AeroSkyPreset;
    QString displayName;
    QString headline;
    QStringList info;
    QString mood;
    QString songTitle, songArtist;
    Rows interests;
    Rows details;
    QString aboutMe, meet;
    QStringList friends; // labels; a roster member's first name means that contact
};

// A fixed publish time, so every run renders the same page.
constexpr qint64 referenceRevision = 1'790'000'000'000;

// A reference page's song: 45 s of silent Opus. Each 60 ms packet is a lone
// table-of-contents byte (SILK narrowband, 60 ms, one frame) without frame
// data, which a decoder conceals as silence, so the mockups' player shows and
// plays without shipping audio or running the encoder. Pages differ in the
// (inaudible) gain, so no two share a song key and moving between them
// always changes the page's song.
QByteArray silentSong(int variant)
{
    SongContainer song;
    song.channels = 1;
    song.frameSamples = 2880;
    song.preSkip = 312;
    song.totalSamples = qint64(SongContainer::maxDurationMs) * SongContainer::sampleRate / 1000;
    song.gainQ8 = -variant;
    const qint64 packets = (song.preSkip + song.totalSamples + song.frameSamples - 1) / song.frameSamples;
    song.packets.fill(QByteArray(1, char(0x18)), packets);
    return encodeSongContainer(song);
}

Profile::MediaRef songRef(const QByteArray &container)
{
    Profile::MediaRef ref;
    if (const std::optional<SongContainer> song = decodeSongContainer(container)) {
        ref.sha256 = pageMediaHash(container);
        ref.bytes = quint32(container.size());
        ref.durationMs = quint32(song->durationMs());
    }
    return ref;
}

QString slugFor(const QString &label)
{
    QString slug;
    for (const QChar c : label.toLower()) {
        if (c.isLetterOrNumber() && c.unicode() < 0x80)
            slug.append(c);
        else if (!slug.isEmpty() && !slug.endsWith(QLatin1Char('-')))
            slug.append(QLatin1Char('-'));
    }
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    return slug;
}

// A Friend Space label names a mock contact when it is their roster name;
// anyone else is a stranger known by a slug of the label.
QString friendIdFor(const QString &label)
{
    const QString lower = label.toLower();
    return rosterIds().contains(lower) ? lower : slugFor(label);
}

Profile::Mood moodFor(const QString &label)
{
    for (int id = 1; id <= Profile::maxMood; ++id) {
        if (Profile::moodName(Profile::Mood(id)) == label)
            return Profile::Mood(id);
    }
    return Profile::Mood::NoMood;
}

Profile::Zodiac zodiacFor(const QString &name)
{
    for (int id = 1; id <= int(Profile::Zodiac::Pisces); ++id) {
        if (Profile::zodiacName(Profile::Zodiac(id)) == name)
            return Profile::Zodiac(id);
    }
    return Profile::Zodiac::NoZodiac;
}

quint8 hereForBits(const QString &text)
{
    quint8 bits = 0;
    for (int bit = 0; bit < 6; ++bit) {
        const quint8 flag = quint8(1U << bit);
        if (text.split(QStringLiteral(", ")).contains(Profile::hereForText(flag)))
            bits |= flag;
    }
    return bits;
}

Profile::Page build(const Spec &spec, int songVariant)
{
    Profile::Page page = Profile::applyPreset(Profile::defaultPage(), spec.preset);
    page.revision = referenceRevision;
    page.publishedAtMs = referenceRevision;
    Profile::Content &content = page.content;
    content.displayName = spec.displayName;
    content.headline = spec.headline;
    content.infoLines = spec.info;
    content.mood = moodFor(spec.mood);
    page.song = songRef(silentSong(songVariant));
    content.songTitle = spec.songTitle;
    content.songArtist = spec.songArtist;
    content.aboutMe = spec.aboutMe;
    content.meet = spec.meet;
    for (const auto &[label, value] : spec.interests) {
        Profile::Interests &interests = content.interests;
        QString *field = label == QLatin1String("General")      ? &interests.general
                         : label == QLatin1String("Music")      ? &interests.music
                         : label == QLatin1String("Movies")     ? &interests.movies
                         : label == QLatin1String("Television") ? &interests.television
                         : label == QLatin1String("Books")      ? &interests.books
                                                                 : &interests.heroes;
        *field = value;
    }
    for (const auto &[label, value] : spec.details) {
        Profile::Details &details = content.details;
        if (label == QLatin1String("Here for"))
            details.hereFor = hereForBits(value);
        else if (label == QLatin1String("Hometown"))
            details.hometown = value;
        else if (label == QLatin1String("Zodiac sign"))
            details.zodiac = zodiacFor(value);
        else if (label == QLatin1String("Occupation"))
            details.occupation = value;
        else if (label == QLatin1String("Education"))
            details.education = value;
        else if (label == QLatin1String("Languages"))
            details.languages = value;
    }
    for (const QString &label : spec.friends)
        page.topFriends.push_back({mockAccountFor(friendIdFor(label)).bytes(), label});
    return Profile::normalized(page);
}

const QList<Spec> &specs()
{
    static const QList<Spec> list{
        {QStringLiteral("michael"),
         Profile::Preset::AeroSkyPreset,
         QStringLiteral("Michael"),
         QStringLiteral("Shoot film. Drink coffee. Repeat."),
         {QStringLiteral("31 · he/him"), QStringLiteral("Brooklyn, NY"), QStringLiteral("Photographer")},
         QStringLiteral("creative"),
         QStringLiteral("Paper Planes"),
         QStringLiteral("M.I.A."),
         {{QStringLiteral("General"), QStringLiteral("Film cameras, late-night bike rides, cooking (badly)")},
          {QStringLiteral("Music"), QStringLiteral("M.I.A., LCD Soundsystem, The Strokes, Santigold")},
          {QStringLiteral("Movies"), QStringLiteral("Lost in Translation, Heat, Spirited Away")},
          {QStringLiteral("Television"), QStringLiteral("The Office, Top Chef, Planet Earth")},
          {QStringLiteral("Books"), QStringLiteral("The Road, Kafka on the Shore")},
          {QStringLiteral("Heroes"), QStringLiteral("My grandpa, Gordon Parks")}},
         {{QStringLiteral("Here for"), QStringLiteral("Friends, Networking")},
          {QStringLiteral("Hometown"), QStringLiteral("Scranton, PA")},
          {QStringLiteral("Zodiac sign"), QStringLiteral("Scorpio")},
          {QStringLiteral("Occupation"), QStringLiteral("Photographer")},
          {QStringLiteral("Languages"), QStringLiteral("English, Polish")}},
         QStringLiteral("Photographer by day, amateur chef by night. I shoot mostly on film, which means I "
                        "spend way too much on developing. Ask me about my Contax T2 and I will not stop "
                        "talking.\n\nCurrently working on a series about the Q train at 2 a.m."),
         QStringLiteral("People who send long voice notes. Anyone who knows a good dim sum place in Sunset "
                        "Park."),
         {QStringLiteral("Jessica"), QStringLiteral("Alex"), QStringLiteral("Ryan"), QStringLiteral("Tom"),
          QStringLiteral("Dana Whitfield"), QStringLiteral("Priya Nair"), QStringLiteral("Sam Ortiz"),
          QStringLiteral("Kenji Mori")}},
        {QStringLiteral("jessica"),
         Profile::Preset::SceneQueenPreset,
         QStringLiteral("Jessica"),
         QStringLiteral("rawr means i ♥ u in dinosaur x3"),
         {QStringLiteral("19 · she/her"), QStringLiteral("Orlando, FL"), QStringLiteral("taken :]")},
         QStringLiteral("hyper"),
         QStringLiteral("Misery Business"),
         QStringLiteral("Paramore"),
         {{QStringLiteral("General"), QStringLiteral("eyeliner, hair dye, mirror pics, rawr")},
          {QStringLiteral("Music"), QStringLiteral("Paramore, MCR, Panic! at the Disco, BMTH, Cobra Starship")},
          {QStringLiteral("Movies"), QStringLiteral("Nightmare Before Christmas, Juno, Mean Girls")},
          {QStringLiteral("Television"), QStringLiteral("Degrassi, Laguna Beach")},
          {QStringLiteral("Books"), QStringLiteral("Twilight (dont judge)")},
          {QStringLiteral("Heroes"), QStringLiteral("Hayley Williams duh")}},
         {{QStringLiteral("Here for"), QStringLiteral("Friends")},
          {QStringLiteral("Hometown"), QStringLiteral("Orlando, FL")},
          {QStringLiteral("Zodiac sign"), QStringLiteral("Gemini")},
          {QStringLiteral("Occupation"), QStringLiteral("Hot Topic :]")}},
         QStringLiteral("im random, loud & kinda sarcastic. i luv my friends more than anything and i will "
                        "literally fight u for them.\n\nif u dont like me thats ur problem not mine :] xoxo"),
         QStringLiteral("hayley williams!!! ppl who r real. new friends who dont suckk ;]"),
         {QStringLiteral("Michael"), QStringLiteral("Alex"), QStringLiteral("Ryan"), QStringLiteral("Kayla Rawr"),
          QStringLiteral("Brandon Vega"), QStringLiteral("Sk8er Nate"), QStringLiteral("Nicole D"),
          QStringLiteral("Tay Tay")}},
        {QStringLiteral("ryan"),
         Profile::Preset::Classic06Preset,
         QStringLiteral("Ryan"),
         QStringLiteral("Still waiting for my flying car."),
         {QStringLiteral("34 · he/him"), QStringLiteral("San Jose, CA"), QStringLiteral("Hardware tinkerer")},
         QStringLiteral("geeky"),
         QStringLiteral("Harder, Better, Faster, Stronger"),
         QStringLiteral("Daft Punk"),
         {{QStringLiteral("General"), QStringLiteral("Retro computers, synthwave, LAN parties")},
          {QStringLiteral("Music"), QStringLiteral("Daft Punk, The Prodigy, Kavinsky, Chemical Brothers")},
          {QStringLiteral("Movies"), QStringLiteral("The Matrix, Tron, Hackers, Ghost in the Shell")},
          {QStringLiteral("Television"), QStringLiteral("Mr. Robot, Cowboy Bebop")},
          {QStringLiteral("Books"), QStringLiteral("Neuromancer, Snow Crash")},
          {QStringLiteral("Heroes"), QStringLiteral("Steve Wozniak")}},
         {{QStringLiteral("Here for"), QStringLiteral("Friends, Networking")},
          {QStringLiteral("Hometown"), QStringLiteral("Fresno, CA")},
          {QStringLiteral("Zodiac sign"), QStringLiteral("Capricorn")},
          {QStringLiteral("Occupation"), QStringLiteral("Electrical engineer")},
          {QStringLiteral("Languages"), QStringLiteral("English, C, a little Rust")}},
         QStringLiteral("I fix computers nobody asked me to fix. My apartment has more CRTs than chairs.\n\n"
                        "If you have a Dreamcast you're not using, we should talk."),
         QStringLiteral("Anyone who can beat me at Quake III. (Nobody.)"),
         {QStringLiteral("Michael"), QStringLiteral("Alex"), QStringLiteral("Jessica"), QStringLiteral("Neo Tanaka"),
          QStringLiteral("Grace Liu"), QStringLiteral("Pixel Pete"), QStringLiteral("Dana Whitfield"),
          QStringLiteral("Zero Cool")}},
        {QStringLiteral("sarah"),
         Profile::Preset::GlitterGirlPreset,
         QStringLiteral("Sarah"),
         QStringLiteral("glitter is my love language"),
         {QStringLiteral("22 · she/her"), QStringLiteral("San Diego, CA"),
          QStringLiteral("nail tech + part-time mermaid")},
         QStringLiteral("bubbly"),
         QStringLiteral("Circus"),
         QStringLiteral("Britney Spears"),
         {{QStringLiteral("General"), QStringLiteral("nail art, thrifting, beach days, Sanrio everything")},
          {QStringLiteral("Music"), QStringLiteral("Britney, Lady Gaga, Ke$ha, Katy Perry")},
          {QStringLiteral("Movies"), QStringLiteral("Clueless, Legally Blonde, Mean Girls")},
          {QStringLiteral("Television"), QStringLiteral("The Hills, Gossip Girl")},
          {QStringLiteral("Heroes"), QStringLiteral("my mom & Elle Woods")}},
         {{QStringLiteral("Here for"), QStringLiteral("Friends")},
          {QStringLiteral("Hometown"), QStringLiteral("San Diego, CA")},
          {QStringLiteral("Zodiac sign"), QStringLiteral("Leo")},
          {QStringLiteral("Occupation"), QStringLiteral("Nail technician")}},
         QStringLiteral("hiii!! i'm sarah. i do nails, i love my cat Biscuit and i will always pick the pinkest "
                        "option.\n\ni reply to every message eventually <3"),
         QStringLiteral("people who send cute pics of their pets. and Elle Woods."),
         {QStringLiteral("Jessica"), QStringLiteral("Michael"), QStringLiteral("Biscuit the Cat"),
          QStringLiteral("Ashley M"), QStringLiteral("Brit Lopez"), QStringLiteral("Kels"), QStringLiteral("Mia Chen"),
          QStringLiteral("Tay Tay")}},
        {QStringLiteral("alex"),
         Profile::Preset::MidnightEmoPreset,
         QStringLiteral("Alex"),
         QStringLiteral("the best things in life are sad songs at 2am"),
         {QStringLiteral("24 · he/him"), QStringLiteral("Austin, TX"),
          QStringLiteral("bassist in a band u never heard of")},
         QStringLiteral("melancholy"),
         QStringLiteral("The Ghost of You"),
         QStringLiteral("My Chemical Romance"),
         {{QStringLiteral("General"), QStringLiteral("black nail polish, coffee, thrift-store cardigans")},
          {QStringLiteral("Music"), QStringLiteral("MCR, Taking Back Sunday, Brand New, Thursday")},
          {QStringLiteral("Movies"), QStringLiteral("Donnie Darko, Eternal Sunshine")},
          {QStringLiteral("Books"), QStringLiteral("The Perks of Being a Wallflower")},
          {QStringLiteral("Heroes"), QStringLiteral("Gerard Way")}},
         {{QStringLiteral("Here for"), QStringLiteral("Friends")},
          {QStringLiteral("Hometown"), QStringLiteral("El Paso, TX")},
          {QStringLiteral("Zodiac sign"), QStringLiteral("Pisces")}},
         QStringLiteral("i play bass, i write sad lyrics, and i drink way too much coffee.\n\nif u like the same "
                        "bands we are already friends."),
         QStringLiteral("someone to share headphones with on a long bus ride."),
         {QStringLiteral("Jessica"), QStringLiteral("Ryan"), QStringLiteral("Mara Voss"), QStringLiteral("Theo Park"),
          QStringLiteral("Michael"), QStringLiteral("Omar Haddad"), QStringLiteral("Lina Q"),
          QStringLiteral("Ghost Kid")}},
    };
    return list;
}

const Spec &danielSpec()
{
    static const Spec daniel{
        selfId(),
        Profile::Preset::HeadlinerPreset,
        QStringLiteral("Daniel"),
        QStringLiteral("Music is the answer."),
        {QStringLiteral("27 · he/him"), QStringLiteral("Seattle, WA"), QStringLiteral("Bassist, The Night Shift")},
        QStringLiteral("rockin'"),
        QStringLiteral("Such Great Heights"),
        QStringLiteral("The Postal Service"),
        {{QStringLiteral("General"), QStringLiteral("Playing bass, record stores, late shows")},
         {QStringLiteral("Music"), QStringLiteral("The Postal Service, Death Cab for Cutie, Interpol")},
         {QStringLiteral("Movies"), QStringLiteral("Almost Famous, High Fidelity")},
         {QStringLiteral("Heroes"), QStringLiteral("Tom, obviously")}},
        {{QStringLiteral("Here for"), QStringLiteral("Friends, Networking")},
         {QStringLiteral("Hometown"), QStringLiteral("Tacoma, WA")},
         {QStringLiteral("Zodiac sign"), QStringLiteral("Virgo")},
         {QStringLiteral("Occupation"), QStringLiteral("Musician / developer")}},
        QStringLiteral("Bass player in The Night Shift. We play the Crocodile most Fridays: come say hi.\n\n"
                       "When I'm not on stage I'm digging through the dollar bin."),
        QStringLiteral("Drummers who can keep time. Promoters. Anyone with a van."),
        {QStringLiteral("Michael"), QStringLiteral("Jessica"), QStringLiteral("Alex"), QStringLiteral("Ryan"),
         QStringLiteral("Sarah"), QStringLiteral("Tom")}};
    return daniel;
}

// Every id this file can name, by its mock account.
const QHash<QByteArray, QString> &knownAccounts()
{
    static const QHash<QByteArray, QString> accounts = [] {
        QHash<QByteArray, QString> known;
        const auto add = [&known](const QString &id) { known.insert(mockAccountFor(id).bytes(), id); };
        add(selfId());
        for (const QString &id : rosterIds())
            add(id);
        for (const Spec &spec : specs()) {
            for (const QString &label : spec.friends)
                add(friendIdFor(label));
        }
        for (const QString &label : danielSpec().friends)
            add(friendIdFor(label));
        return known;
    }();
    return accounts;
}

} // namespace

QString selfId()
{
    return QStringLiteral("self");
}

AccountId mockAccountFor(const QString &id)
{
    const QByteArray digest = QCryptographicHash::hash(QByteArrayLiteral("openchat-mock:") + id.toUtf8(),
                                                       QCryptographicHash::Sha256);
    // Sixteen bytes of a SHA-256 are never all zero in practice; the fallback
    // only keeps this total.
    return AccountId::fromBytes(digest.left(AccountId::byteCount))
        .value_or(*AccountId::fromBytes(QByteArray(AccountId::byteCount, '\x01')));
}

QString mockIdFor(const AccountId &account)
{
    return knownAccounts().value(account.bytes());
}

QStringList rosterIds()
{
    return {QStringLiteral("michael"), QStringLiteral("sarah"), QStringLiteral("alex"),
            QStringLiteral("jessica"), QStringLiteral("ryan"),  QStringLiteral("tom")};
}

std::optional<Profile::Page> seededPage(const QString &contactId)
{
    for (qsizetype index = 0; index < specs().size(); ++index) {
        if (specs().at(index).id == contactId)
            return build(specs().at(index), int(index) + 1);
    }
    return std::nullopt;
}

QByteArray referenceMedia(const QByteArray &sha256)
{
    // Daniel's song is variant 0, the seeded pages' 1 onwards.
    static const QHash<QByteArray, QByteArray> songs = [] {
        QHash<QByteArray, QByteArray> byHash;
        for (int variant = 0; variant <= int(specs().size()); ++variant) {
            const QByteArray song = silentSong(variant);
            byHash.insert(pageMediaHash(song), song);
        }
        return byHash;
    }();
    return songs.value(sha256);
}

QString contactForPreset(Profile::Preset preset)
{
    for (const Spec &spec : specs()) {
        if (spec.preset == preset)
            return spec.id;
    }
    return {};
}

Profile::Page ownReferencePage()
{
    return build(danielSpec(), 0);
}

} // namespace OpenChat::ProfileReferencePages

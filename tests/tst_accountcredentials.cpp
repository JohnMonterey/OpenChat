#include <QtTest>

#include "domain/Handle.h"
#include "security/PasswordKey.h"

using namespace OpenChat;

namespace {

QByteArray keyFor(const QString &handle, const QString &password)
{
  auto key = derivePasswordKey(handle, password);
  return key.hasValue() ? key.value().view().toByteArray() : QByteArray();
}

} // namespace

// The two rules every account rests on: what a username is, and how a password
// becomes the key that is sent in its place.
class AccountCredentialsTest final : public QObject {
  Q_OBJECT

private slots:
  void handlesNormalizeToOneCanonicalForm_data()
  {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("canonical"); // empty = rejected

    QTest::newRow("already canonical") << QStringLiteral("alice") << QStringLiteral("alice");
    QTest::newRow("capitals fold") << QStringLiteral("ALiCe") << QStringLiteral("alice");
    QTest::newRow("at sign and padding") << QStringLiteral("  @alice\t") << QStringLiteral("alice");
    QTest::newRow("every allowed symbol") << QStringLiteral("a.b_c-9") << QStringLiteral("a.b_c-9");
    QTest::newRow("digit first") << QStringLiteral("9lives") << QStringLiteral("9lives");
    QTest::newRow("shortest") << QStringLiteral("abc") << QStringLiteral("abc");
    QTest::newRow("longest") << QString(32, QLatin1Char('a')) << QString(32, QLatin1Char('a'));

    QTest::newRow("empty") << QString() << QString();
    QTest::newRow("only an at sign") << QStringLiteral("@") << QString();
    QTest::newRow("two characters") << QStringLiteral("ab") << QString();
    QTest::newRow("thirty-three characters") << QString(33, QLatin1Char('a')) << QString();
    QTest::newRow("second at sign") << QStringLiteral("@@alice") << QString();
    QTest::newRow("inner whitespace") << QStringLiteral("al ice") << QString();
    QTest::newRow("symbol first") << QStringLiteral("_alice") << QString();
    QTest::newRow("other punctuation") << QStringLiteral("alice!") << QString();
    QTest::newRow("path characters") << QStringLiteral("../alice") << QString();
    QTest::newRow("sql metacharacters") << QStringLiteral("a';drop--") << QString();
    // Look-alikes of ASCII letters: refusing every non-ASCII character is what
    // makes two distinct handles impossible to mistake for one another.
    QTest::newRow("latin with diaeresis") << QString::fromUtf8("\xC3\xA4lice") << QString();
    QTest::newRow("cyrillic a") << QString::fromUtf8("\xD0\xB0lice") << QString();
    QTest::newRow("fullwidth a") << QString::fromUtf8("\xEF\xBD\x81lice") << QString();
    QTest::newRow("turkish dotted capital I") << QString::fromUtf8("al\xC4\xB0" "ce") << QString();
    QTest::newRow("zero-width joiner") << QString::fromUtf8("ali\xE2\x80\x8D" "ce") << QString();
  }

  void handlesNormalizeToOneCanonicalForm()
  {
    QFETCH(QString, input);
    QFETCH(QString, canonical);

    const std::optional<QString> normalized = normalizeHandle(input);
    QCOMPARE(normalized.has_value(), !canonical.isEmpty());
    if (normalized) {
      QCOMPARE(*normalized, canonical);
      QVERIFY(isCanonicalHandle(*normalized));
      // Normalizing is idempotent: a stored handle always looks itself up.
      QCOMPARE(normalizeHandle(*normalized), normalized);
    }
  }

  void passwordKeyMatchesThePinnedVector()
  {
    // Computed independently with the OpenSSL command line:
    //   salt = SHA-256("OpenChat password key v1" || 0x00 || "alice")
    //   openssl kdf -keylen 32 -kdfopt pass:... -kdfopt hexsalt:<salt> \
    //       -kdfopt iter:3 -kdfopt memcost:65536 -kdfopt lanes:1 ARGON2ID
    // This value IS login protocol version 1. If this test fails, a parameter
    // changed and every existing account's password has stopped working: add a
    // new version instead of editing this one.
    QCOMPARE(passwordKeyVersion, 1);
    QCOMPARE(keyFor(QStringLiteral("alice"), QStringLiteral("correct horse battery staple")).toHex(),
             QByteArray("061eae54f4a23c8ee3fa7cea58de99f75c3c671e8b261aa806d1562ae84b683f"));
  }

  void passwordKeyIsBoundToTheAccount()
  {
    const QString password = QStringLiteral("correct horse battery staple");
    const QByteArray alice = keyFor(QStringLiteral("alice"), password);
    QCOMPARE(alice.size(), passwordKeyBytes);

    // However the username is typed, it is the same account and the same key...
    QCOMPARE(keyFor(QStringLiteral("  @ALICE "), password), alice);
    // ...while the same password under another username is unrelated, so a key
    // captured for one account is useless against any other.
    QVERIFY(keyFor(QStringLiteral("alicf"), password) != alice);
    QVERIFY(keyFor(QStringLiteral("alice"), password + QLatin1Char(' ')) != alice);
    // Passwords, unlike usernames, are case-sensitive and never trimmed.
    QVERIFY(keyFor(QStringLiteral("alice"), password.toUpper()) != alice);
  }

  void passwordKeyIsTheSameOnEveryKeyboard()
  {
    // "é" typed as one code point on one machine and as e + combining acute on
    // another must be the same password.
    const QString composed = QString::fromUtf8("caf\xC3\xA9-au-lait-2026");
    const QString decomposed = QString::fromUtf8("cafe\xCC\x81-au-lait-2026");
    QVERIFY(composed != decomposed);
    QCOMPARE(keyFor(QStringLiteral("alice"), composed),
             keyFor(QStringLiteral("alice"), decomposed));
  }

  void passwordKeyRejectsUnusableInput()
  {
    const auto badHandle = derivePasswordKey(QStringLiteral("a b"), QStringLiteral("whatever-long"));
    QVERIFY(!badHandle.hasValue());
    QCOMPARE(badHandle.error(), PasswordKeyError::InvalidHandle);

    const auto empty = derivePasswordKey(QStringLiteral("alice"), QString());
    QVERIFY(!empty.hasValue());
    QCOMPARE(empty.error(), PasswordKeyError::InvalidPassword);

    const auto huge = derivePasswordKey(QStringLiteral("alice"),
                                        QString(maximumPasswordLength + 1, QLatin1Char('x')));
    QVERIFY(!huge.hasValue());
    QCOMPARE(huge.error(), PasswordKeyError::InvalidPassword);

    // Deriving is allowed for any non-empty password: an existing account's
    // password may be shorter than today's sign-up minimum.
    QVERIFY(derivePasswordKey(QStringLiteral("alice"), QStringLiteral("old")).hasValue());
  }
};

QTEST_GUILESS_MAIN(AccountCredentialsTest)

#include "tst_accountcredentials.moc"

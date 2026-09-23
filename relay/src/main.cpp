#include "AuthService.h"
#include "CosmeticsService.h"
#include "DirectoryService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"
#include "RelayServer.h"
#include "UdpMediaService.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QTimer>

#include <cstdio>

// openchat-relay: a ciphertext-only relay. Configuration comes from the
// environment so no secrets appear on the command line. TLS is terminated by a
// reverse proxy in front of this process; the relay listens plain HTTP/WS on a
// loopback interface.
//
// Environment:
//   OPENCHAT_RELAY_PG_HOST      (default 127.0.0.1)
//   OPENCHAT_RELAY_PG_PORT      (default 5432)
//   OPENCHAT_RELAY_PG_USER
//   OPENCHAT_RELAY_PG_PASSWORD
//   OPENCHAT_RELAY_PG_DATABASE
//   OPENCHAT_RELAY_BIND         (default 127.0.0.1)
//   OPENCHAT_RELAY_PORT         (default 8443)
//   OPENCHAT_RELAY_MEDIA_BIND   (default off)
//   OPENCHAT_RELAY_MEDIA_PORT   (default 8444)
//
// Operator command, run against the same database and then exiting:
//   openchat-relay grant-cases <count> --all        every account
//   openchat-relay grant-cases <count> <handle>     one account
// adds <count> waiting cases (1-1000). Connected clients see them the next
// time they ask (opening the case popup, or reconnecting).
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    using namespace OpenChat::Relay;

    const QStringList args = QCoreApplication::arguments();
    const bool grantCommand = args.size() >= 2 && args.at(1) == QLatin1String("grant-cases");
    if (grantCommand && args.size() != 4) {
        std::fprintf(stderr, "usage: openchat-relay grant-cases <count> (--all | <handle>)\n");
        return 2;
    }

    PostgresStore::Config config;
    config.host = qEnvironmentVariable("OPENCHAT_RELAY_PG_HOST", QStringLiteral("127.0.0.1"));
    config.port = qEnvironmentVariableIntValue("OPENCHAT_RELAY_PG_PORT");
    if (config.port == 0)
        config.port = 5432;
    config.user = qEnvironmentVariable("OPENCHAT_RELAY_PG_USER");
    config.password = qEnvironmentVariable("OPENCHAT_RELAY_PG_PASSWORD");
    config.database = qEnvironmentVariable("OPENCHAT_RELAY_PG_DATABASE");

    if (config.user.isEmpty() || config.database.isEmpty()) {
        qCritical("OPENCHAT_RELAY_PG_USER and OPENCHAT_RELAY_PG_DATABASE are required");
        return 2;
    }

    QString error;
    auto store = PostgresStore::open(config, QStringLiteral("relay_main"), &error);
    if (!store) {
        qCritical("database connection failed"); // never log the error text (may leak DSN)
        return 3;
    }

    const QStringList migrations{QStringLiteral(":/relay/001_accounts_devices.sql"),
                                 QStringLiteral(":/relay/002_tokens_keypackages.sql"),
                                 QStringLiteral(":/relay/003_inboxes_attachments.sql"),
                                 QStringLiteral(":/relay/004_invites.sql"),
                                 QStringLiteral(":/relay/005_envelope_acceptances.sql"),
                                 QStringLiteral(":/relay/006_account_passwords.sql"),
                                 QStringLiteral(":/relay/007_cosmetics.sql")};
    if (!store->applyMigrations(migrations, &error)) {
        qCritical("migration failed");
        return 4;
    }

    // Account passwords depend on OpenSSL's Argon2id (3.2+). Prove it works before
    // accepting traffic, so a build against an older library stops here instead
    // of failing every sign-up and login at request time.
    if (hashPasswordKey(QByteArray(passwordKeyBytes, 'k'), QByteArray(passwordSaltBytes, 's'),
                        PasswordHashParams{})
            .size()
        != passwordHashBytes) {
        qCritical("Argon2id is unavailable in the linked OpenSSL; cannot protect passwords");
        return 6;
    }

    AuthService auth(*store);
    EnvelopeService envelopes(*store);
    KeyPackageService keyPackages(*store);
    DirectoryService directory(*store);
    CosmeticsService cosmetics(*store);

    if (grantCommand) {
        bool numeric = false;
        const int count = args.at(2).toInt(&numeric);
        std::optional<OpenChat::AccountId> account;
        if (args.at(3) != QLatin1String("--all")) {
            const auto resolved = directory.resolveHandle(args.at(3));
            if (!resolved.hasValue()) {
                std::fprintf(stderr, "no account has that handle\n");
                return 7;
            }
            account = resolved.value().accountId;
        }
        const auto granted = numeric ? cosmetics.grantDrops(count, account)
                                     : OpenChat::Result<int, RelayError>::failure(RelayError::InvalidRequest);
        if (!granted.hasValue()) {
            std::fprintf(stderr, "grant failed: the count must be 1-1000\n");
            return 8;
        }
        std::printf("granted %d case(s) to %d account(s)\n", count, granted.value());
        return 0;
    }

    RelayServer server(*store, auth, envelopes, keyPackages, directory);
    server.setCosmetics(&cosmetics);

    // Inboxes hold envelopes for devices that are offline, so storage no longer
    // drains just because recipients connect: sweep what has expired or can no
    // longer be fetched, once now and then hourly.
    const auto sweep = [&envelopes] {
        const auto pruned = envelopes.pruneExpired();
        if (!pruned.hasValue())
            qWarning("inbox retention sweep failed");
    };
    sweep();
    QTimer retention;
    retention.setInterval(60 * 60 * 1000);
    QObject::connect(&retention, &QTimer::timeout, &app, sweep);
    retention.start();

    const QString bindAddress =
        qEnvironmentVariable("OPENCHAT_RELAY_BIND", QStringLiteral("127.0.0.1"));
    int port = qEnvironmentVariableIntValue("OPENCHAT_RELAY_PORT");
    if (port == 0)
        port = 8443;

    const quint16 bound = server.start(QHostAddress(bindAddress), static_cast<quint16>(port));
    if (bound == 0) {
        qCritical("failed to bind relay listener");
        return 5;
    }
    qInfo("openchat-relay listening on %s:%u", qUtf8Printable(bindAddress), bound);
    if (server.udpMediaService() && server.udpMediaService()->isRunning()) {
        qInfo("openchat-relay UDP media listening on port %u", server.udpMediaService()->localPort());
    }

    return app.exec();
}

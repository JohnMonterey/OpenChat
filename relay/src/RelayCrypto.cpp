#include "RelayCrypto.h"

#include "protocol/CanonicalCborCodec.h"

#include <QCryptographicHash>
#include <QList>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

namespace OpenChat::Relay {

namespace {

void appendLengthBigEndian(QByteArray &output, qsizetype size)
{
    const quint32 value = static_cast<quint32>(size);
    output.append(static_cast<char>((value >> 24) & 0xff));
    output.append(static_cast<char>((value >> 16) & 0xff));
    output.append(static_cast<char>((value >> 8) & 0xff));
    output.append(static_cast<char>(value & 0xff));
}

} // namespace

bool verifyEd25519(QByteArrayView pubKey, QByteArrayView message, QByteArrayView signature)
{
    if (pubKey.size() != 32 || signature.size() != 64)
        return false;

    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(pubKey.data()), 32);
    if (!key)
        return false;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        ok = EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.data()),
                              static_cast<std::size_t>(signature.size()),
                              reinterpret_cast<const unsigned char *>(message.data()),
                              static_cast<std::size_t>(message.size()))
             == 1;
    }
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

QByteArray sha256(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray randomBytes(int count)
{
    if (count <= 0)
        return {};
    QByteArray buffer(count, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(buffer.data()), count) != 1)
        return {};
    return buffer;
}

QByteArray PasswordHashParams::serialize() const
{
    return QByteArrayLiteral("argon2id$m=") + QByteArray::number(memoryKiB)
        + QByteArrayLiteral(",t=") + QByteArray::number(iterations);
}

std::optional<PasswordHashParams> PasswordHashParams::parse(QByteArrayView text)
{
    const QByteArray prefix = QByteArrayLiteral("argon2id$m=");
    const QByteArray value = text.toByteArray();
    if (!value.startsWith(prefix))
        return std::nullopt;
    const QList<QByteArray> parts = value.mid(prefix.size()).split(',');
    if (parts.size() != 2 || !parts.at(1).startsWith("t="))
        return std::nullopt;
    bool memoryOk = false;
    bool iterationsOk = false;
    PasswordHashParams params;
    params.memoryKiB = parts.at(0).toUInt(&memoryOk);
    params.iterations = parts.at(1).mid(2).toUInt(&iterationsOk);
    // Bounded on both sides: a corrupted or hostile row must not be able to ask
    // the relay for an unbounded allocation or an effectively free hash.
    if (!memoryOk || !iterationsOk || params.memoryKiB < 8 * 1024
        || params.memoryKiB > 1024 * 1024 || params.iterations < 1 || params.iterations > 16)
        return std::nullopt;
    return params;
}

QByteArray hashPasswordKey(QByteArrayView passwordKey, QByteArrayView salt,
                           const PasswordHashParams &params)
{
    if (passwordKey.size() != passwordKeyBytes || salt.size() != passwordSaltBytes)
        return {};

    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!kdf)
        return {};
    EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx)
        return {};

    // A single lane on the calling thread: the relay is single-threaded and must
    // not depend on OpenSSL's optional thread pool being configured.
    quint32 memory = params.memoryKiB;
    quint32 iterations = params.iterations;
    quint32 lanes = 1;
    QByteArray keyCopy = passwordKey.toByteArray();
    QByteArray saltCopy = salt.toByteArray();
    OSSL_PARAM kdfParams[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, keyCopy.data(),
                                          static_cast<std::size_t>(keyCopy.size())),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, saltCopy.data(),
                                          static_cast<std::size_t>(saltCopy.size())),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_end(),
    };

    QByteArray hash(passwordHashBytes, Qt::Uninitialized);
    const bool ok = EVP_KDF_derive(ctx, reinterpret_cast<unsigned char *>(hash.data()),
                                   static_cast<std::size_t>(hash.size()), kdfParams)
        == 1;
    EVP_KDF_CTX_free(ctx);
    OPENSSL_cleanse(keyCopy.data(), static_cast<std::size_t>(keyCopy.size()));
    return ok ? hash : QByteArray();
}

bool constantTimeEquals(QByteArrayView left, QByteArrayView right)
{
    if (left.size() != right.size() || left.isEmpty())
        return false;
    return CRYPTO_memcmp(left.data(), right.data(), static_cast<std::size_t>(left.size())) == 0;
}

QByteArray challengeSigningMessage(QByteArrayView challenge, QByteArrayView context)
{
    QByteArray message("OpenChat device challenge v1", 28);
    appendLengthBigEndian(message, context.size());
    message.append(context.data(), context.size());
    appendLengthBigEndian(message, challenge.size());
    message.append(challenge.data(), challenge.size());
    return message;
}

QByteArray envelopeSigningInput(const CiphertextEnvelopeV1 &envelope)
{
    // Delegate to the shared codec definition so the relay's verifier and the
    // client's signer can never diverge on what bytes are covered.
    return encodeForSignature(envelope);
}

} // namespace OpenChat::Relay

#include "RelayCrypto.h"

#include "protocol/CanonicalCborCodec.h"

#include <QCryptographicHash>

#include <openssl/evp.h>
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
    CiphertextEnvelopeV1 copy = envelope;
    copy.senderSignature.clear();
    return encodeCanonical(copy);
}

} // namespace OpenChat::Relay

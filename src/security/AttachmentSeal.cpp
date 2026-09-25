#include "security/AttachmentSeal.h"

#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

namespace OpenChat {
namespace {

constexpr qsizetype nonceBytes = 12;
constexpr qsizetype tagBytes = AttachmentLimits::sealOverhead;
static_assert(tagBytes == 16, "AES-GCM's full tag");

using CipherContextPointer = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// 00 00 00 00 ‖ u32 type ‖ u32 index. A key belongs to one attachment and
// each (type, index) is sealed under it once (a part sent again is the same
// sealed bytes, never a new encryption of them), so no nonce repeats.
[[nodiscard]] QByteArray frameNonce(quint8 type, quint32 index)
{
    QByteArray nonce(nonceBytes, '\0');
    qToBigEndian<quint32>(type, nonce.data() + 4);
    qToBigEndian<quint32>(index, nonce.data() + 8);
    return nonce;
}

[[nodiscard]] const unsigned char *bytes(QByteArrayView view)
{
    return reinterpret_cast<const unsigned char *>(view.data());
}

// Whether splitAttachmentFrame would take a frame with this header: sealing
// one it would refuse only wastes a send.
[[nodiscard]] bool isSendableHeader(AttachmentFrameType type, quint32 index)
{
    switch (type) {
    case AttachmentFrameType::Part:
        return index < quint32(AttachmentLimits::maxParts);
    case AttachmentFrameType::Preview:
    case AttachmentFrameType::Request:
    case AttachmentFrameType::Cancel:
        return index == 0;
    }
    return false;
}

} // namespace

QByteArray randomAttachmentKey()
{
    QByteArray key(AttachmentLimits::keyBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(key.data()), static_cast<int>(key.size())) != 1)
        return {};
    return key;
}

QByteArray sealAttachmentFrame(const QByteArray &key, AttachmentFrameType type,
                               const AttachmentId &attachmentId, quint32 index,
                               QByteArrayView plaintext)
{
    if (key.size() != AttachmentLimits::keyBytes || !isSendableHeader(type, index)
        || attachmentFrameHeaderBytes + plaintext.size() + tagBytes > AttachmentLimits::maxFrameBytes)
        return {};

    const QByteArray header = attachmentFrameHeader(type, attachmentId, index);
    const QByteArray nonce = frameNonce(quint8(type), index);
    const int length = int(plaintext.size());
    QByteArray frame = header;
    frame.resize(header.size() + length + tagBytes);
    auto *out = reinterpret_cast<unsigned char *>(frame.data()) + header.size();

    CipherContextPointer context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    EVP_CIPHER_CTX *cipher = context.get();
    int written = 0;
    int total = 0;
    if (!cipher || EVP_EncryptInit_ex(cipher, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_IVLEN, int(nonce.size()), nullptr) != 1
        || EVP_EncryptInit_ex(cipher, nullptr, nullptr, bytes(key), bytes(nonce)) != 1
        || EVP_EncryptUpdate(cipher, nullptr, &written, bytes(header), int(header.size())) != 1)
        return {};
    // A Cancel has no body; GCM then authenticates the header alone.
    if (length > 0) {
        if (EVP_EncryptUpdate(cipher, out, &written, bytes(plaintext), length) != 1)
            return {};
        total = written;
    }
    if (EVP_EncryptFinal_ex(cipher, out + total, &written) != 1 || total + written != length
        || EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, int(tagBytes), out + length) != 1)
        return {};
    return frame;
}

std::optional<QByteArray> openAttachmentFrame(const QByteArray &key, QByteArrayView frame)
{
    if (!splitAttachmentFrame(frame))
        return std::nullopt;
    return openAttachmentBody(key, frame.first(attachmentFrameHeaderBytes),
                              frame.sliced(attachmentFrameHeaderBytes));
}

std::optional<QByteArray> openAttachmentBody(const QByteArray &key, QByteArrayView header,
                                             QByteArrayView sealedBody)
{
    // Bounded like a whole frame, so every length below fits an int.
    if (key.size() != AttachmentLimits::keyBytes || header.size() != attachmentFrameHeaderBytes
        || sealedBody.size() < tagBytes
        || header.size() + sealedBody.size() > AttachmentLimits::maxFrameBytes)
        return std::nullopt;

    // The nonce is rebuilt from the header, which is also the associated
    // data: a body presented under any other header fails the tag.
    const quint8 type = quint8(header[2]);
    const quint32 index = qFromBigEndian<quint32>(header.data() + 19);
    const QByteArray nonce = frameNonce(type, index);
    const QByteArrayView ciphertext = sealedBody.first(sealedBody.size() - tagBytes);
    const int length = int(ciphertext.size());
    QByteArray tag = sealedBody.last(tagBytes).toByteArray();
    QByteArray plaintext(length, Qt::Uninitialized);
    auto *out = reinterpret_cast<unsigned char *>(plaintext.data());

    CipherContextPointer context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    EVP_CIPHER_CTX *cipher = context.get();
    int written = 0;
    int total = 0;
    bool ok = cipher
              && EVP_DecryptInit_ex(cipher, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
              && EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_IVLEN, int(nonce.size()), nullptr) == 1
              && EVP_DecryptInit_ex(cipher, nullptr, nullptr, bytes(key), bytes(nonce)) == 1
              && EVP_DecryptUpdate(cipher, nullptr, &written, bytes(header), int(header.size())) == 1;
    if (ok && length > 0) {
        ok = EVP_DecryptUpdate(cipher, out, &written, bytes(ciphertext), length) == 1;
        total = written;
    }
    // OpenSSL compares the tag in constant time.
    ok = ok && EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_TAG, int(tag.size()), tag.data()) == 1
         && EVP_DecryptFinal_ex(cipher, out + total, &written) == 1 && total + written == length;
    if (!ok) {
        // Unauthenticated bytes are never handed out, not even partly.
        OPENSSL_cleanse(plaintext.data(), std::size_t(plaintext.size()));
        return std::nullopt;
    }
    return plaintext;
}

} // namespace OpenChat

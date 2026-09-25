#include "security/AttachmentSeal.h"

#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

namespace OpenChat {
namespace {

constexpr qsizetype nonceBytes = AttachmentLimits::controlNonceBytes;
constexpr qsizetype tagBytes = AttachmentLimits::sealOverhead;
static_assert(tagBytes == 16, "AES-GCM's full tag");
static_assert(nonceBytes == 12, "AES-GCM's standard nonce");

using CipherContextPointer = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// A Part's nonce, 00 00 00 00 ‖ u32 type ‖ u32 index. A key belongs to one
// attachment and each part is sealed under it once (a part sent again is the
// same sealed bytes, never a new encryption of them), so none repeats.
[[nodiscard]] QByteArray partNonce(quint32 index)
{
    QByteArray nonce(nonceBytes, '\0');
    qToBigEndian<quint32>(quint32(AttachmentFrameType::Part), nonce.data() + 4);
    qToBigEndian<quint32>(index, nonce.data() + 8);
    return nonce;
}

// Any other frame can be sealed again with different content (a receiver asks
// again for fewer parts, and every member of a group asks under the same key),
// so each seal draws its own nonce and carries it ahead of the ciphertext. Its
// top bit is set, which no Part nonce has. Empty if the generator fails.
[[nodiscard]] QByteArray randomNonce()
{
    QByteArray nonce(nonceBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), int(nonce.size())) != 1)
        return {};
    nonce[0] = char(quint8(nonce[0]) | 0x80);
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
    const bool isPart = type == AttachmentFrameType::Part;
    const qsizetype overhead = isPart ? tagBytes : AttachmentLimits::controlSealOverhead;
    if (key.size() != AttachmentLimits::keyBytes || !isSendableHeader(type, index)
        || attachmentFrameHeaderBytes + plaintext.size() + overhead > AttachmentLimits::maxFrameBytes)
        return {};

    const QByteArray header = attachmentFrameHeader(type, attachmentId, index);
    const QByteArray nonce = isPart ? partNonce(index) : randomNonce();
    if (nonce.isEmpty())
        return {};
    const int length = int(plaintext.size());
    QByteArray frame = header;
    if (!isPart)
        frame += nonce;
    const qsizetype bodyStart = frame.size();
    frame.resize(bodyStart + length + tagBytes);
    auto *out = reinterpret_cast<unsigned char *>(frame.data()) + bodyStart;

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
        || header.size() + sealedBody.size() > AttachmentLimits::maxFrameBytes)
        return std::nullopt;

    // A Part's nonce is rebuilt from the header; any other type's leads its
    // body. The header is also the associated data, so a body presented under
    // any other header fails the tag.
    const quint8 type = quint8(header[2]);
    const quint32 index = qFromBigEndian<quint32>(header.data() + 19);
    const bool isPart = type == quint8(AttachmentFrameType::Part);
    if (sealedBody.size() < (isPart ? tagBytes : AttachmentLimits::controlSealOverhead))
        return std::nullopt;
    const QByteArray nonce = isPart ? partNonce(index) : sealedBody.first(nonceBytes).toByteArray();
    // A nonce without the top bit could only repeat a Part's.
    if (!isPart && (quint8(nonce[0]) & 0x80) == 0)
        return std::nullopt;
    if (!isPart)
        sealedBody = sealedBody.sliced(nonceBytes);
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

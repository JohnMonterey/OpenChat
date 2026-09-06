#include "call/CallMediaCrypto.h"

#include "call/CallSignal.h" // callSecretBytes

#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace OpenChat {

namespace {

constexpr int mediaKeyBytes = 32;
constexpr int mediaSaltBytes = CallMediaSealer::nonceBytes;
constexpr int derivedPerDirection = mediaKeyBytes + mediaSaltBytes;

// Distinct HKDF info labels per direction. Changing either string re-keys the
// media path, so they are versioned along with the packet format.
constexpr char callerLabel[] = "openchat/call/v1/media/caller";
constexpr char calleeLabel[] = "openchat/call/v1/media/callee";

// HKDF-SHA256 over the raw secret. The call id is the HKDF salt: the same
// secret observed twice under different call ids expands to unrelated keys, so
// a replayed offer cannot resurrect a previous call's key stream.
[[nodiscard]] QByteArray hkdf(QByteArrayView secret, QByteArrayView salt, QByteArrayView info,
                              int outputBytes)
{
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (context == nullptr)
        return {};
    QByteArray output(outputBytes, Qt::Uninitialized);
    auto length = static_cast<size_t>(outputBytes);
    const bool ok =
        EVP_PKEY_derive_init(context) == 1
        && EVP_PKEY_CTX_set_hkdf_md(context, EVP_sha256()) == 1
        && EVP_PKEY_CTX_set1_hkdf_salt(context,
                                       reinterpret_cast<const unsigned char *>(salt.data()),
                                       static_cast<int>(salt.size())) == 1
        && EVP_PKEY_CTX_set1_hkdf_key(context,
                                      reinterpret_cast<const unsigned char *>(secret.data()),
                                      static_cast<int>(secret.size())) == 1
        && EVP_PKEY_CTX_add1_hkdf_info(context,
                                       reinterpret_cast<const unsigned char *>(info.data()),
                                       static_cast<int>(info.size())) == 1
        && EVP_PKEY_derive(context, reinterpret_cast<unsigned char *>(output.data()), &length) == 1;
    EVP_PKEY_CTX_free(context);
    if (!ok || length != static_cast<size_t>(outputBytes))
        return {};
    return output;
}

[[nodiscard]] CallMediaKeys splitKeys(const QByteArray &derived)
{
    if (derived.size() != derivedPerDirection)
        return {};
    return CallMediaKeys{derived.left(mediaKeyBytes), derived.mid(mediaKeyBytes, mediaSaltBytes)};
}

// nonce = salt XOR the sequence, right-aligned. Distinct sequences therefore
// give distinct nonces under a fixed salt, which is the whole GCM requirement.
[[nodiscard]] std::array<unsigned char, CallMediaSealer::nonceBytes>
nonceFor(const QByteArray &salt, quint32 sequence)
{
    std::array<unsigned char, CallMediaSealer::nonceBytes> nonce{};
    std::memcpy(nonce.data(), salt.constData(), nonce.size());
    unsigned char counter[4];
    qToBigEndian(sequence, counter);
    for (size_t i = 0; i < sizeof(counter); ++i)
        nonce[nonce.size() - sizeof(counter) + i] ^= counter[i];
    return nonce;
}

} // namespace

bool CallMediaKeys::isValid() const noexcept
{
    return key.size() == mediaKeyBytes && salt.size() == mediaSaltBytes;
}

std::optional<CallMediaKeySchedule> CallMediaKeySchedule::derive(QByteArrayView secret,
                                                                 const CallId &callId)
{
    if (secret.size() != callSecretBytes)
        return std::nullopt;
    const QByteArray salt = callId.bytes();
    CallMediaKeySchedule schedule;
    schedule.fromCaller = splitKeys(
        hkdf(secret, salt, QByteArrayView(callerLabel, sizeof(callerLabel) - 1),
             derivedPerDirection));
    schedule.fromCallee = splitKeys(
        hkdf(secret, salt, QByteArrayView(calleeLabel, sizeof(calleeLabel) - 1),
             derivedPerDirection));
    if (!schedule.fromCaller.isValid() || !schedule.fromCallee.isValid())
        return std::nullopt;
    return schedule;
}

std::optional<CallMediaKeySchedule> CallMediaKeySchedule::deriveVideo(QByteArrayView secret,
                                                                    const CallId &callId)
{
    if (secret.size() != callSecretBytes)
        return std::nullopt;
    // Separate domains: audio and video can use the same sequence without ever
    // reusing a GCM key/nonce pair. Camera toggles do not reset this schedule.
    CallMediaKeySchedule schedule;
    schedule.fromCaller = splitKeys(hkdf(secret, callId.bytes(),
        QByteArrayView("openchat/call/v1/video/caller"), derivedPerDirection));
    schedule.fromCallee = splitKeys(hkdf(secret, callId.bytes(),
        QByteArrayView("openchat/call/v1/video/callee"), derivedPerDirection));
    if (!schedule.fromCaller.isValid() || !schedule.fromCallee.isValid())
        return std::nullopt;
    return schedule;
}

std::optional<CallMediaKeySchedule> CallMediaKeySchedule::deriveScreen(QByteArrayView secret,
                                                                       const CallId &callId)
{
    if (secret.size() != callSecretBytes)
        return std::nullopt;
    CallMediaKeySchedule schedule;
    schedule.fromCaller = splitKeys(hkdf(secret, callId.bytes(),
        QByteArrayView("openchat/call/v1/screen/caller"), derivedPerDirection));
    schedule.fromCallee = splitKeys(hkdf(secret, callId.bytes(),
        QByteArrayView("openchat/call/v1/screen/callee"), derivedPerDirection));
    if (!schedule.fromCaller.isValid() || !schedule.fromCallee.isValid())
        return std::nullopt;
    return schedule;
}

std::optional<CallMediaKeySchedule>
CallMediaKeySchedule::deriveScreenFeedback(QByteArrayView secret, const CallId &callId)
{
    if (secret.size() != callSecretBytes)
        return std::nullopt;
    CallMediaKeySchedule schedule;
    schedule.fromCaller = splitKeys(hkdf(secret, callId.bytes(),
        QByteArrayView("openchat/call/v1/screenfb/caller"), derivedPerDirection));
    schedule.fromCallee = splitKeys(hkdf(secret, callId.bytes(),
        QByteArrayView("openchat/call/v1/screenfb/callee"), derivedPerDirection));
    if (!schedule.fromCaller.isValid() || !schedule.fromCallee.isValid())
        return std::nullopt;
    return schedule;
}

QByteArray deriveGroupPairSecret(QByteArrayView callSecret, const CallId &callId,
                                 const DeviceId &first, const DeviceId &second)
{
    if (callSecret.size() != callSecretBytes)
        return {};
    // Order the pair so both members build the same label whichever end they are.
    const QByteArray low = std::min(first.bytes(), second.bytes());
    const QByteArray high = std::max(first.bytes(), second.bytes());
    QByteArray info = QByteArrayLiteral("openchat/call/v1/group/pair");
    info.append(low);
    info.append(high);
    return hkdf(callSecret, callId.bytes(), info, callSecretBytes);
}

const CallMediaKeys &CallMediaKeySchedule::sendKeys(CallDirection direction) const noexcept
{
    return direction == CallDirection::Outgoing ? fromCaller : fromCallee;
}

const CallMediaKeys &CallMediaKeySchedule::receiveKeys(CallDirection direction) const noexcept
{
    return direction == CallDirection::Outgoing ? fromCallee : fromCaller;
}

CallMediaSealer::CallMediaSealer(CallMediaKeys keys)
    : m_keys(std::move(keys))
{
}

QByteArray CallMediaSealer::seal(quint32 sequence, QByteArrayView plaintext,
                                 QByteArrayView associatedData) const
{
    if (!m_keys.isValid())
        return {};
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (context == nullptr)
        return {};

    const auto nonce = nonceFor(m_keys.salt, sequence);
    QByteArray output(plaintext.size() + tagBytes, Qt::Uninitialized);
    auto *out = reinterpret_cast<unsigned char *>(output.data());
    int written = 0;
    int total = 0;
    bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonceBytes, nullptr) == 1
        && EVP_EncryptInit_ex(context, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(m_keys.key.constData()),
                              nonce.data()) == 1;
    if (ok && !associatedData.isEmpty()) {
        ok = EVP_EncryptUpdate(context, nullptr, &written,
                               reinterpret_cast<const unsigned char *>(associatedData.data()),
                               static_cast<int>(associatedData.size())) == 1;
    }
    if (ok && !plaintext.isEmpty()) {
        ok = EVP_EncryptUpdate(context, out, &written,
                               reinterpret_cast<const unsigned char *>(plaintext.data()),
                               static_cast<int>(plaintext.size())) == 1;
        total = written;
    }
    if (ok)
        ok = EVP_EncryptFinal_ex(context, out + total, &written) == 1;
    if (ok)
        total += written;
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tagBytes, out + total) == 1;
        total += tagBytes;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok)
        return {};
    output.resize(total);
    return output;
}

CallMediaOpener::CallMediaOpener(CallMediaKeys keys)
    : m_keys(std::move(keys))
{
}

bool CallMediaOpener::isFresh(quint32 sequence) const noexcept
{
    if (!m_seen)
        return true;
    const auto behind = static_cast<qint32>(m_highest - sequence);
    if (behind < 0)
        return true; // ahead of everything seen so far
    if (behind >= windowSize)
        return false; // too old to prove it is not a replay
    return (m_window & (quint64{1} << behind)) == 0;
}

void CallMediaOpener::commit(quint32 sequence) noexcept
{
    if (!m_seen) {
        m_seen = true;
        m_highest = sequence;
        m_window = 1;
        return;
    }
    const auto ahead = static_cast<qint32>(sequence - m_highest);
    if (ahead > 0) {
        // Shifting by >= 64 is undefined, and anything that far ahead clears the
        // window outright, so handle the jump explicitly.
        m_window = ahead >= windowSize ? quint64{1} : ((m_window << ahead) | quint64{1});
        m_highest = sequence;
        return;
    }
    m_window |= quint64{1} << static_cast<quint32>(-ahead);
}

std::optional<QByteArray> CallMediaOpener::open(quint32 sequence, QByteArrayView sealed,
                                                QByteArrayView associatedData)
{
    if (!m_keys.isValid() || sealed.size() < CallMediaSealer::tagBytes) {
        ++m_rejected;
        return std::nullopt;
    }
    if (!isFresh(sequence)) {
        ++m_replays;
        return std::nullopt;
    }

    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        ++m_rejected;
        return std::nullopt;
    }

    const auto nonce = nonceFor(m_keys.salt, sequence);
    const qsizetype cipherBytes = sealed.size() - CallMediaSealer::tagBytes;
    const auto *cipher = reinterpret_cast<const unsigned char *>(sealed.data());
    auto *tag = const_cast<unsigned char *>(cipher + cipherBytes);

    QByteArray plaintext(cipherBytes, Qt::Uninitialized);
    auto *out = reinterpret_cast<unsigned char *>(plaintext.data());
    int written = 0;
    int total = 0;
    bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, CallMediaSealer::nonceBytes,
                               nullptr) == 1
        && EVP_DecryptInit_ex(context, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(m_keys.key.constData()),
                              nonce.data()) == 1;
    if (ok && !associatedData.isEmpty()) {
        ok = EVP_DecryptUpdate(context, nullptr, &written,
                               reinterpret_cast<const unsigned char *>(associatedData.data()),
                               static_cast<int>(associatedData.size())) == 1;
    }
    if (ok && cipherBytes > 0) {
        ok = EVP_DecryptUpdate(context, out, &written, cipher, static_cast<int>(cipherBytes)) == 1;
        total = written;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, CallMediaSealer::tagBytes, tag) == 1
            && EVP_DecryptFinal_ex(context, out + total, &written) == 1;
    }
    EVP_CIPHER_CTX_free(context);

    if (!ok) {
        // Authentication failed: the window is deliberately NOT advanced, so a
        // forged packet claiming a high sequence cannot lock out the real ones.
        ++m_rejected;
        return std::nullopt;
    }
    total += written;
    plaintext.resize(total);
    commit(sequence);
    ++m_accepted;
    return plaintext;
}

} // namespace OpenChat

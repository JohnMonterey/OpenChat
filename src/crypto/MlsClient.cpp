#include "crypto/MlsClient.h"
#include "diagnostics/Logging.h"

#include <openchat_mls.h>

#include <QMutexLocker>

#include <cstring>
#include <limits>
#include <optional>

namespace OpenChat {
namespace {

MlsError errorFromCode(int code)
{
    qCWarning(mlsLog) << "MLS operation failed; bridge status" << code;
    switch (code) {
    case OC_MLS_INVALID_INPUT:
        return MlsError::InvalidInput;
    case OC_MLS_MISSING_GROUP:
        return MlsError::MissingGroup;
    case OC_MLS_INVALID_MESSAGE:
        return MlsError::InvalidMessage;
    case OC_MLS_CRYPTO_ERROR:
        return MlsError::Crypto;
    case OC_MLS_STORAGE_ERROR:
        return MlsError::Storage;
    case OC_MLS_UNSUPPORTED:
        return MlsError::Unsupported;
    default:
        return MlsError::Internal;
    }
}

QByteArray takeBuffer(oc_mls_buffer buffer)
{
    QByteArray bytes;
    if (buffer.data && buffer.len <= static_cast<size_t>(std::numeric_limits<qsizetype>::max()))
        bytes = QByteArray(reinterpret_cast<const char *>(buffer.data),
                           static_cast<qsizetype>(buffer.len));
    oc_mls_free_buffer(buffer);
    return bytes;
}

QByteArray frameList(const QList<QByteArray> &values)
{
    if (values.isEmpty() || values.size() > std::numeric_limits<quint16>::max())
        return {};

    QByteArray framed;
    const quint16 count = static_cast<quint16>(values.size());
    framed.append(static_cast<char>((count >> 8) & 0xff));
    framed.append(static_cast<char>(count & 0xff));
    for (const QByteArray &value : values) {
        if (value.isEmpty()
            || static_cast<quint64>(value.size()) > std::numeric_limits<quint32>::max())
            return {};
        const quint32 size = static_cast<quint32>(value.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            framed.append(static_cast<char>((size >> shift) & 0xff));
        framed.append(value);
    }
    return framed;
}

// Exact inverse of frameList: u16 count, then per item a u32 byte length +
// bytes. Returns nullopt on any framing inconsistency.
std::optional<QList<QByteArray>> unframeList(QByteArrayView framed)
{
    if (framed.size() < 2)
        return std::nullopt;
    const auto byteAt = [&](qsizetype index) {
        return static_cast<quint32>(static_cast<quint8>(framed[index]));
    };
    const quint16 count = static_cast<quint16>((byteAt(0) << 8) | byteAt(1));
    qsizetype position = 2;
    QList<QByteArray> values;
    values.reserve(count);
    for (quint16 index = 0; index < count; ++index) {
        if (position + 4 > framed.size())
            return std::nullopt;
        quint32 size = 0;
        for (int byte = 0; byte < 4; ++byte)
            size = (size << 8) | byteAt(position++);
        if (size == 0 || static_cast<qsizetype>(size) > framed.size() - position)
            return std::nullopt;
        values.append(framed.sliced(position, static_cast<qsizetype>(size)).toByteArray());
        position += static_cast<qsizetype>(size);
    }
    if (position != framed.size())
        return std::nullopt;
    return values;
}

} // namespace

struct MlsClient::CallbackContext final {
    MlsStateStore *store = nullptr;
    QByteArray pendingLoad;

    ~CallbackContext()
    {
        pendingLoad.fill('\0');
    }

    static int load(void *opaque, uint8_t *data, size_t capacity, size_t *actualSize) noexcept
    {
        try {
            if (!opaque || !actualSize)
                return -1;
            auto &context = *static_cast<CallbackContext *>(opaque);
            if (!data && capacity == 0) {
                auto loaded = context.store->load();
                if (!loaded)
                    return -1;
                context.pendingLoad = std::move(loaded).value();
                *actualSize = static_cast<size_t>(context.pendingLoad.size());
                return 0;
            }
            if (capacity < static_cast<size_t>(context.pendingLoad.size()))
                return -1;
            if (!context.pendingLoad.isEmpty())
                std::memcpy(data, context.pendingLoad.constData(),
                            static_cast<size_t>(context.pendingLoad.size()));
            *actualSize = static_cast<size_t>(context.pendingLoad.size());
            context.pendingLoad.fill('\0');
            context.pendingLoad.clear();
            return 0;
        } catch (...) {
            return -1;
        }
    }

    static int storeState(void *opaque, const uint8_t *data, size_t size) noexcept
    {
        try {
            if (!opaque || (!data && size != 0)
                || size > static_cast<size_t>(std::numeric_limits<qsizetype>::max()))
                return -1;
            auto &context = *static_cast<CallbackContext *>(opaque);
            auto stored = context.store->store(
                QByteArrayView(reinterpret_cast<const char *>(data),
                               static_cast<qsizetype>(size)));
            return stored ? 0 : -1;
        } catch (...) {
            return -1;
        }
    }
};

Result<std::unique_ptr<MlsClient>, MlsError>
MlsClient::create(QByteArrayView identity, MlsStateStore *stateStore)
{
    if (identity.isEmpty())
        return Result<std::unique_ptr<MlsClient>, MlsError>::failure(MlsError::InvalidInput);

    auto context = std::make_unique<CallbackContext>();
    context->store = stateStore;
    oc_mls_storage_callbacks callbacks{};
    const oc_mls_storage_callbacks *callbacksPointer = nullptr;
    if (stateStore) {
        callbacks.context = context.get();
        callbacks.load = &CallbackContext::load;
        callbacks.store = &CallbackContext::storeState;
        callbacksPointer = &callbacks;
    }

    oc_mls_client *handle = nullptr;
    const int status = oc_mls_client_create(
        reinterpret_cast<const uint8_t *>(identity.data()),
        static_cast<size_t>(identity.size()), callbacksPointer, &handle);
    if (status != OC_MLS_OK)
        return Result<std::unique_ptr<MlsClient>, MlsError>::failure(errorFromCode(status));
    return Result<std::unique_ptr<MlsClient>, MlsError>::success(
        std::unique_ptr<MlsClient>(new MlsClient(std::move(context), handle)));
}

MlsClient::MlsClient(std::unique_ptr<CallbackContext> callbackContext, oc_mls_client *handle)
    : m_callbackContext(std::move(callbackContext))
    , m_handle(handle)
{
}

MlsClient::~MlsClient()
{
    oc_mls_client_free(m_handle);
}

Result<QByteArray, MlsError> MlsClient::generateKeyPackage()
{
    qCDebug(mlsLog) << "Generating key package";
    QMutexLocker locker(&m_mutex);
    oc_mls_buffer output{};
    const int status = oc_mls_generate_key_package(m_handle, &output);
    if (status != OC_MLS_OK)
        return Result<QByteArray, MlsError>::failure(errorFromCode(status));
    return Result<QByteArray, MlsError>::success(takeBuffer(output));
}

Result<void, MlsError> MlsClient::createGroup(const ConversationId &conversation)
{
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    const int status = oc_mls_create_group(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()));
    if (status != OC_MLS_OK)
        return Result<void, MlsError>::failure(errorFromCode(status));
    return Result<void, MlsError>::success();
}

Result<void, MlsError> MlsClient::joinGroup(const ConversationId &conversation,
                                            QByteArrayView welcome)
{
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    const int status = oc_mls_join_group(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()),
        reinterpret_cast<const uint8_t *>(welcome.data()),
        static_cast<size_t>(welcome.size()));
    if (status != OC_MLS_OK)
        return Result<void, MlsError>::failure(errorFromCode(status));
    return Result<void, MlsError>::success();
}

Result<QList<QByteArray>, MlsError> MlsClient::inspectWelcome(QByteArrayView welcome)
{
    QMutexLocker locker(&m_mutex);
    oc_mls_buffer output{};
    const int status = oc_mls_inspect_welcome(
        m_handle, reinterpret_cast<const uint8_t *>(welcome.data()),
        static_cast<size_t>(welcome.size()), &output);
    if (status != OC_MLS_OK)
        return Result<QList<QByteArray>, MlsError>::failure(errorFromCode(status));
    const QByteArray framed = takeBuffer(output);
    auto members = unframeList(framed);
    if (!members)
        return Result<QList<QByteArray>, MlsError>::failure(MlsError::Internal);
    return Result<QList<QByteArray>, MlsError>::success(std::move(*members));
}

Result<QByteArray, MlsError> MlsClient::inspectKeyPackage(QByteArrayView keyPackage)
{
    QMutexLocker locker(&m_mutex);
    oc_mls_buffer output{};
    const int status = oc_mls_inspect_key_package(
        m_handle, reinterpret_cast<const uint8_t *>(keyPackage.data()),
        static_cast<size_t>(keyPackage.size()), &output);
    if (status != OC_MLS_OK)
        return Result<QByteArray, MlsError>::failure(errorFromCode(status));
    return Result<QByteArray, MlsError>::success(takeBuffer(output));
}

Result<MlsAddResult, MlsError>
MlsClient::addMembers(const ConversationId &conversation,
                      const QList<QByteArray> &keyPackages)
{
    const QByteArray framed = frameList(keyPackages);
    if (framed.isEmpty())
        return Result<MlsAddResult, MlsError>::failure(MlsError::InvalidInput);
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    oc_mls_add_result output{};
    const int status = oc_mls_add_members(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()),
        reinterpret_cast<const uint8_t *>(framed.constData()),
        static_cast<size_t>(framed.size()), &output);
    if (status != OC_MLS_OK)
        return Result<MlsAddResult, MlsError>::failure(errorFromCode(status));
    return Result<MlsAddResult, MlsError>::success(
        MlsAddResult{takeBuffer(output.commit), takeBuffer(output.welcome)});
}

Result<QByteArray, MlsError>
MlsClient::removeMembers(const ConversationId &conversation,
                         const QList<QByteArray> &identities)
{
    const QByteArray framed = frameList(identities);
    if (framed.isEmpty())
        return Result<QByteArray, MlsError>::failure(MlsError::InvalidInput);
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    oc_mls_buffer output{};
    const int status = oc_mls_remove_members(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()),
        reinterpret_cast<const uint8_t *>(framed.constData()),
        static_cast<size_t>(framed.size()), &output);
    if (status != OC_MLS_OK)
        return Result<QByteArray, MlsError>::failure(errorFromCode(status));
    return Result<QByteArray, MlsError>::success(takeBuffer(output));
}

Result<QList<QByteArray>, MlsError> MlsClient::groupMembers(const ConversationId &conversation)
{
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    oc_mls_buffer output{};
    const int status = oc_mls_group_members(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()), &output);
    if (status != OC_MLS_OK)
        return Result<QList<QByteArray>, MlsError>::failure(errorFromCode(status));
    const QByteArray framed = takeBuffer(output);
    // A group whose only member is this device frames as a zero-count list,
    // which the bridge hands back as an empty buffer.
    if (framed.isEmpty())
        return Result<QList<QByteArray>, MlsError>::success(QList<QByteArray>{});
    auto members = unframeList(framed);
    if (!members)
        return Result<QList<QByteArray>, MlsError>::failure(MlsError::Internal);
    return Result<QList<QByteArray>, MlsError>::success(std::move(*members));
}

Result<MlsCiphertext, MlsError>
MlsClient::encrypt(const ConversationId &conversation, QByteArrayView plaintext)
{
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    oc_mls_buffer output{};
    const int status = oc_mls_encrypt(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()),
        reinterpret_cast<const uint8_t *>(plaintext.data()),
        static_cast<size_t>(plaintext.size()), &output);
    if (status != OC_MLS_OK)
        return Result<MlsCiphertext, MlsError>::failure(errorFromCode(status));
    return Result<MlsCiphertext, MlsError>::success(MlsCiphertext{takeBuffer(output)});
}

Result<MlsProcessResult, MlsError>
MlsClient::process(const ConversationId &conversation, QByteArrayView mlsMessage)
{
    QMutexLocker locker(&m_mutex);
    const QByteArray bytes = conversation.bytes();
    oc_mls_process_result output{};
    const int status = oc_mls_process(
        m_handle, reinterpret_cast<const uint8_t *>(bytes.constData()),
        reinterpret_cast<const uint8_t *>(mlsMessage.data()),
        static_cast<size_t>(mlsMessage.size()), &output);
    if (status != OC_MLS_OK)
        return Result<MlsProcessResult, MlsError>::failure(errorFromCode(status));

    MlsProcessKind kind;
    switch (output.kind) {
    case OC_MLS_APPLICATION:
        kind = MlsProcessKind::Application;
        break;
    case OC_MLS_PROPOSAL:
        kind = MlsProcessKind::Proposal;
        break;
    case OC_MLS_COMMIT:
        kind = MlsProcessKind::Commit;
        break;
    default:
        oc_mls_free_buffer(output.payload);
        oc_mls_free_buffer(output.sender);
        return Result<MlsProcessResult, MlsError>::failure(MlsError::Internal);
    }
    return Result<MlsProcessResult, MlsError>::success(
        MlsProcessResult{kind, takeBuffer(output.payload), takeBuffer(output.sender)});
}

} // namespace OpenChat

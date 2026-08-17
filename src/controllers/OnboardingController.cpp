#include "controllers/OnboardingController.h"

#include <utility>

namespace OpenChat {

namespace {

// Deterministic placeholder recovery code used until a real Starter is injected.
// Grouped like the codes the security layer emits so the Recovery screen and its
// captures read realistically; it carries no cryptographic meaning and only lets
// the standalone screen and capture path render an account-creation result.
QString placeholderRecoveryCode()
{
    return QStringLiteral("K7QME-3FBWX-9TJHR-2VNDS-8PLCA-6YZUG");
}

} // namespace

OnboardingController::OnboardingController(QObject *parent)
    : OnboardingController(Starter{}, parent)
{
}

OnboardingController::OnboardingController(Starter starter, QObject *parent)
    : QObject(parent)
    , m_starter(std::move(starter))
{
}

OnboardingController::Step OnboardingController::step() const
{
    return m_step;
}

QString OnboardingController::displayName() const
{
    return m_displayName;
}

QString OnboardingController::handle() const
{
    return m_handle;
}

bool OnboardingController::canCreate() const
{
    return !m_displayName.trimmed().isEmpty() && handleIsValid(m_handle);
}

bool OnboardingController::creating() const
{
    return m_creating;
}

QString OnboardingController::recoveryCode() const
{
    return m_recoveryCode;
}

QString OnboardingController::errorText() const
{
    return m_errorText;
}

void OnboardingController::setDisplayName(const QString &displayName)
{
    if (m_displayName == displayName)
        return;

    const bool wasCreatable = canCreate();
    m_displayName = displayName;
    emit displayNameChanged();
    if (wasCreatable != canCreate())
        emit canCreateChanged();
}

void OnboardingController::setHandle(const QString &handle)
{
    if (m_handle == handle)
        return;

    const bool wasCreatable = canCreate();
    m_handle = handle;
    emit handleChanged();
    if (wasCreatable != canCreate())
        emit canCreateChanged();
}

void OnboardingController::createProfile()
{
    // Only the Create step creates, only when both fields are valid, and only
    // when no creation is already in flight. Guarding here keeps a disabled
    // button, a stray invocation, or a double-tap from starting a second flow.
    if (m_step != Step::Create || !canCreate() || m_creating)
        return;

    setErrorText({});
    setCreating(true);

    if (m_starter) {
        // Real (or test) Starter: the outcome arrives later through the owner's
        // onCreationSucceeded()/onCreationFailed() callbacks.
        m_starter(m_displayName.trimmed(), m_handle.trimmed());
        return;
    }

    // No Starter wired (the standalone/preview path): complete immediately with a
    // deterministic placeholder so the screen and captures render a result.
    onCreationSucceeded(placeholderRecoveryCode());
}

void OnboardingController::onCreationSucceeded(const QString &recoveryCode)
{
    // Ignore a late or duplicated success: only an in-flight create on the Create
    // step may advance, and it does so exactly once.
    if (!m_creating || m_step != Step::Create)
        return;

    setCreating(false);
    setErrorText({});
    setRecoveryCode(recoveryCode);
    setStep(Step::Recovery);
}

void OnboardingController::onCreationFailed(const QString &message)
{
    if (!m_creating || m_step != Step::Create)
        return;

    setCreating(false);
    setErrorText(message.isEmpty()
                     ? QStringLiteral("Couldn't create your profile. Please try again.")
                     : message);
    emit creationFailed();
}

void OnboardingController::confirmRecoverySaved()
{
    if (m_step != Step::Recovery)
        return;

    setStep(Step::Done);
    emit completed();
}

bool OnboardingController::handleIsValid(const QString &handle)
{
    const QString trimmed = handle.trimmed();
    if (trimmed.isEmpty())
        return false;

    for (const QChar ch : trimmed) {
        if (ch.isSpace())
            return false;
    }
    return true;
}

void OnboardingController::setStep(Step step)
{
    if (m_step == step)
        return;

    m_step = step;
    emit stepChanged();
}

void OnboardingController::setCreating(bool creating)
{
    if (m_creating == creating)
        return;

    m_creating = creating;
    emit creatingChanged();
}

void OnboardingController::setRecoveryCode(const QString &code)
{
    if (m_recoveryCode == code)
        return;

    m_recoveryCode = code;
    emit recoveryCodeChanged();
}

void OnboardingController::setErrorText(const QString &text)
{
    if (m_errorText == text)
        return;

    m_errorText = text;
    emit errorTextChanged();
}

} // namespace OpenChat

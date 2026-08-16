#include "controllers/OnboardingController.h"

#include <utility>

namespace OpenChat {

namespace {

// Deterministic placeholder recovery code used until a real Creator is injected.
// Grouped like the codes the security layer emits so the Recovery screen and its
// captures read realistically; it carries no cryptographic meaning and only lets
// the standalone screen and capture path render an account-creation result.
QString placeholderRecoveryCode()
{
    return QStringLiteral("K7QME-3FBWX-9TJHR-2VNDS-8PLCA-6YZUG");
}

} // namespace

OnboardingController::OnboardingController(QObject *parent)
    : OnboardingController(
          [](const QString &, const QString &) -> std::optional<QString> {
              return placeholderRecoveryCode();
          },
          parent)
{
}

OnboardingController::OnboardingController(Creator creator, QObject *parent)
    : QObject(parent)
    , m_creator(std::move(creator))
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
    // Only the Create step creates, and only when both fields are valid. Guarding
    // here keeps a disabled button or a stray invocation from advancing the flow.
    if (m_step != Step::Create || !canCreate())
        return;

    const std::optional<QString> code =
        m_creator ? m_creator(m_displayName.trimmed(), m_handle.trimmed()) : std::nullopt;
    if (!code) {
        setErrorText(QStringLiteral("Couldn't create your profile. Please try again."));
        emit creationFailed();
        return;
    }

    setErrorText({});
    setRecoveryCode(*code);
    setStep(Step::Recovery);
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

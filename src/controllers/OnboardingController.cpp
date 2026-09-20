#include "controllers/OnboardingController.h"

#include "domain/Handle.h"
#include "security/PasswordKey.h"

#include <QSet>

#include <algorithm>
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

// Overwrites a string's characters before releasing it. Best effort -- Qt may
// have made copies on the way in from the text field -- but it keeps the
// controller itself from leaving the password behind in freed memory.
void scrub(QString &secret)
{
    secret.fill(QChar(u'\0'));
    secret.clear();
}

// Long enough to pass a length check, guessed first by every cracking tool.
bool isCommonPassword(const QString &lowered)
{
    static const QSet<QString> common{
        QStringLiteral("password123"),  QStringLiteral("passwordpassword"),
        QStringLiteral("1234567890"),   QStringLiteral("0123456789"),
        QStringLiteral("12345678910"),  QStringLiteral("0987654321"),
        QStringLiteral("qwertyuiop"),   QStringLiteral("qwertyuiop123"),
        QStringLiteral("1q2w3e4r5t"),   QStringLiteral("1qaz2wsx3edc"),
        QStringLiteral("iloveyou123"),  QStringLiteral("letmeinplease"),
        QStringLiteral("administrator"), QStringLiteral("welcome123"),
        QStringLiteral("openchat123"),  QStringLiteral("abcdefghij"),
    };
    return common.contains(lowered) || lowered.startsWith(QStringLiteral("password"));
}

bool isSingleRepeatedCharacter(const QString &text)
{
    return std::all_of(text.cbegin(), text.cend(),
                       [first = text.front()](QChar ch) { return ch == first; });
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

OnboardingController::~OnboardingController()
{
    scrub(m_password);
    scrub(m_passwordConfirm);
}

OnboardingController::Step OnboardingController::step() const
{
    return m_step;
}

OnboardingController::Mode OnboardingController::mode() const
{
    return m_mode;
}

QString OnboardingController::handle() const
{
    return m_handle;
}

QString OnboardingController::password() const
{
    return m_password;
}

QString OnboardingController::passwordConfirm() const
{
    return m_passwordConfirm;
}

bool OnboardingController::busy() const
{
    return m_busy;
}

QString OnboardingController::recoveryCode() const
{
    return m_recoveryCode;
}

QString OnboardingController::errorText() const
{
    return m_errorText;
}

QString OnboardingController::notice() const
{
    return m_notice;
}

bool OnboardingController::canSubmit() const
{
    if (m_busy || m_step != Step::Credentials || !normalizeHandle(m_handle))
        return false;
    if (m_mode == Mode::LogIn)
        return !m_password.isEmpty() && m_password.size() <= maximumPasswordLength;
    return passwordProblem().isEmpty() && m_password == m_passwordConfirm;
}

QString OnboardingController::handleHint() const
{
    QString candidate = m_handle.trimmed();
    if (candidate.startsWith(QLatin1Char('@')))
        candidate.remove(0, 1);
    if (candidate.isEmpty() || normalizeHandle(m_handle))
        return {};
    if (candidate.size() < minimumHandleLength)
        return QStringLiteral("Use at least %1 characters.").arg(minimumHandleLength);
    if (candidate.size() > maximumHandleLength)
        return QStringLiteral("Use at most %1 characters.").arg(maximumHandleLength);
    const QChar first = candidate.front();
    if (first == QLatin1Char('.') || first == QLatin1Char('_') || first == QLatin1Char('-'))
        return QStringLiteral("Start with a letter or a number.");
    return QStringLiteral("Use only letters, numbers, dots, dashes and underscores.");
}

QString OnboardingController::passwordProblem() const
{
    if (m_password.size() < minimumPasswordLength)
        return QStringLiteral("Use at least %1 characters.").arg(minimumPasswordLength);
    if (m_password.size() > maximumPasswordLength)
        return QStringLiteral("Use at most %1 characters.").arg(maximumPasswordLength);
    const QString lowered = m_password.toLower();
    // The username is public, so it contributes nothing: what is left once it is
    // taken out has to be long enough on its own. (Merely containing it is fine
    // -- a short username can turn up inside any passphrase.)
    if (const std::optional<QString> canonical = normalizeHandle(m_handle)) {
        QString remainder = lowered;
        remainder.remove(*canonical);
        if (remainder.size() < minimumPasswordLength)
            return QStringLiteral("Don't build your password around your username.");
    }
    if (isSingleRepeatedCharacter(m_password) || isCommonPassword(lowered))
        return QStringLiteral("That password is too easy to guess.");
    return {};
}

QString OnboardingController::passwordHint() const
{
    if (m_mode != Mode::SignUp || m_password.isEmpty())
        return {};
    return passwordProblem();
}

QString OnboardingController::passwordConfirmHint() const
{
    if (m_mode != Mode::SignUp || m_passwordConfirm.isEmpty()
        || m_password == m_passwordConfirm)
        return {};
    return QStringLiteral("The passwords don't match.");
}

int OnboardingController::passwordStrength() const
{
    if (m_password.isEmpty())
        return 0;
    if (!passwordProblem().isEmpty())
        return 1;
    bool lower = false;
    bool upper = false;
    bool digit = false;
    bool other = false;
    for (const QChar ch : m_password) {
        if (ch.isLower())
            lower = true;
        else if (ch.isUpper())
            upper = true;
        else if (ch.isDigit())
            digit = true;
        else
            other = true;
    }
    const int classes = int(lower) + int(upper) + int(digit) + int(other);
    // Length dominates: a long passphrase of plain words beats a short jumble.
    int score = 2;
    if (m_password.size() >= 14 || classes >= 3)
        ++score;
    if (m_password.size() >= 18 || (m_password.size() >= 14 && classes >= 3))
        ++score;
    return std::min(score, 4);
}

void OnboardingController::inputsChanged(bool couldSubmit)
{
    emit hintsChanged();
    if (couldSubmit != canSubmit())
        emit canSubmitChanged();
}

void OnboardingController::setMode(Mode mode)
{
    if (m_mode == mode || m_busy || m_step != Step::Credentials)
        return;

    const bool couldSubmit = canSubmit();
    m_mode = mode;
    emit modeChanged();
    clearPasswords();
    setErrorText({});
    inputsChanged(couldSubmit);
}

void OnboardingController::setHandle(const QString &handle)
{
    if (m_handle == handle)
        return;

    const bool couldSubmit = canSubmit();
    m_handle = handle;
    emit handleChanged();
    inputsChanged(couldSubmit);
}

void OnboardingController::setPassword(const QString &password)
{
    if (m_password == password)
        return;

    const bool couldSubmit = canSubmit();
    m_password = password;
    emit passwordChanged();
    inputsChanged(couldSubmit);
}

void OnboardingController::setPasswordConfirm(const QString &passwordConfirm)
{
    if (m_passwordConfirm == passwordConfirm)
        return;

    const bool couldSubmit = canSubmit();
    m_passwordConfirm = passwordConfirm;
    emit passwordConfirmChanged();
    inputsChanged(couldSubmit);
}

void OnboardingController::setNotice(const QString &notice)
{
    if (m_notice == notice)
        return;

    m_notice = notice;
    emit noticeChanged();
}

void OnboardingController::clearPasswords()
{
    if (!m_password.isEmpty()) {
        scrub(m_password);
        emit passwordChanged();
    }
    if (!m_passwordConfirm.isEmpty()) {
        scrub(m_passwordConfirm);
        emit passwordConfirmChanged();
    }
}

void OnboardingController::submit()
{
    // Only the Credentials step submits, only with valid input, and only when no
    // submit is already in flight. Guarding here keeps a disabled button, a stray
    // invocation, or a double-tap from starting a second flow.
    if (!canSubmit())
        return;

    const std::optional<QString> canonical = normalizeHandle(m_handle);
    // Take the password out of the controller before anything else happens, so
    // no path below -- success, failure, or a Starter that throws -- can leave it
    // in the fields.
    QString password = std::exchange(m_password, QString());
    emit passwordChanged();
    if (!m_passwordConfirm.isEmpty()) {
        scrub(m_passwordConfirm);
        emit passwordConfirmChanged();
    }

    setErrorText({});
    setBusy(true);
    emit hintsChanged();
    // canSubmit was true on entry and is false now (no password, and busy).
    emit canSubmitChanged();

    if (m_starter) {
        // Real (or test) Starter: the outcome arrives later through the owner's
        // onSubmitSucceeded()/onSubmitFailed() callbacks.
        m_starter(m_mode, *canonical, password);
        scrub(password);
        return;
    }
    scrub(password);

    // No Starter wired (the standalone/preview path): complete immediately with a
    // deterministic placeholder so the screen and captures render a result.
    onSubmitSucceeded(placeholderRecoveryCode());
}

void OnboardingController::onSubmitSucceeded(const QString &recoveryCode)
{
    // Ignore a late or duplicated success: only an in-flight submit on the
    // Credentials step may advance, and it does so exactly once.
    if (!m_busy || m_step != Step::Credentials)
        return;

    setBusy(false);
    setErrorText({});
    if (m_mode == Mode::LogIn) {
        // Nothing to save when signing in to an account that already exists.
        setStep(Step::Done);
        emit completed();
        return;
    }
    setRecoveryCode(recoveryCode);
    setStep(Step::Recovery);
}

void OnboardingController::onSubmitFailed(const QString &message)
{
    if (!m_busy || m_step != Step::Credentials)
        return;

    setBusy(false);
    setErrorText(message.isEmpty() ? QStringLiteral("Something went wrong. Please try again.")
                                   : message);
    emit submitFailed();
}

void OnboardingController::confirmRecoverySaved()
{
    if (m_step != Step::Recovery)
        return;

    setStep(Step::Done);
    emit completed();
}

void OnboardingController::setStep(Step step)
{
    if (m_step == step)
        return;

    m_step = step;
    emit stepChanged();
}

void OnboardingController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;

    const bool couldSubmit = canSubmit();
    m_busy = busy;
    emit busyChanged();
    if (couldSubmit != canSubmit())
        emit canSubmitChanged();
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

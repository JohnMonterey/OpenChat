#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace OpenChat {

// Drives the first-run surface: signing up for a new account or logging in to an
// existing one with a username and password, then (for a new account) revealing
// the one-time recovery code. The network flow is asynchronous and injected as a
// Starter so the screens and their tests run without a real ProfileSession,
// KeyVault, or relay. The integration phase supplies a Starter that stretches the
// password, performs ProfileSession::create and drives an AccountBootstrap; the
// flow's outcome is reported back through onSubmitSucceeded()/onSubmitFailed().
//
// The password is held only while it is being typed. submit() hands it to the
// Starter and clears both password fields in the same call, so it is gone from
// the controller (and from the bound text fields) before any network activity
// starts, whatever the outcome.
class OnboardingController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Step step READ step NOTIFY stepChanged)
    Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString handle READ handle WRITE setHandle NOTIFY handleChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY passwordChanged)
    Q_PROPERTY(QString passwordConfirm READ passwordConfirm WRITE setPasswordConfirm
                   NOTIFY passwordConfirmChanged)
    Q_PROPERTY(QString handleHint READ handleHint NOTIFY hintsChanged)
    Q_PROPERTY(QString passwordHint READ passwordHint NOTIFY hintsChanged)
    Q_PROPERTY(QString passwordConfirmHint READ passwordConfirmHint NOTIFY hintsChanged)
    Q_PROPERTY(int passwordStrength READ passwordStrength NOTIFY hintsChanged)
    Q_PROPERTY(bool canSubmit READ canSubmit NOTIFY canSubmitChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString recoveryCode READ recoveryCode NOTIFY recoveryCodeChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(QString notice READ notice WRITE setNotice NOTIFY noticeChanged)

public:
    // First-run flow position. Credentials gathers the username and password;
    // Recovery reveals the recovery code that must be saved once (new accounts
    // only); Done marks onboarding complete so the app can open the main window.
    enum class Step {
        Credentials,
        Recovery,
        Done,
    };
    Q_ENUM(Step)

    enum class Mode {
        SignUp, // create a new account
        LogIn,  // sign in to an existing account on this device
    };
    Q_ENUM(Mode)

    // Async seam. Given the mode, the canonical username and the password, kicks
    // off the flow; it returns nothing and the step does not advance
    // synchronously. The owner drives the outcome back through
    // onSubmitSucceeded()/onSubmitFailed(). A default-constructed controller has
    // no Starter and completes immediately with a deterministic placeholder code,
    // so the standalone screen and captures work without real services.
    using Starter =
        std::function<void(Mode mode, const QString &handle, const QString &password)>;

    explicit OnboardingController(QObject *parent = nullptr);
    explicit OnboardingController(Starter starter, QObject *parent = nullptr);
    ~OnboardingController() override;

    [[nodiscard]] Step step() const;
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] QString handle() const;
    [[nodiscard]] QString password() const;
    [[nodiscard]] QString passwordConfirm() const;
    [[nodiscard]] bool canSubmit() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] QString recoveryCode() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString notice() const;

    // Inline guidance for a field that has content but is not acceptable yet;
    // empty when the field is empty or fine. The password rules apply to SignUp
    // only: a login must accept whatever password the account already has.
    [[nodiscard]] QString handleHint() const;
    [[nodiscard]] QString passwordHint() const;
    [[nodiscard]] QString passwordConfirmHint() const;
    // 0 (nothing typed) to 4 (strong); a coarse guide for the sign-up meter.
    [[nodiscard]] int passwordStrength() const;

    // Switching mode keeps the username and clears the passwords and any error.
    // Ignored while a submit is in flight or after the Credentials step.
    Q_INVOKABLE void setMode(Mode mode);
    Q_INVOKABLE void setHandle(const QString &handle);
    Q_INVOKABLE void setPassword(const QString &password);
    Q_INVOKABLE void setPasswordConfirm(const QString &passwordConfirm);
    // An informational line shown above the form (for example after outdated
    // local data was erased on startup).
    void setNotice(const QString &notice);

    // From the Credentials step, if canSubmit is true, marks busy, clears the
    // error and both password fields, and invokes the Starter. The flow advances
    // only when the owner reports success; submit() never advances the step.
    Q_INVOKABLE void submit();

    // From the Recovery step, advances to Done and emits completed() once.
    Q_INVOKABLE void confirmRecoverySaved();

public slots:
    // Owner callbacks that complete an in-flight submit(). Succeeded clears busy
    // and advances: a new account goes to Recovery with its code, a login goes
    // straight to Done and emits completed(). Failed clears busy, stays on
    // Credentials, surfaces the message and emits submitFailed(). Both are no-ops
    // unless a submit is currently in flight, so a late or duplicated callback
    // cannot resurrect or skip the flow.
    void onSubmitSucceeded(const QString &recoveryCode);
    void onSubmitFailed(const QString &message);

signals:
    void stepChanged();
    void modeChanged();
    void handleChanged();
    void passwordChanged();
    void passwordConfirmChanged();
    void hintsChanged();
    void canSubmitChanged();
    void busyChanged();
    void recoveryCodeChanged();
    void errorTextChanged();
    void noticeChanged();
    void submitFailed();
    void completed();

private:
    [[nodiscard]] QString passwordProblem() const;
    // Re-evaluates canSubmit and the hints after any input changed.
    void inputsChanged(bool couldSubmit);
    void clearPasswords();
    void setStep(Step step);
    void setBusy(bool busy);
    void setRecoveryCode(const QString &code);
    void setErrorText(const QString &text);

    Starter m_starter;
    Step m_step = Step::Credentials;
    Mode m_mode = Mode::SignUp;
    bool m_busy = false;
    QString m_handle;
    QString m_password;
    QString m_passwordConfirm;
    QString m_recoveryCode;
    QString m_errorText;
    QString m_notice;
};

} // namespace OpenChat

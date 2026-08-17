#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace OpenChat {

// Drives the first-run onboarding surface: gathering the profile fields,
// starting account creation, and revealing the one-time recovery code. Account
// creation is asynchronous and injected as a Starter so the screens and their
// tests run without a real ProfileSession, KeyVault, or relay. The integration
// phase supplies a Starter that performs ProfileSession::create and drives an
// AccountBootstrap (relay registration + KeyPackage publish); the flow's outcome
// is reported back through onCreationSucceeded()/onCreationFailed().
class OnboardingController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Step step READ step NOTIFY stepChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString handle READ handle WRITE setHandle NOTIFY handleChanged)
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY canCreateChanged)
    Q_PROPERTY(bool creating READ creating NOTIFY creatingChanged)
    Q_PROPERTY(QString recoveryCode READ recoveryCode NOTIFY recoveryCodeChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    // First-run flow position. Create gathers the display name and handle;
    // Recovery reveals the recovery code that must be saved once; Done marks the
    // onboarding complete so the app can proceed into the main window.
    enum class Step {
        Create,
        Recovery,
        Done,
    };
    Q_ENUM(Step)

    // Async creation seam. Given the trimmed display name and handle, kicks off
    // account creation; the Starter itself returns nothing and the flow does not
    // advance synchronously. The owner drives the outcome back through
    // onCreationSucceeded()/onCreationFailed(). A default-constructed controller
    // has no Starter and completes immediately with a deterministic placeholder
    // code, so the standalone screen and captures work without real services.
    using Starter = std::function<void(const QString &displayName, const QString &handle)>;

    explicit OnboardingController(QObject *parent = nullptr);
    explicit OnboardingController(Starter starter, QObject *parent = nullptr);

    [[nodiscard]] Step step() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString handle() const;
    [[nodiscard]] bool canCreate() const;
    [[nodiscard]] bool creating() const;
    [[nodiscard]] QString recoveryCode() const;
    [[nodiscard]] QString errorText() const;

    Q_INVOKABLE void setDisplayName(const QString &displayName);
    Q_INVOKABLE void setHandle(const QString &handle);

    // From the Create step, if canCreate is true and no creation is already in
    // flight, marks creating, clears the error and invokes the Starter. The flow
    // advances only when the owner reports success through onCreationSucceeded();
    // createProfile() never advances the step itself.
    Q_INVOKABLE void createProfile();

    // From the Recovery step, advances to Done and emits completed() once.
    Q_INVOKABLE void confirmRecoverySaved();

public slots:
    // Owner callbacks that complete an in-flight createProfile(). Succeeded stores
    // the recovery code, clears creating and advances to Recovery; Failed clears
    // creating, stays on Create, surfaces the message and emits creationFailed().
    // Both are no-ops unless a creation is currently in flight, so a late or
    // duplicated callback cannot resurrect or skip the flow.
    void onCreationSucceeded(const QString &recoveryCode);
    void onCreationFailed(const QString &message);

signals:
    void stepChanged();
    void displayNameChanged();
    void handleChanged();
    void canCreateChanged();
    void creatingChanged();
    void recoveryCodeChanged();
    void errorTextChanged();
    void creationFailed();
    void completed();

private:
    // A handle is valid when, after trimming, it is non-empty and contains no
    // whitespace. Kept intentionally simple; the relay enforces the authoritative
    // handle rules during registration in the integration phase.
    [[nodiscard]] static bool handleIsValid(const QString &handle);
    void setStep(Step step);
    void setCreating(bool creating);
    void setRecoveryCode(const QString &code);
    void setErrorText(const QString &text);

    Starter m_starter;
    Step m_step = Step::Create;
    bool m_creating = false;
    QString m_displayName;
    QString m_handle;
    QString m_recoveryCode;
    QString m_errorText;
};

} // namespace OpenChat

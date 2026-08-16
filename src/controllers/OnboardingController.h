#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace OpenChat {

// Drives the first-run onboarding surface: gathering the profile fields,
// creating the account, and revealing the one-time recovery code. The account
// creation itself is injected as a callback so the screens and their tests run
// without a real ProfileSession, KeyVault, or relay; the integration phase
// supplies a Creator that performs ProfileSession::create, relay registration,
// and KeyPackage publish and returns the resulting RecoveryCode as a string.
class OnboardingController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Step step READ step NOTIFY stepChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString handle READ handle WRITE setHandle NOTIFY handleChanged)
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY canCreateChanged)
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

    // Creation seam. Given the trimmed display name and handle, performs account
    // creation and returns the recovery-code string to reveal, or nullopt on
    // failure. The default-constructed controller uses a creator that returns a
    // deterministic placeholder so the screens and captures work standalone.
    using Creator = std::function<std::optional<QString>(const QString &displayName,
                                                         const QString &handle)>;

    explicit OnboardingController(QObject *parent = nullptr);
    explicit OnboardingController(Creator creator, QObject *parent = nullptr);

    [[nodiscard]] Step step() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString handle() const;
    [[nodiscard]] bool canCreate() const;
    [[nodiscard]] QString recoveryCode() const;
    [[nodiscard]] QString errorText() const;

    Q_INVOKABLE void setDisplayName(const QString &displayName);
    Q_INVOKABLE void setHandle(const QString &handle);

    // From the Create step, if canCreate is true, runs the Creator. On success
    // the returned recovery code is stored and the step advances to Recovery; on
    // failure the step stays on Create, an error is surfaced, and creationFailed
    // is emitted. Never advances on failure.
    Q_INVOKABLE void createProfile();

    // From the Recovery step, advances to Done and emits completed() once.
    Q_INVOKABLE void confirmRecoverySaved();

signals:
    void stepChanged();
    void displayNameChanged();
    void handleChanged();
    void canCreateChanged();
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
    void setRecoveryCode(const QString &code);
    void setErrorText(const QString &text);

    Creator m_creator;
    Step m_step = Step::Create;
    QString m_displayName;
    QString m_handle;
    QString m_recoveryCode;
    QString m_errorText;
};

} // namespace OpenChat

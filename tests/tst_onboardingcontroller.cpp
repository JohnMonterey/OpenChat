#include <QtTest>

#include "controllers/OnboardingController.h"

using OpenChat::OnboardingController;
using Mode = OpenChat::OnboardingController::Mode;
using Step = OpenChat::OnboardingController::Step;

namespace {

const QString goodPassword = QStringLiteral("correct horse battery");

// What a Starter was handed, so tests can assert on exactly what left the
// controller.
struct Started {
    int calls = 0;
    Mode mode = Mode::SignUp;
    QString handle;
    QString password;
};

OnboardingController::Starter recordInto(Started &started)
{
    return [&started](Mode mode, const QString &handle, const QString &password) {
        ++started.calls;
        started.mode = mode;
        started.handle = handle;
        // Deep copy: the controller scrubs its own buffer once the Starter returns.
        started.password = QString(password.constData(), password.size());
    };
}

void fillSignUp(OnboardingController &controller, const QString &handle = QStringLiteral("ada"))
{
    controller.setHandle(handle);
    controller.setPassword(goodPassword);
    controller.setPasswordConfirm(goodPassword);
}

} // namespace

class OnboardingControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsToSignUpOnCredentialsStep()
    {
        OnboardingController controller;

        QCOMPARE(controller.step(), Step::Credentials);
        QCOMPARE(controller.mode(), Mode::SignUp);
        QCOMPARE(controller.handle(), QString());
        QCOMPARE(controller.password(), QString());
        QCOMPARE(controller.passwordConfirm(), QString());
        QCOMPARE(controller.recoveryCode(), QString());
        QCOMPARE(controller.errorText(), QString());
        QCOMPARE(controller.notice(), QString());
        QVERIFY(!controller.canSubmit());
        QVERIFY(!controller.busy());
        // Empty fields are not errors: no hint nags before anything is typed.
        QCOMPARE(controller.handleHint(), QString());
        QCOMPARE(controller.passwordHint(), QString());
        QCOMPARE(controller.passwordConfirmHint(), QString());
        QCOMPARE(controller.passwordStrength(), 0);
    }

    void signUpNeedsUsernamePasswordAndMatchingConfirmation()
    {
        OnboardingController controller;
        QSignalSpy canSubmitSpy(&controller, &OnboardingController::canSubmitChanged);

        controller.setHandle(QStringLiteral("ada"));
        QVERIFY(!controller.canSubmit());
        controller.setPassword(goodPassword);
        QVERIFY(!controller.canSubmit()); // not confirmed yet
        controller.setPasswordConfirm(QStringLiteral("correct horse"));
        QVERIFY(!controller.canSubmit());
        QVERIFY(!controller.passwordConfirmHint().isEmpty());
        QCOMPARE(canSubmitSpy.count(), 0);

        controller.setPasswordConfirm(goodPassword);
        QVERIFY(controller.canSubmit());
        QCOMPARE(controller.passwordConfirmHint(), QString());
        QCOMPARE(canSubmitSpy.count(), 1);

        // Editing the password afterwards un-confirms it.
        controller.setPassword(goodPassword + QLatin1Char('!'));
        QVERIFY(!controller.canSubmit());
        QCOMPARE(canSubmitSpy.count(), 2);
    }

    void usernameRulesAreExplained_data()
    {
        QTest::addColumn<QString>("handle");
        QTest::addColumn<bool>("valid");

        QTest::newRow("plain") << QStringLiteral("ada") << true;
        QTest::newRow("at-prefix, capitals, padding") << QStringLiteral("  @Ada.Lovelace-1 ") << true;
        QTest::newRow("too short") << QStringLiteral("ab") << false;
        QTest::newRow("too long") << QString(33, QLatin1Char('a')) << false;
        QTest::newRow("inner space") << QStringLiteral("ada lovelace") << false;
        QTest::newRow("leading dot") << QStringLiteral(".ada") << false;
        QTest::newRow("non-ascii letter") << QString::fromUtf8("\xC3\xA4" "da") << false;
        QTest::newRow("cyrillic look-alike") << QString::fromUtf8("\xD0\xB0" "da") << false;
        QTest::newRow("symbol") << QStringLiteral("ada!") << false;
    }

    void usernameRulesAreExplained()
    {
        QFETCH(QString, handle);
        QFETCH(bool, valid);

        OnboardingController controller;
        fillSignUp(controller, handle);

        QCOMPARE(controller.canSubmit(), valid);
        // An unacceptable username always says why; an acceptable one never nags.
        QCOMPARE(controller.handleHint().isEmpty(), valid);
    }

    void weakPasswordsAreRefusedForNewAccounts_data()
    {
        QTest::addColumn<QString>("password");

        QTest::newRow("too short") << QStringLiteral("short1!");
        QTest::newRow("nine characters") << QStringLiteral("ninechars");
        QTest::newRow("one repeated character") << QString(12, QLatin1Char('a'));
        QTest::newRow("famous") << QStringLiteral("1234567890");
        QTest::newRow("famous, any case") << QStringLiteral("QwertyUiop");
        QTest::newRow("password-prefixed") << QStringLiteral("Password2026!");
        QTest::newRow("built around the username") << QStringLiteral("Ada-ada-ADA-2026");
        QTest::newRow("too long") << QString(257, QLatin1Char('x')) + QStringLiteral("y");
    }

    void weakPasswordsAreRefusedForNewAccounts()
    {
        QFETCH(QString, password);

        OnboardingController controller;
        controller.setHandle(QStringLiteral("ada"));
        controller.setPassword(password);
        controller.setPasswordConfirm(password);

        QVERIFY(!controller.canSubmit());
        QVERIFY(!controller.passwordHint().isEmpty());
        QCOMPARE(controller.passwordStrength(), 1);
    }

    void usernameInsideALongPasswordIsFine()
    {
        OnboardingController controller;
        controller.setHandle(QStringLiteral("ada"));
        controller.setPassword(QStringLiteral("canada goose parade"));
        controller.setPasswordConfirm(QStringLiteral("canada goose parade"));

        QVERIFY(controller.canSubmit());
        QCOMPARE(controller.passwordHint(), QString());
    }

    void strengthGrowsWithLengthAndVariety()
    {
        OnboardingController controller;
        controller.setHandle(QStringLiteral("ada"));

        controller.setPassword(QStringLiteral("tenletters"));
        const int plain = controller.passwordStrength();
        controller.setPassword(QStringLiteral("a much longer passphrase here"));
        const int longer = controller.passwordStrength();
        controller.setPassword(QStringLiteral("Tr1cky&Long-enough"));
        const int varied = controller.passwordStrength();

        QCOMPARE(plain, 2);
        QVERIFY(longer > plain);
        QCOMPARE(varied, 4);
        QVERIFY(longer <= 4);
    }

    void logInAcceptsWhateverPasswordTheAccountHas()
    {
        Started started;
        OnboardingController controller(recordInto(started));
        controller.setMode(Mode::LogIn);
        controller.setHandle(QStringLiteral("@Ada"));

        QVERIFY(!controller.canSubmit()); // a password is still required

        // An account may predate today's rules (or come from another client), so
        // logging in must not apply them, and needs no confirmation.
        controller.setPassword(QStringLiteral("short"));
        QVERIFY(controller.canSubmit());
        QCOMPARE(controller.passwordHint(), QString());

        controller.submit();
        QCOMPARE(started.calls, 1);
        QCOMPARE(started.mode, Mode::LogIn);
        QCOMPARE(started.handle, QStringLiteral("ada")); // canonical form
        QCOMPARE(started.password, QStringLiteral("short"));
    }

    void switchingModeKeepsUsernameAndClearsSecrets()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        QSignalSpy modeSpy(&controller, &OnboardingController::modeChanged);
        fillSignUp(controller);
        controller.submit();
        controller.onSubmitFailed(QStringLiteral("That username is taken."));
        QVERIFY(!controller.errorText().isEmpty());
        controller.setPassword(goodPassword);

        controller.setMode(Mode::LogIn);

        QCOMPARE(modeSpy.count(), 1);
        QCOMPARE(controller.handle(), QStringLiteral("ada"));
        QCOMPARE(controller.password(), QString());
        QCOMPARE(controller.passwordConfirm(), QString());
        QCOMPARE(controller.errorText(), QString());
        QVERIFY(!controller.canSubmit());
    }

    void submitHandsOverThePasswordAndForgetsIt()
    {
        Started started;
        OnboardingController controller(recordInto(started));
        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy busySpy(&controller, &OnboardingController::busyChanged);
        QSignalSpy canSubmitSpy(&controller, &OnboardingController::canSubmitChanged);
        QSignalSpy passwordSpy(&controller, &OnboardingController::passwordChanged);
        QSignalSpy confirmSpy(&controller, &OnboardingController::passwordConfirmChanged);

        fillSignUp(controller, QStringLiteral("  @Ada  "));
        passwordSpy.clear();
        confirmSpy.clear();
        canSubmitSpy.clear();

        controller.submit();

        QCOMPARE(started.calls, 1);
        QCOMPARE(started.mode, Mode::SignUp);
        QCOMPARE(started.handle, QStringLiteral("ada"));
        QCOMPARE(started.password, goodPassword);

        // Gone from the controller -- and, through the change signals, from the
        // bound text fields -- before any network activity could have begun.
        QCOMPARE(controller.password(), QString());
        QCOMPARE(controller.passwordConfirm(), QString());
        QCOMPARE(passwordSpy.count(), 1);
        QCOMPARE(confirmSpy.count(), 1);

        // In flight: busy, not submittable, and the step has NOT advanced.
        QVERIFY(controller.busy());
        QVERIFY(!controller.canSubmit());
        QCOMPARE(busySpy.count(), 1);
        QCOMPARE(canSubmitSpy.count(), 1);
        QCOMPARE(controller.step(), Step::Credentials);
        QCOMPARE(stepSpy.count(), 0);

        // A double-tap cannot start a second flow.
        controller.submit();
        QCOMPARE(started.calls, 1);
    }

    void submitIsIgnoredUntilInputIsValid()
    {
        Started started;
        OnboardingController controller(recordInto(started));

        controller.submit();
        controller.setHandle(QStringLiteral("ada"));
        controller.submit();
        controller.setPassword(goodPassword);
        controller.submit(); // unconfirmed

        QCOMPARE(started.calls, 0);
        QVERIFY(!controller.busy());
        QCOMPARE(controller.step(), Step::Credentials);
    }

    void modeCannotChangeWhileInFlight()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        fillSignUp(controller);
        controller.submit();

        controller.setMode(Mode::LogIn);

        QCOMPARE(controller.mode(), Mode::SignUp);
    }

    void signUpSuccessRevealsRecoveryCode()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy codeSpy(&controller, &OnboardingController::recoveryCodeChanged);
        QSignalSpy completedSpy(&controller, &OnboardingController::completed);

        fillSignUp(controller);
        controller.submit();
        controller.onSubmitSucceeded(QStringLiteral("CODE-1234"));

        QVERIFY(!controller.busy());
        QCOMPARE(controller.step(), Step::Recovery);
        QCOMPARE(controller.recoveryCode(), QStringLiteral("CODE-1234"));
        QCOMPARE(stepSpy.count(), 1);
        QCOMPARE(codeSpy.count(), 1);
        QCOMPARE(completedSpy.count(), 0); // not until the code is acknowledged

        // A duplicated success must not skip or repeat anything.
        controller.onSubmitSucceeded(QStringLiteral("OTHER"));
        QCOMPARE(controller.recoveryCode(), QStringLiteral("CODE-1234"));
        QCOMPARE(stepSpy.count(), 1);
    }

    void logInSuccessCompletesWithoutRecoveryStep()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        QSignalSpy completedSpy(&controller, &OnboardingController::completed);
        controller.setMode(Mode::LogIn);
        controller.setHandle(QStringLiteral("ada"));
        controller.setPassword(goodPassword);

        controller.submit();
        controller.onSubmitSucceeded(QStringLiteral("unused-for-login"));

        // An existing account has nothing new to save: straight into the app.
        QCOMPARE(controller.step(), Step::Done);
        QCOMPARE(controller.recoveryCode(), QString());
        QCOMPARE(completedSpy.count(), 1);

        controller.onSubmitSucceeded(QString());
        controller.confirmRecoverySaved();
        QCOMPARE(completedSpy.count(), 1);
    }

    void failureStaysOnCredentialsAndSurfacesError()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        QSignalSpy failedSpy(&controller, &OnboardingController::submitFailed);
        QSignalSpy errorSpy(&controller, &OnboardingController::errorTextChanged);

        fillSignUp(controller);
        controller.submit();
        controller.onSubmitFailed(QStringLiteral("That username is taken."));

        QVERIFY(!controller.busy());
        QCOMPARE(controller.step(), Step::Credentials);
        QCOMPARE(controller.errorText(), QStringLiteral("That username is taken."));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(errorSpy.count(), 1);
        // The username survives for the retry; the password has to be retyped.
        QCOMPARE(controller.handle(), QStringLiteral("ada"));
        QCOMPARE(controller.password(), QString());
        QVERIFY(!controller.canSubmit());

        // Retrying clears the old error as soon as the new attempt starts.
        controller.setPassword(goodPassword);
        controller.setPasswordConfirm(goodPassword);
        controller.submit();
        QCOMPARE(controller.errorText(), QString());

        // An empty message still tells the user something.
        controller.onSubmitFailed(QString());
        QVERIFY(!controller.errorText().isEmpty());
    }

    void ownerCallbacksAreNoOpsWhenNothingIsInFlight()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy failedSpy(&controller, &OnboardingController::submitFailed);

        controller.onSubmitSucceeded(QStringLiteral("STRAY"));
        controller.onSubmitFailed(QStringLiteral("stray"));

        QCOMPARE(controller.step(), Step::Credentials);
        QCOMPARE(controller.recoveryCode(), QString());
        QCOMPARE(controller.errorText(), QString());
        QCOMPARE(stepSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
    }

    void confirmRecoverySavedCompletesOnce()
    {
        OnboardingController controller([](Mode, const QString &, const QString &) {});
        QSignalSpy completedSpy(&controller, &OnboardingController::completed);

        // Not on the Recovery step yet: nothing happens.
        controller.confirmRecoverySaved();
        QCOMPARE(completedSpy.count(), 0);

        fillSignUp(controller);
        controller.submit();
        controller.onSubmitSucceeded(QStringLiteral("CODE"));
        controller.confirmRecoverySaved();
        QCOMPARE(controller.step(), Step::Done);
        QCOMPARE(completedSpy.count(), 1);

        controller.confirmRecoverySaved();
        QCOMPARE(completedSpy.count(), 1);
    }

    void noticeIsSurfaced()
    {
        OnboardingController controller;
        QSignalSpy noticeSpy(&controller, &OnboardingController::noticeChanged);

        controller.setNotice(QStringLiteral("Old data was erased."));
        controller.setNotice(QStringLiteral("Old data was erased."));

        QCOMPARE(controller.notice(), QStringLiteral("Old data was erased."));
        QCOMPARE(noticeSpy.count(), 1);
    }

    void defaultStarterYieldsPlaceholderCode()
    {
        // The standalone/preview path has no Starter and completes immediately.
        OnboardingController controller;
        fillSignUp(controller);

        controller.submit();

        QCOMPARE(controller.step(), Step::Recovery);
        QVERIFY(!controller.recoveryCode().isEmpty());
        QVERIFY(!controller.busy());
        QCOMPARE(controller.password(), QString());
    }
};

QTEST_MAIN(OnboardingControllerTest)

#include "tst_onboardingcontroller.moc"

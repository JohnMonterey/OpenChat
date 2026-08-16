#include <QtTest>

#include <optional>

#include "controllers/OnboardingController.h"

using OpenChat::OnboardingController;

class OnboardingControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsToCreateStep()
    {
        OnboardingController controller;

        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QVERIFY(controller.displayName().isEmpty());
        QVERIFY(controller.handle().isEmpty());
        QVERIFY(controller.recoveryCode().isEmpty());
        QVERIFY(controller.errorText().isEmpty());
        QVERIFY(!controller.canCreate());
    }

    void canCreateRequiresBothFields()
    {
        OnboardingController controller;
        QSignalSpy canCreateSpy(&controller, &OnboardingController::canCreateChanged);

        controller.setDisplayName(QStringLiteral("Ada Lovelace"));
        QVERIFY(!controller.canCreate());

        // A handle with whitespace is rejected; leading/trailing space is trimmed.
        controller.setHandle(QStringLiteral("ada lovelace"));
        QVERIFY(!controller.canCreate());

        controller.setHandle(QStringLiteral("  ada  "));
        QVERIFY(controller.canCreate());
        QVERIFY(canCreateSpy.count() >= 1);

        // Clearing either field disables creation again.
        controller.setDisplayName(QStringLiteral("   "));
        QVERIFY(!controller.canCreate());
    }

    void successfulCreatorAdvancesToRecovery()
    {
        int calls = 0;
        OnboardingController controller(
            [&calls](const QString &displayName, const QString &handle)
                -> std::optional<QString> {
                ++calls;
                // The Creator receives trimmed field values.
                Q_ASSERT(displayName == QStringLiteral("Ada Lovelace"));
                Q_ASSERT(handle == QStringLiteral("ada"));
                return QStringLiteral("ABCD-1234-EFGH-5678");
            });

        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy recoverySpy(&controller, &OnboardingController::recoveryCodeChanged);
        QSignalSpy failedSpy(&controller, &OnboardingController::creationFailed);
        QSignalSpy completedSpy(&controller, &OnboardingController::completed);

        controller.setDisplayName(QStringLiteral("  Ada Lovelace  "));
        controller.setHandle(QStringLiteral("ada"));
        QVERIFY(controller.canCreate());

        controller.createProfile();

        QCOMPARE(calls, 1);
        QCOMPARE(controller.step(), OnboardingController::Step::Recovery);
        QCOMPARE(controller.recoveryCode(), QStringLiteral("ABCD-1234-EFGH-5678"));
        QCOMPARE(stepSpy.count(), 1);
        QCOMPARE(recoverySpy.count(), 1);
        QVERIFY(controller.errorText().isEmpty());
        // Nothing spurious: no failure, and the flow has not completed yet.
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(completedSpy.count(), 0);
    }

    void createIsIgnoredUntilFieldsValid()
    {
        int calls = 0;
        OnboardingController controller(
            [&calls](const QString &, const QString &) -> std::optional<QString> {
                ++calls;
                return QStringLiteral("SHOULD-NOT-RUN");
            });

        // No fields set: createProfile must not call the Creator or advance.
        controller.createProfile();
        QCOMPARE(calls, 0);
        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QVERIFY(controller.recoveryCode().isEmpty());
    }

    void failingCreatorStaysOnCreateAndSurfacesError()
    {
        OnboardingController controller(
            [](const QString &, const QString &) -> std::optional<QString> {
                return std::nullopt;
            });

        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy failedSpy(&controller, &OnboardingController::creationFailed);
        QSignalSpy errorSpy(&controller, &OnboardingController::errorTextChanged);

        controller.setDisplayName(QStringLiteral("Grace"));
        controller.setHandle(QStringLiteral("grace"));
        QVERIFY(controller.canCreate());

        controller.createProfile();

        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QVERIFY(controller.recoveryCode().isEmpty());
        QVERIFY(!controller.errorText().isEmpty());
        QCOMPARE(stepSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(errorSpy.count() >= 1);
    }

    void confirmRecoverySavedCompletesOnce()
    {
        OnboardingController controller(
            [](const QString &, const QString &) -> std::optional<QString> {
                return QStringLiteral("CODE");
            });
        controller.setDisplayName(QStringLiteral("Alan"));
        controller.setHandle(QStringLiteral("alan"));
        controller.createProfile();
        QCOMPARE(controller.step(), OnboardingController::Step::Recovery);

        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy completedSpy(&controller, &OnboardingController::completed);

        controller.confirmRecoverySaved();
        QCOMPARE(controller.step(), OnboardingController::Step::Done);
        QCOMPARE(stepSpy.count(), 1);
        QCOMPARE(completedSpy.count(), 1);

        // Re-invoking from Done is a no-op and does not complete again.
        controller.confirmRecoverySaved();
        QCOMPARE(controller.step(), OnboardingController::Step::Done);
        QCOMPARE(completedSpy.count(), 1);
    }

    void defaultCreatorYieldsPlaceholderCode()
    {
        // The default-constructed controller must be able to complete a create
        // without any injected Creator, so the standalone screen and captures
        // work: it advances to Recovery with a non-empty placeholder code.
        OnboardingController controller;
        controller.setDisplayName(QStringLiteral("Katherine"));
        controller.setHandle(QStringLiteral("katherine"));
        QVERIFY(controller.canCreate());

        controller.createProfile();
        QCOMPARE(controller.step(), OnboardingController::Step::Recovery);
        QVERIFY(!controller.recoveryCode().isEmpty());
    }
};

QTEST_MAIN(OnboardingControllerTest)

#include "tst_onboardingcontroller.moc"

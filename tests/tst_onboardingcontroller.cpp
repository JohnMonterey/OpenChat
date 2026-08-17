#include <QtTest>

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
        QVERIFY(!controller.creating());
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

    void createStartsAsyncFlowWithoutAdvancing()
    {
        int starts = 0;
        QString startedDisplay;
        QString startedHandle;
        // A Starter that records the call but does NOT complete synchronously, so
        // the in-flight state is observable.
        OnboardingController controller(
            [&](const QString &displayName, const QString &handle) {
                ++starts;
                startedDisplay = displayName;
                startedHandle = handle;
            });

        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy creatingSpy(&controller, &OnboardingController::creatingChanged);
        QSignalSpy failedSpy(&controller, &OnboardingController::creationFailed);

        controller.setDisplayName(QStringLiteral("  Ada Lovelace  "));
        controller.setHandle(QStringLiteral("ada"));
        QVERIFY(controller.canCreate());

        controller.createProfile();

        // The Starter ran once with trimmed field values, creating is true, and
        // the flow has NOT advanced away from Create.
        QCOMPARE(starts, 1);
        QCOMPARE(startedDisplay, QStringLiteral("Ada Lovelace"));
        QCOMPARE(startedHandle, QStringLiteral("ada"));
        QVERIFY(controller.creating());
        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QCOMPARE(creatingSpy.count(), 1);
        QCOMPARE(stepSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);

        // A second createProfile while a creation is in flight is ignored.
        controller.createProfile();
        QCOMPARE(starts, 1);
    }

    void successCallbackAdvancesToRecovery()
    {
        OnboardingController controller(
            [](const QString &, const QString &) { /* async: no synchronous result */ });

        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy creatingSpy(&controller, &OnboardingController::creatingChanged);
        QSignalSpy recoverySpy(&controller, &OnboardingController::recoveryCodeChanged);
        QSignalSpy failedSpy(&controller, &OnboardingController::creationFailed);
        QSignalSpy completedSpy(&controller, &OnboardingController::completed);

        controller.setDisplayName(QStringLiteral("Ada Lovelace"));
        controller.setHandle(QStringLiteral("ada"));
        controller.createProfile();
        QVERIFY(controller.creating());

        controller.onCreationSucceeded(QStringLiteral("ABCD-1234-EFGH-5678"));

        QVERIFY(!controller.creating());
        QCOMPARE(controller.step(), OnboardingController::Step::Recovery);
        QCOMPARE(controller.recoveryCode(), QStringLiteral("ABCD-1234-EFGH-5678"));
        QCOMPARE(stepSpy.count(), 1);
        QCOMPARE(recoverySpy.count(), 1);
        QCOMPARE(creatingSpy.count(), 2); // true then false
        QVERIFY(controller.errorText().isEmpty());
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(completedSpy.count(), 0);
    }

    void createIsIgnoredUntilFieldsValid()
    {
        int starts = 0;
        OnboardingController controller(
            [&starts](const QString &, const QString &) { ++starts; });

        // No fields set: createProfile must not run the Starter or advance.
        controller.createProfile();
        QCOMPARE(starts, 0);
        QVERIFY(!controller.creating());
        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QVERIFY(controller.recoveryCode().isEmpty());
    }

    void failureCallbackStaysOnCreateAndSurfacesError()
    {
        OnboardingController controller(
            [](const QString &, const QString &) { /* async */ });

        QSignalSpy stepSpy(&controller, &OnboardingController::stepChanged);
        QSignalSpy failedSpy(&controller, &OnboardingController::creationFailed);
        QSignalSpy errorSpy(&controller, &OnboardingController::errorTextChanged);
        QSignalSpy creatingSpy(&controller, &OnboardingController::creatingChanged);

        controller.setDisplayName(QStringLiteral("Grace"));
        controller.setHandle(QStringLiteral("grace"));
        controller.createProfile();
        QVERIFY(controller.creating());

        controller.onCreationFailed(QStringLiteral("That handle is unavailable."));

        QVERIFY(!controller.creating());
        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QVERIFY(controller.recoveryCode().isEmpty());
        QCOMPARE(controller.errorText(), QStringLiteral("That handle is unavailable."));
        QCOMPARE(stepSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(errorSpy.count() >= 1);
        QCOMPARE(creatingSpy.count(), 2); // true then false
    }

    void ownerCallbacksAreNoOpsWhenNotCreating()
    {
        OnboardingController controller(
            [](const QString &, const QString &) { /* async */ });

        // Neither callback does anything before a create is in flight.
        controller.onCreationSucceeded(QStringLiteral("STRAY-CODE"));
        QCOMPARE(controller.step(), OnboardingController::Step::Create);
        QVERIFY(controller.recoveryCode().isEmpty());

        controller.onCreationFailed(QStringLiteral("stray"));
        QVERIFY(controller.errorText().isEmpty());
    }

    void confirmRecoverySavedCompletesOnce()
    {
        // The default (no Starter) controller completes creation immediately with
        // a placeholder, so it reaches Recovery without an owner callback.
        OnboardingController controller;
        controller.setDisplayName(QStringLiteral("Alan"));
        controller.setHandle(QStringLiteral("alan"));
        controller.createProfile();
        QCOMPARE(controller.step(), OnboardingController::Step::Recovery);
        QVERIFY(!controller.creating());

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

    void defaultStarterYieldsPlaceholderCode()
    {
        // The default-constructed controller must be able to complete a create
        // without any injected Starter, so the standalone screen and captures
        // work: it advances to Recovery with a non-empty placeholder code.
        OnboardingController controller;
        controller.setDisplayName(QStringLiteral("Katherine"));
        controller.setHandle(QStringLiteral("katherine"));
        QVERIFY(controller.canCreate());

        controller.createProfile();
        QCOMPARE(controller.step(), OnboardingController::Step::Recovery);
        QVERIFY(!controller.creating());
        QVERIFY(!controller.recoveryCode().isEmpty());
    }
};

QTEST_MAIN(OnboardingControllerTest)

#include "tst_onboardingcontroller.moc"

#pragma once

#include "call/CallEngine.h"
#include "call/CallTypes.h"
#include "call/QtScreenCapture.h"
#include "call/QtVideoCapture.h"
#include "models/CallParticipantModel.h"

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace OpenChat {

class ChatController;

// The QML-facing bridge for voice calls.
//
// Dual-world like the other controllers: constructed with no engine it drives a
// scripted preview the --call capture path renders, so the approved call screen
// can be reviewed without a peer, a microphone, or a network. setLiveEngine()
// swaps in the real CallEngine and every property below starts reflecting an
// actual call. The preview seam is never touched once live.
class CallController final : public QObject
{
    Q_OBJECT
    // The lifecycle is exposed to QML as the handful of booleans the view
    // actually branches on rather than as the enum itself: the view never needs
    // to distinguish Dialing from Connecting (both say "not yet talking"), and
    // keeping the enum C++-side means the states can be refined without
    // rewriting bindings.
    //
    // True whenever a call occupies the conversation surface. This is what
    // replaces the conversation header with the call screen.
    Q_PROPERTY(bool cameraEnabled READ cameraEnabled NOTIFY videoChanged)
    Q_PROPERTY(bool remoteCameraEnabled READ remoteCameraEnabled NOTIFY videoChanged)
    Q_PROPERTY(QImage localVideoFrame READ localVideoFrame NOTIFY localVideoChanged)
    Q_PROPERTY(QImage remoteVideoFrame READ remoteVideoFrame NOTIFY remoteVideoChanged)
    Q_PROPERTY(double localVideoAspect READ localVideoAspect NOTIFY videoChanged)
    Q_PROPERTY(double remoteVideoAspect READ remoteVideoAspect NOTIFY videoChanged)
    Q_PROPERTY(QString cameraError READ cameraError NOTIFY videoChanged)
    // Screen sharing. A screen is a second video source beside the camera, not
    // a replacement for it, so these live alongside the camera's properties and
    // both can be true at once.
    //
    // True when this build and this machine can capture a screen at all, so the
    // button can be shown disabled with a reason rather than silently failing.
    Q_PROPERTY(bool screenShareAvailable READ screenShareAvailable NOTIFY screenShareChanged)
    Q_PROPERTY(bool screenShareEnabled READ screenShareEnabled NOTIFY screenShareChanged)
    Q_PROPERTY(QString screenShareError READ screenShareError NOTIFY screenShareChanged)
    // What is being shared, for the caption on the local preview.
    Q_PROPERTY(QString screenShareSourceName READ screenShareSourceName NOTIFY screenShareChanged)
    // True while the peer (or anyone in a group) is sharing a screen with us.
    Q_PROPERTY(bool remoteScreenShareActive READ remoteScreenShareActive NOTIFY screenShareChanged)
    // Holds a ScreenCanvasPtr; QML passes it straight to a CallVideoItem.
    Q_PROPERTY(QVariant remoteScreenCanvas READ remoteScreenCanvas NOTIFY remoteScreenChanged)
    // Who is on the stage. In a group several people could share at once; one
    // of them holds the stage until they stop, and then the next takes it,
    // rather than several desktops competing for the same strip of window.
    Q_PROPERTY(QString remoteScreenSharerName READ remoteScreenSharerName
                   NOTIFY remoteScreenChanged)
    // A deliberately small picture of our own share: enough to confirm what is
    // going out, nowhere near the resolution it is going out at.
    Q_PROPERTY(QImage localScreenPreview READ localScreenPreview NOTIFY localScreenChanged)
    Q_PROPERTY(double remoteScreenAspect READ remoteScreenAspect NOTIFY remoteScreenChanged)
    Q_PROPERTY(bool inCall READ inCall NOTIFY callChanged)
    Q_PROPERTY(bool isIncoming READ isIncoming NOTIFY callChanged)
    // True only while an incoming call is still waiting to be answered, i.e.
    // when the accept/decline pair should be offered.
    Q_PROPERTY(bool isRinging READ isRinging NOTIFY callChanged)
    Q_PROPERTY(bool isActive READ isActive NOTIFY callChanged)
    // True once the call is over and the surface is only reporting why.
    Q_PROPERTY(bool callEnded READ callEnded NOTIFY callChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY callChanged)
    Q_PROPERTY(QString peerName READ peerName NOTIFY callChanged)
    Q_PROPERTY(QString peerAvatarKey READ peerAvatarKey NOTIFY callChanged)
    Q_PROPERTY(QString localName READ localName NOTIFY callChanged)
    Q_PROPERTY(QString localAvatarKey READ localAvatarKey NOTIFY callChanged)
    Q_PROPERTY(bool localSpeaking READ localSpeaking NOTIFY levelsChanged)
    Q_PROPERTY(bool remoteSpeaking READ remoteSpeaking NOTIFY levelsChanged)
    Q_PROPERTY(double localLevel READ localLevel NOTIFY levelsChanged)
    Q_PROPERTY(double remoteLevel READ remoteLevel NOTIFY levelsChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY callChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY durationChanged)
    // True when this build/machine can actually place a call, so the header's
    // call button can explain itself instead of failing silently.
    Q_PROPERTY(bool callsAvailable READ callsAvailable NOTIFY callChanged)
    // Group calls: every other member is a row in `participants`, each with
    // its own picture, name, state ("Ringing…", "Left", ...), speaking ring and
    // camera. The one-to-one properties above keep working for the group's
    // headline (peerName is the group's name).
    Q_PROPERTY(bool isGroupCall READ isGroupCall NOTIFY callChanged)
    Q_PROPERTY(QString groupTitle READ groupTitle NOTIFY callChanged)
    Q_PROPERTY(int joinedCount READ joinedCount NOTIFY callChanged)
    Q_PROPERTY(CallParticipantModel *participants READ participants CONSTANT)

public:
    explicit CallController(QObject *parent = nullptr);
    ~CallController() override;

    [[nodiscard]] CallState callState() const noexcept { return m_state; }
    [[nodiscard]] bool inCall() const noexcept;
    [[nodiscard]] bool isIncoming() const noexcept;
    [[nodiscard]] bool isRinging() const noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] bool callEnded() const noexcept { return m_state == CallState::Ended; }
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString peerName() const { return m_peerName; }
    [[nodiscard]] QString peerAvatarKey() const { return m_peerAvatarKey; }
    [[nodiscard]] QString localName() const { return m_localName; }
    [[nodiscard]] QString localAvatarKey() const { return m_localAvatarKey; }
    [[nodiscard]] bool localSpeaking() const noexcept { return m_localSpeaking; }
    [[nodiscard]] bool remoteSpeaking() const noexcept { return m_remoteSpeaking; }
    [[nodiscard]] double localLevel() const noexcept { return m_localLevel; }
    [[nodiscard]] double remoteLevel() const noexcept { return m_remoteLevel; }
    [[nodiscard]] bool muted() const noexcept { return m_muted; }
    [[nodiscard]] QString durationText() const;
    [[nodiscard]] bool callsAvailable() const noexcept { return m_engine != nullptr; }
    [[nodiscard]] bool isGroupCall() const noexcept { return m_isGroupCall; }
    [[nodiscard]] QString groupTitle() const { return m_groupTitle; }
    [[nodiscard]] int joinedCount() const noexcept { return m_joinedCount; }
    [[nodiscard]] CallParticipantModel *participants() { return &m_participants; }

    // Calls whoever is open in the conversation pane. No-op when nothing is
    // open, when the contact has no reachable device, or when a call is running.
    // An open group chat rings every member.
    Q_INVOKABLE void callCurrentContact(bool video = false);
    Q_INVOKABLE void callContact(const QString &contactId);
    Q_INVOKABLE void acceptCall();
    Q_INVOKABLE void declineCall();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleCamera();

    // Screen sharing. Stopping is unconditional; starting needs a source, so
    // the button asks the view to offer the picker rather than guessing.
    Q_INVOKABLE void toggleScreenShare();
    // The screens and windows that can be shared right now, newly enumerated.
    // Each entry is {name, kind, id}; the index is what startScreenShare takes.
    Q_INVOKABLE QVariantList screenShareSources();
    Q_INVOKABLE void startScreenShare(int sourceIndex);
    Q_INVOKABLE void stopScreenShare();
    // How large an incoming share is actually being displayed. Reported back to
    // the sender, which stops encoding more than this. A zero size means the
    // view is closed and the sender can idle.
    Q_INVOKABLE void setRemoteScreenViewSize(int width, int height);
    Q_INVOKABLE void setParticipantScreenViewSize(const QString &deviceId, int width, int height);
    // A one-line summary of the live share for development builds.
    Q_INVOKABLE QString screenShareDiagnostics() const;

    [[nodiscard]] bool screenShareAvailable() const noexcept;
    [[nodiscard]] bool screenShareEnabled() const noexcept { return m_screenShareEnabled; }
    [[nodiscard]] QString screenShareError() const { return m_screenShareError; }
    [[nodiscard]] QString screenShareSourceName() const { return m_screenShareSourceName; }
    [[nodiscard]] bool remoteScreenShareActive() const;
    [[nodiscard]] QVariant remoteScreenCanvas() const { return QVariant::fromValue(m_remoteScreen); }
    [[nodiscard]] QString remoteScreenSharerName() const { return m_remoteScreenSharerName; }
    [[nodiscard]] QImage localScreenPreview() const { return m_localScreenPreview; }
    [[nodiscard]] double remoteScreenAspect() const;

    bool cameraEnabled() const { return m_cameraEnabled; }
    bool remoteCameraEnabled() const { return !m_remoteVideo.isNull(); }
    QImage localVideoFrame() const { return m_localVideo; }
    QImage remoteVideoFrame() const { return m_remoteVideo; }
    double localVideoAspect() const
    {
        return m_localVideo.isNull() ? 4.0 / 3.0
                                    : double(m_localVideo.width()) / m_localVideo.height();
    }
    double remoteVideoAspect() const
    {
        return m_remoteVideo.isNull() ? 4.0 / 3.0
                                     : double(m_remoteVideo.width()) / m_remoteVideo.height();
    }
    QString cameraError() const { return m_cameraError; }
    void setPreviewVideo(const QImage &local, const QImage &remote);
    // Preview seam for a received screen share: pins a canvas into the call
    // surface without an engine, for the capture path and the QML tests. Never
    // reachable from a live session.
    void setPreviewScreenShare(const ScreenCanvasPtr &canvas, const QString &sharerName);

    Q_INVOKABLE void dismissCall();

    // How this user is shown on the call screen. Set from the profile in a live
    // session and directly in previews; a live session refreshes it whenever the
    // display name changes.
    void setLocalIdentity(const QString &name, const QString &avatarKey);

    // Live seam. Both are borrowed and must outlive this controller.
    void setLiveEngine(CallEngine *engine, ChatController *chats);

    // Preview seam for the --call capture path: pins the call screen into a
    // chosen state with scripted names and speaking indicators. Never reachable
    // from a live session.
    void enableForPreview(CallState state, const QString &peerName,
                          const QString &peerAvatarKey, bool remoteSpeaking,
                          bool localSpeaking);
    // The group form: a scripted roster with per-member states, for the
    // --call-group capture and the QML tests.
    void enableForGroupPreview(CallState state, const QString &title,
                               const QVector<CallParticipantRow> &participants);

signals:
    void callChanged();
    void videoChanged();
    void screenShareChanged();
    void remoteScreenChanged();
    void localScreenChanged();
    // Raised by toggleScreenShare() when a source still has to be chosen. The
    // view answers by showing the picker and calling startScreenShare().
    void screenSourcePickRequested();
    void localVideoChanged();
    void remoteVideoChanged();
    void levelsChanged();
    void durationChanged();
    // Raised when a call arrives, so the app can alert the user.
    void incomingCall();

private:
    void syncFromEngine();
    void syncParticipants();
    void stopCamera();
    void setRemoteVideo(const QImage &image);
    void setRemoteScreen(const ScreenCanvasPtr &canvas);
    // Picks which group member's share is on the stage, keeping whoever is
    // already there until they stop.
    void refreshGroupScreenStage();
    void onScreenFrameCaptured(const ScreenFrameView &view);
    void updateScreenPreview(const ScreenFrameView &view, qint64 nowMs);
    void syncLevels();

    QtVideoCapture m_camera;
    QtScreenCapture m_screenCapture;
    QVector<ScreenShareSource> m_screenSources;
    bool m_screenShareEnabled = false;
    bool m_screenShareUnsupported = false;
    QString m_screenShareError;
    QString m_screenShareSourceName;
    ScreenCanvasPtr m_remoteScreen;
    // Empty in a one-to-one call; the staged member's device in a group.
    QString m_remoteScreenDevice;
    QString m_remoteScreenSharerName;
    QImage m_localScreenPreview;
    qint64 m_lastPreviewMs = 0;
    int m_appliedCaptureFps = 0;
    bool m_cameraEnabled = false;
    QImage m_localVideo;
    QImage m_remoteVideo;
    QString m_cameraError;
    CallEngine *m_engine = nullptr;
    ChatController *m_chats = nullptr;
    QTimer *m_durationTimer = nullptr;
    CallParticipantModel m_participants;

    CallState m_state = CallState::Idle;
    CallEndReason m_endReason = CallEndReason::None;
    CallDirection m_direction = CallDirection::Outgoing;
    QString m_peerName;
    QString m_peerAvatarKey = QStringLiteral("userpfp_none");
    QString m_localName;
    QString m_localAvatarKey = QStringLiteral("userpfp_none");
    bool m_localSpeaking = false;
    bool m_remoteSpeaking = false;
    double m_localLevel = 0.0;
    double m_remoteLevel = 0.0;
    bool m_muted = false;
    qint64 m_durationMs = 0;
    bool m_isGroupCall = false;
    QString m_groupTitle;
    int m_joinedCount = 0;
};

} // namespace OpenChat

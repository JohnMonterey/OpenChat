OpenChat test build for Windows
===============================

Run OpenChat.exe from this folder. Keep the whole folder together.

Closing OpenChat
----------------
Closing the window keeps OpenChat running in the notification area (the
icons by the clock; Windows may tuck it under the ^ arrow), so messages and
calls still reach you. Click the icon to bring the window back, or
right-click it and choose Close to quit. During a call the icon is a green
light that brightens while you talk, and turns red when you are muted.

If OpenChat crashes
-------------------
OpenChat restarts itself and shows what happened. Press "Copy report" and send
the text, or press "Open folder" and send the crash-*.txt and crash-*.dmp
files. Reports are kept in:

    %LOCALAPPDATA%\OpenChat\OpenChat\crashes

If OpenChat freezes and you close it, or it disappears without a window, the
explanation appears the next time you start it.

To see what a crash report looks like without waiting for a real one, run
this from a Command Prompt in this folder:

    OpenChat.exe --crash-test segv

Screen sharing in this build
----------------------------
Shares are now sent as a video stream, so scrolling, videos and games stay
smooth instead of filling in square by square. Both people in the call need
this build (or a later one) for a share to show up.

A share now carries your computer's sound too: videos, games, music. OpenChat's
own sound (the call itself) is never included. "Share sound" in the share
picker turns it off. On Windows it needs Windows 10 version 2004 or later.
While someone shares with sound you will see a speaker in the corner of their
screen: click it to mute, or hover it to change the volume.

If screen sharing does not work
-------------------------------
Open a Command Prompt in this folder and run:

    start /wait OpenChat.exe --screen-share-check

("start /wait" keeps the prompt from coming back before the check has
finished printing.) It lists what can be shared, captures each screen for a
few seconds, says how fast this computer can encode the share, and then
listens to the computer's sound for three seconds. Play some music while it
runs: the "loudest" level it prints should then be well above silence. Send
what it prints, and the capture-*.png pictures it names, to whoever gave you
this build. The check never prints window titles; the pictures show your
screen, so look at them before sending.

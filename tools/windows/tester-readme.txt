OpenChat test build for Windows
===============================

Run OpenChat.exe from this folder. Keep the whole folder together.

If OpenChat crashes
-------------------
OpenChat restarts itself and shows what happened. Press "Copy report" and send
the text, or press "Open folder" and send the crash-*.txt and crash-*.dmp
files. Reports are kept in:

    %LOCALAPPDATA%\OpenChat\OpenChat\crashes

If OpenChat freezes and you close it, or it disappears without a window, the
explanation appears the next time you start it.

Starting OpenChat again while it is open brings the open window to the front;
it does not start a second copy. If OpenChat cannot start at all, it says why
in a small window.

If OpenChat takes ten seconds or more to show its window, it notes which step
of starting up it was waiting on: a freeze-*.txt file appears in the crashes
folder above. Please send it, even if OpenChat opened in the end.

To see what a crash report looks like without waiting for a real one, run
this from a Command Prompt in this folder:

    OpenChat.exe --crash-test segv

Screen sharing in this build
----------------------------
Shares are now sent as a video stream, so scrolling, videos and games stay
smooth instead of filling in square by square. Both people in the call need
this build (or a later one) for a share to show up.

If screen sharing does not work
-------------------------------
Open a Command Prompt in this folder and run:

    OpenChat.exe --screen-share-check

It lists what can be shared, captures each screen for a few seconds, and
says how fast this computer can encode the share. Send
what it prints, and the capture-*.png pictures it names, to whoever gave you
this build. The check never prints window titles; the pictures show your
screen, so look at them before sending.

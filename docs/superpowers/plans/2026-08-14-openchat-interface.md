# OpenChat Interface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and visually verify a functional local C++/Qt OpenChat prototype that faithfully reproduces the supplied Windows 7 Aero chat reference.

**Architecture:** Qt Quick/QML renders the window and interface while focused C++ models own contacts, messages, filtering, selection, and sending. A C++ `QQuickPaintedItem` draws each message bubble and tail as one `QPainterPath`, eliminating tail seams at arbitrary sizes and display scales.

**Tech Stack:** C++20, CMake 3.21+, Qt 6.5+ Core/Gui/Quick/QuickControls2/Svg/Test/QuickTest, QML, SVG resources

**Spec:** `docs/superpowers/specs/2026-08-14-openchat-interface-design.md`

## Global Constraints

- Product name is exactly `OpenChat`; do not change any other approved reference content.
- Reference window is 860 × 680 logical pixels with a 720 × 560 minimum.
- The interface is a cross-platform, local-data prototype with no accounts, networking, persistence, voice, or video.
- Only Favorites and Contacts category boundaries divide the contact list; contact rows have no separators.
- Message bubbles use one closed painted path for body and tail, cap width at `min(360 px, 68% of history width)`, and wrap long text by words.
- Video and phone controls use replaceable SVG silhouettes, never font glyphs.
- QML owns presentation only; C++ owns deterministic data and behavior.
- Every coherent visual pass is built, launched, captured, and checked against the supplied PNG.

---

## File Map

- `CMakeLists.txt` — project configuration, Qt discovery, application, and test targets.
- `src/main.cpp` — application startup, QML registration, command-line capture mode, and load failure handling.
- `src/app/AppMetadata.h` — canonical application identity and geometry constants.
- `src/models/Contact.h` — contact value type and presence enum.
- `src/models/ContactListModel.{h,cpp}` — Qt contact roles, grouping, filtering, and selection.
- `src/models/Message.h` — message value type and direction enum.
- `src/models/MessageListModel.{h,cpp}` — Qt message roles and append behavior.
- `src/controllers/ChatController.{h,cpp}` — selected contact, query, composer, send action, and seeded prototype data.
- `src/render/BubbleBackground.{h,cpp}` — single-path bubble/tail geometry and painting.
- `qml/OpenChat/Main.qml` — transparent top-level window and pane composition.
- `qml/OpenChat/Theme.qml` — centralized geometry, palette, typography, and gradient values.
- System window decorations — supplied by the active platform theme (AeroThemePlasma on the development machine), not painted in QML.
- `qml/OpenChat/components/ContactSidebar.qml` — current user, search, groups, and bottom actions.
- `qml/OpenChat/components/ContactCategory.qml` — category boundary and rows without per-row rules.
- `qml/OpenChat/components/ContactRow.qml` — contact thumbnail, presence, labels, and selection wash.
- `qml/OpenChat/components/ConversationHeader.qml` — avatar, identity, SVG call controls, and dropdown.
- `qml/OpenChat/components/MessageHistory.qml` — date rule, list layout, and end positioning.
- `qml/OpenChat/components/MessageDelegate.qml` — direction-aware content sizing and painted background.
- `qml/OpenChat/components/Composer.qml` — input, dropdown segment, and Send state.
- `qml/OpenChat/components/PresenceBead.qml` — green, amber, and gray glossy beads.
- `qml/OpenChat/components/Avatar.qml` — deterministic avatar artwork with neutral fallback.
- `assets/icons/video-call.svg`, `assets/icons/phone-call.svg`, `assets/icons/chevron-down.svg` — replaceable vector controls.
- `tests/tst_models.cpp` — contact and message model behavior.
- `tests/tst_chatcontroller.cpp` — selection, search, composer, and send behavior.
- `tests/tst_bubblebackground.cpp` — closed geometry, bounds, mirroring, and variable sizes.
- `tests/tst_qmlload.cpp` — full UI load, object identities, minimum sizing, contact structure, and composer interaction.
- `tools/capture_openchat.sh` — deterministic launch and screenshot command.

---

### Task 1: Buildable Qt application shell

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/app/AppMetadata.h`
- Create: `src/main.cpp`
- Create: `qml/OpenChat/Main.qml`
- Create: `qml/OpenChat/Theme.qml`
- Create: `tests/tst_appmetadata.cpp`

**Interfaces:**
- Produces: `OpenChat::AppMetadata::{name, defaultWidth, defaultHeight, minimumWidth, minimumHeight}`.
- Produces: QML module `OpenChat` and executable target `OpenChat`.

- [ ] **Step 1: Write the failing metadata test**

```cpp
#include <QtTest>
#include "app/AppMetadata.h"

class AppMetadataTest final : public QObject {
    Q_OBJECT
private slots:
    void approvedIdentity() {
        QCOMPARE(OpenChat::AppMetadata::name, QStringView(u"OpenChat"));
        QCOMPARE(OpenChat::AppMetadata::defaultWidth, 860);
        QCOMPARE(OpenChat::AppMetadata::defaultHeight, 680);
        QCOMPARE(OpenChat::AppMetadata::minimumWidth, 720);
        QCOMPARE(OpenChat::AppMetadata::minimumHeight, 560);
    }
};
QTEST_MAIN(AppMetadataTest)
#include "tst_appmetadata.moc"
```

- [ ] **Step 2: Configure and verify failure**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j2`

Expected: compilation fails because `app/AppMetadata.h` does not exist.

- [ ] **Step 3: Add the minimal project and metadata**

```cpp
namespace OpenChat::AppMetadata {
inline constexpr auto name = QStringView(u"OpenChat");
inline constexpr int defaultWidth = 860;
inline constexpr int defaultHeight = 680;
inline constexpr int minimumWidth = 720;
inline constexpr int minimumHeight = 560;
}
```

Configure CMake with `qt_standard_project_setup(REQUIRES 6.5)`, one `qt_add_executable(OpenChat ...)`, one `qt_add_qml_module` with URI `OpenChat`, Qt resources, `enable_testing()`, and QtTest targets.

- [ ] **Step 4: Add startup and a minimal transparent window**

`main.cpp` creates `QGuiApplication`, sets organization/application names, registers later C++ types under `OpenChat.Native 1.0`, loads `qrc:/qt/qml/OpenChat/Main.qml`, and returns `EXIT_FAILURE` if no root object exists. `Main.qml` binds all size constants and displays the approved product name.

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: build succeeds and `tst_appmetadata` passes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/app/AppMetadata.h src/main.cpp qml/OpenChat/Main.qml qml/OpenChat/Theme.qml tests/tst_appmetadata.cpp
git commit -m "feat: create OpenChat Qt application shell"
```

### Task 2: Typed contact and message models

**Files:**
- Create: `src/models/Contact.h`
- Create: `src/models/ContactListModel.h`
- Create: `src/models/ContactListModel.cpp`
- Create: `src/models/Message.h`
- Create: `src/models/MessageListModel.h`
- Create: `src/models/MessageListModel.cpp`
- Create: `tests/tst_models.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `enum class Presence { Available, Away, Offline }`.
- Produces: `ContactListModel::{setContacts, setQuery, selectContact, contactAt}` and roles `name`, `statusText`, `presence`, `favorite`, `selected`, `avatarKey`.
- Produces: `MessageListModel::{setMessages, appendOutgoing, messageAt}` and roles `direction`, `body`, `timestamp`, `kind`.

- [ ] **Step 1: Write failing model tests**

```cpp
void ModelsTest::contactsExposeApprovedGroups() {
    ContactListModel model;
    model.setContacts({Contact{"michael", "Michael", Presence::Available, true, "landscape"},
                       Contact{"tom", "Tom", Presence::Offline, false, "mono"}});
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0), ContactListModel::FavoriteRole).toBool(), true);
    QCOMPARE(model.data(model.index(1), ContactListModel::StatusTextRole).toString(), "Offline");
}

void ModelsTest::searchAndSelectionAreDeterministic() {
    ContactListModel model;
    model.setContacts(seedContacts());
    model.setQuery("sar");
    QCOMPARE(model.rowCount(), 1);
    model.setQuery({});
    QVERIFY(model.selectContact("michael"));
    QCOMPARE(model.data(model.index(0), ContactListModel::SelectedRole).toBool(), true);
}

void ModelsTest::outgoingMessageAppends() {
    MessageListModel model;
    QVERIFY(model.appendOutgoing("Hello", QTime(10, 19)));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), MessageListModel::BodyRole).toString(), "Hello");
}
```

- [ ] **Step 2: Build and verify failures**

Run: `cmake --build build -j2`

Expected: compilation fails because the model types do not exist.

- [ ] **Step 3: Implement value types and `QAbstractListModel` roles**

Implement explicit `roleNames()`, bounds-checked `data()`, reset notifications for filtering, and targeted `dataChanged` notifications for selection. `appendOutgoing` rejects trimmed-empty bodies and stores the trimmed result.

- [ ] **Step 4: Run model tests**

Run: `ctest --test-dir build -R tst_models --output-on-failure`

Expected: all model tests pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/models tests/tst_models.cpp
git commit -m "feat: add contact and message models"
```

### Task 3: Controller and deterministic prototype behavior

**Files:**
- Create: `src/controllers/ChatController.h`
- Create: `src/controllers/ChatController.cpp`
- Create: `tests/tst_chatcontroller.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ContactListModel`, `MessageListModel`.
- Produces: QML properties `contacts`, `messages`, `currentContactName`, `currentStatusText`, `currentAvatarKey`, `composerText`, `canSend`, `searchQuery`.
- Produces invokables `selectContact(QString)`, `setSearchQuery(QString)`, `setComposerText(QString)`, and `sendMessage()`.

- [ ] **Step 1: Write failing controller tests**

```cpp
void ChatControllerTest::startsOnMichael() {
    ChatController controller;
    QCOMPARE(controller.currentContactName(), "Michael");
    QCOMPARE(controller.messages()->rowCount(), 5);
}

void ChatControllerTest::whitespaceCannotSend() {
    ChatController controller;
    controller.setComposerText("   ");
    QVERIFY(!controller.canSend());
    QVERIFY(!controller.sendMessage());
}

void ChatControllerTest::sendTrimsAndClears() {
    ChatController controller;
    controller.setComposerText("  A local message  ");
    const auto before = controller.messages()->rowCount();
    QVERIFY(controller.sendMessage());
    QCOMPARE(controller.messages()->rowCount(), before + 1);
    QCOMPARE(controller.composerText(), QString());
}
```

- [ ] **Step 2: Build and verify failure**

Run: `cmake --build build -j2`

Expected: compilation fails because `ChatController` does not exist.

- [ ] **Step 3: Implement the controller and exact seed content**

Seed Daniel as the local user; Favorites Michael and Sarah; Contacts Alex, Jessica, Ryan, and Tom; and the five reference messages dated May 24, 2010. Emit one property notification per actual change. Expose the controller as context property `chatController` before QML loads.

- [ ] **Step 4: Run controller and full tests**

Run: `ctest --test-dir build --output-on-failure`

Expected: metadata, model, and controller tests pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/main.cpp src/controllers tests/tst_chatcontroller.cpp
git commit -m "feat: add local OpenChat controller"
```

### Task 4: Seamless variable-size bubble renderer

**Files:**
- Create: `src/render/BubbleBackground.h`
- Create: `src/render/BubbleBackground.cpp`
- Create: `tests/tst_bubblebackground.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `BubbleBackground : QQuickPaintedItem` with QML properties `outgoing`, `radius`, `tailWidth`, `tailHeight`, `fillTop`, `fillBottom`, and `strokeColor`.
- Produces: `static QPainterPath makePath(QRectF bounds, bool outgoing, qreal radius, qreal tailWidth, qreal tailHeight)` for deterministic tests.

- [ ] **Step 1: Write failing path tests**

```cpp
void BubbleBackgroundTest::pathStaysInsideBounds() {
    const QRectF bounds(0, 0, 320, 94);
    const auto path = BubbleBackground::makePath(bounds, false, 6, 9, 13);
    QVERIFY(bounds.adjusted(-0.01, -0.01, 0.01, 0.01).contains(path.boundingRect()));
    QVERIFY(path.contains(QPointF(10, 80)));
}

void BubbleBackgroundTest::directionsMirror() {
    const QRectF bounds(0, 0, 240, 70);
    const auto incoming = BubbleBackground::makePath(bounds, false, 6, 9, 13).toFillPolygon();
    const auto outgoing = BubbleBackground::makePath(bounds, true, 6, 9, 13).toFillPolygon();
    QCOMPARE(incoming.boundingRect().size(), outgoing.boundingRect().size());
}

void BubbleBackgroundTest::tinyAndTallPathsRemainValid() {
    for (const QSizeF size : {QSizeF(110, 42), QSizeF(360, 58), QSizeF(360, 180)})
        QVERIFY(!BubbleBackground::makePath(QRectF(QPointF(), size), false, 6, 9, 13).isEmpty());
}
```

- [ ] **Step 2: Build and verify failure**

Run: `cmake --build build -j2`

Expected: compilation fails because `BubbleBackground` does not exist.

- [ ] **Step 3: Implement one closed body-and-tail path**

Build the path clockwise from the tail-side top corner. Incoming paths reserve `tailWidth` on the left and join the tail at `height - radius - tailHeight`; outgoing paths mirror the x coordinates. Close the subpath once. Paint the same path once with a vertical `QLinearGradient` and once with a 1-pixel cosmetic pen.

- [ ] **Step 4: Register and test the painted item**

Register with `qmlRegisterType<BubbleBackground>("OpenChat.Native", 1, 0, "BubbleBackground")` and run `ctest --test-dir build -R tst_bubblebackground --output-on-failure`.

Expected: all path tests pass at minimum, typical, maximum, and wrapped heights.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/main.cpp src/render tests/tst_bubblebackground.cpp
git commit -m "feat: draw seamless message bubbles"
```

### Task 5: System window chrome and contact pane

**Files:**
- Create: `qml/OpenChat/components/PresenceBead.qml`
- Create: `qml/OpenChat/components/Avatar.qml`
- Create: `qml/OpenChat/components/ContactSidebar.qml`
- Create: `qml/OpenChat/components/ContactCategory.qml`
- Create: `qml/OpenChat/components/ContactRow.qml`
- Modify: `qml/OpenChat/Main.qml`
- Modify: `qml/OpenChat/Theme.qml`
- Create: `tests/tst_qmlload.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `chatController.contacts`, `selectContact`, and `setSearchQuery`.
- Produces object names: `openChatWindow`, `contactSidebar`, `favoritesCategory`, `contactsCategory`, `contactSearch`.

- [ ] **Step 1: Write the failing QML-load smoke test**

```cpp
void QmlLoadTest::requiredStructure() {
    ChatController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("chatController", &controller);
    engine.loadFromModule("OpenChat", "Main");
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *root = engine.rootObjects().constFirst();
    QCOMPARE(root->property("minimumWidth").toInt(), 720);
    QCOMPARE(root->property("minimumHeight").toInt(), 560);
    QVERIFY(!(qobject_cast<QWindow *>(root)->flags() & Qt::FramelessWindowHint));
    QVERIFY(root->findChild<QObject *>("contactSidebar"));
    QVERIFY(root->findChild<QObject *>("favoritesCategory"));
    QVERIFY(root->findChild<QObject *>("contactsCategory"));
}
```

Give this QtTest target a manual `QGuiApplication` main and set its CTest environment to `QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software`.

- [ ] **Step 2: Run QML test and verify failure**

Run: `ctest --test-dir build -R tst_qmlload --output-on-failure`

Expected: failure because required contact-pane component object names do not exist.

- [ ] **Step 3: Implement the frame and contact pane**

Use a normal decorated `Window`; do not set `FramelessWindowHint` and do not draw caption buttons in QML. The active platform theme owns movement, resizing, window actions, border, and shadow. Implement category header rules only. `ContactRow` has `border.width: 0` and no bottom rule; its selected wash is a two-stop horizontal gradient.

- [ ] **Step 4: Run tests and launch the shell**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure && ./build/OpenChat`

Expected: the real Qt window opens with the system frame, search, six contacts, category-only separators, and functional selection/filtering.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt qml/OpenChat tests/tst_qmlload.cpp
git commit -m "feat: build Aero frame and contact pane"
```

### Task 6: Conversation, call vectors, and composer

**Files:**
- Create: `qml/OpenChat/components/ConversationHeader.qml`
- Create: `qml/OpenChat/components/MessageHistory.qml`
- Create: `qml/OpenChat/components/MessageDelegate.qml`
- Create: `qml/OpenChat/components/Composer.qml`
- Create: `assets/icons/video-call.svg`
- Create: `assets/icons/phone-call.svg`
- Create: `assets/icons/chevron-down.svg`
- Modify: `qml/OpenChat/Main.qml`
- Modify: `CMakeLists.txt`
- Modify: `tests/tst_qmlload.cpp`

**Interfaces:**
- Consumes: all current-contact properties, `chatController.messages`, `composerText`, `canSend`, and `sendMessage()`.
- Produces object names: `conversationHeader`, `videoCallButton`, `phoneCallButton`, `messageHistory`, `messageComposer`, `messageInput`, `sendButton`.

- [ ] **Step 1: Extend the failing QML test**

```cpp
void QmlLoadTest::conversationStructure() {
    ChatController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("chatController", &controller);
    engine.loadFromModule("OpenChat", "Main");
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *root = engine.rootObjects().constFirst();
    QVERIFY(root->findChild<QObject *>("conversationHeader"));
    QVERIFY(root->findChild<QObject *>("videoCallButton"));
    QVERIFY(root->findChild<QObject *>("phoneCallButton"));
    auto *input = root->findChild<QObject *>("messageInput");
    auto *send = root->findChild<QObject *>("sendButton");
    QVERIFY(input);
    QVERIFY(send);
    QCOMPARE(send->property("enabled").toBool(), false);
}
```

- [ ] **Step 2: Run and verify failure**

Run: `ctest --test-dir build -R tst_qmlload --output-on-failure`

Expected: failure because conversation components do not exist.

- [ ] **Step 3: Implement the exact header and SVG controls**

Create 24 × 18 video and 25 × 25 handset silhouettes in view boxes with `fill="#415570"`. Keep each SVG file isolated so the user's final assets can replace it without QML changes. Match reference spacing: 68-pixel avatar, 18-pixel gap, right controls separated by 28 pixels.

- [ ] **Step 4: Implement wrapping message delegates**

Set content width with `Math.min(messageText.implicitWidth + horizontalPadding * 2, Math.min(360, history.width * 0.68))`; set `Text.wrapMode: Text.Wrap`; let implicit height derive from painted text plus vertical padding. Bind the `BubbleBackground` to the delegate's complete width and height.

- [ ] **Step 5: Implement and test composer behavior**

Bind input text and Send enabled state to the controller. Enter and Send invoke `sendMessage()`, then position the history at the end. Extend `tst_qmlload.cpp` to call `input->setProperty("text", "Hello")`, verify `send->property("enabled")`, invoke `send.clicked()`, and compare the controller message count increase.

- [ ] **Step 6: Run full tests and launch**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure && ./build/OpenChat`

Expected: exact seeded conversation, connected bubble tails, correct call silhouettes, text wrapping, and functional local sending.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt qml/OpenChat assets/icons tests/tst_qmlload.cpp
git commit -m "feat: complete OpenChat conversation interface"
```

### Task 7: Deterministic capture and visual convergence

**Files:**
- Modify: `src/main.cpp`
- Create: `tools/capture_openchat.sh`
- Create: `tests/visual/.gitkeep`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces CLI: `OpenChat --capture /absolute/output.png --capture-delay 500`.
- Produces command: `./tools/capture_openchat.sh build/openchat-reference.png`.

- [ ] **Step 1: Add a failing capture-mode test**

Add CTest `capture_smoke` that runs:

```bash
OpenChat --capture ${CMAKE_BINARY_DIR}/openchat-capture.png --capture-delay 100
```

and a CMake script that fails unless the PNG exists and is larger than 10 KiB.

- [ ] **Step 2: Run and verify failure**

Run: `ctest --test-dir build -R capture_smoke --output-on-failure`

Expected: failure because capture arguments are not implemented and no PNG is created.

- [ ] **Step 3: Implement capture mode**

Parse arguments with `QCommandLineParser`. After QML loads, cast the root object to `QQuickWindow`, wait for the requested delay with `QTimer::singleShot`, save `window->grabWindow()` to the absolute path, and exit with code 0 only when `QImage::save()` succeeds.

- [ ] **Step 4: Run automated capture**

Run: `cmake --build build -j2 && ctest --test-dir build -R capture_smoke --output-on-failure && ./tools/capture_openchat.sh build/openchat-reference.png`

Expected: a nonempty 860 × 680 PNG of the actual Qt application.

- [ ] **Step 5: Compare against the supplied reference and refine**

Inspect both images side by side and record/fix discrepancies in this order: outer frame and title bar; pane split; vertical regions; category-only separators; contact density; header alignment; date rule; bubble dimensions and tails; composer; call icons; shadows and color. After every patch, rebuild, capture, and inspect again.

- [ ] **Step 6: Validate resize boundaries**

Capture 720 × 560 and 1024 × 768 variants by adding `--width` and `--height` capture arguments. Verify both panes remain visible, text wraps, category rules stay intact, and no control clips.

- [ ] **Step 7: Run final verification and launch on the computer**

Run: `git diff --check && cmake --build build -j2 && ctest --test-dir build --output-on-failure && ./build/OpenChat`

Expected: all tests pass and the user sees the actual running Qt application.

- [ ] **Step 8: Commit the verified state**

```bash
git add CMakeLists.txt src/main.cpp tools tests/visual
git commit -m "stable: verify OpenChat interface"
```

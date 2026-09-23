#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantMap>

class QTextDocument;

namespace OpenChat {

// The line editing code editors are loved for, for the message composer's
// TextEdit. Each command works on the lines the selection touches (only the
// cursor's line when nothing is selected), changes the text in a single undo
// step, and returns the selection to show afterwards as {start, end}. A
// command that would take the text past maxLength does nothing.
class ComposerEditing : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int maxLength READ maxLength WRITE setMaxLength NOTIFY maxLengthChanged)
public:
    explicit ComposerEditing(QObject *parent = nullptr);

    [[nodiscard]] QQuickTextDocument *document() const { return m_quickDocument; }
    void setDocument(QQuickTextDocument *document);
    // For callers holding the QTextDocument itself.
    void setTextDocument(QTextDocument *document);
    [[nodiscard]] int maxLength() const { return m_maxLength; }
    void setMaxLength(int length);

    // Shift+Enter: a line break that keeps the line's indentation.
    Q_INVOKABLE QVariantMap newLine(int start, int end);
    // Ctrl+Enter, Ctrl+Shift+Enter: an empty line below or above, indented
    // like this one, wherever the cursor is in it.
    Q_INVOKABLE QVariantMap insertLine(int start, int end, bool above);
    // Tab: spaces to the next tab stop, or every line indented when the
    // selection spans more than one.
    Q_INVOKABLE QVariantMap tab(int start, int end);
    // Ctrl+], and Ctrl+[ or Shift+Tab: the lines one level in or out.
    Q_INVOKABLE QVariantMap indentLines(int start, int end);
    Q_INVOKABLE QVariantMap outdentLines(int start, int end);
    // Alt+Up, Alt+Down: the lines swapped with the one above or below.
    Q_INVOKABLE QVariantMap moveLines(int start, int end, bool up);
    // Shift+Alt+Up, Shift+Alt+Down: the lines duplicated above or below.
    Q_INVOKABLE QVariantMap copyLines(int start, int end, bool up);
    // Ctrl+Shift+K: the lines removed.
    Q_INVOKABLE QVariantMap deleteLines(int start, int end);
    // Ctrl+L: the whole lines selected; again, the next line too.
    Q_INVOKABLE QVariantMap selectLines(int start, int end);
    // Ctrl+C, Ctrl+X with nothing selected: the cursor's line and its break
    // to the clipboard; cutting takes the line out as well.
    Q_INVOKABLE QVariantMap copyLine(int position);
    Q_INVOKABLE QVariantMap cutLine(int position);

    // One indentation level.
    static constexpr int indentWidth = 4;

signals:
    void documentChanged();
    void maxLengthChanged();

private:
    [[nodiscard]] QString text() const;
    // Replaces [from, to) with `replacement` as one undo step; false (and
    // nothing changed) without a document or past maxLength.
    bool replace(int from, int to, const QString &replacement);

    QPointer<QQuickTextDocument> m_quickDocument;
    QPointer<QTextDocument> m_document;
    int m_maxLength = -1;
};

} // namespace OpenChat

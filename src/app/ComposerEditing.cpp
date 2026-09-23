#include "app/ComposerEditing.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QQuickTextDocument>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>

namespace OpenChat {

namespace {

// Lines are the runs of text between '\n's. Positions are indices into the
// plain text, which for a plain-text document are the document's own.
int lineStart(const QString &text, int position)
{
    return position <= 0 ? 0 : int(text.lastIndexOf(QLatin1Char('\n'), position - 1)) + 1;
}

int lineEnd(const QString &text, int position)
{
    const qsizetype found = text.indexOf(QLatin1Char('\n'), position);
    return found < 0 ? int(text.size()) : int(found);
}

// The lines a selection touches, as [first, last): from the start of the
// first to the end of the last, without its break. A selection that ends at
// the very start of a line leaves that line out, as editors do.
struct Lines final {
    int first = 0;
    int last = 0;
};

Lines touchedLines(const QString &text, int start, int end)
{
    const int lastPosition = end > start && end == lineStart(text, end) ? end - 1 : end;
    return {lineStart(text, start), lineEnd(text, lastPosition)};
}

QString indentationOf(const QString &text, int lineBegin)
{
    int end = lineBegin;
    while (end < text.size() && (text.at(end) == QLatin1Char(' ') || text.at(end) == QLatin1Char('\t')))
        ++end;
    return text.mid(lineBegin, end - lineBegin);
}

void order(const QString &text, int &start, int &end)
{
    start = std::clamp(start, 0, int(text.size()));
    end = std::clamp(end, 0, int(text.size()));
    if (start > end)
        std::swap(start, end);
}

QVariantMap selection(int start, int end)
{
    return {{QStringLiteral("start"), start}, {QStringLiteral("end"), end}};
}

QVariantMap cursorAt(int position)
{
    return selection(position, position);
}

} // namespace

ComposerEditing::ComposerEditing(QObject *parent)
    : QObject(parent)
{
}

void ComposerEditing::setDocument(QQuickTextDocument *document)
{
    if (m_quickDocument == document)
        return;
    m_quickDocument = document;
    m_document = document != nullptr ? document->textDocument() : nullptr;
    emit documentChanged();
}

void ComposerEditing::setTextDocument(QTextDocument *document)
{
    m_quickDocument = nullptr;
    m_document = document;
    emit documentChanged();
}

void ComposerEditing::setMaxLength(int length)
{
    if (m_maxLength == length)
        return;
    m_maxLength = length;
    emit maxLengthChanged();
}

QString ComposerEditing::text() const
{
    return m_document != nullptr ? m_document->toPlainText() : QString();
}

bool ComposerEditing::replace(int from, int to, const QString &replacement)
{
    if (m_document == nullptr)
        return false;
    const int length = m_document->characterCount() - 1;
    if (m_maxLength >= 0 && length - (to - from) + replacement.size() > m_maxLength)
        return false;
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(from);
    cursor.setPosition(to, QTextCursor::KeepAnchor);
    cursor.insertText(replacement);
    cursor.endEditBlock();
    return true;
}

QVariantMap ComposerEditing::newLine(int start, int end)
{
    const QString all = text();
    order(all, start, end);
    const int begin = lineStart(all, start);
    // Only as much indentation as lies before the cursor carries over.
    const QString inserted = QLatin1Char('\n') + indentationOf(all, begin).left(start - begin);
    if (!replace(start, end, inserted))
        return selection(start, end);
    return cursorAt(start + int(inserted.size()));
}

QVariantMap ComposerEditing::insertLine(int start, int end, bool above)
{
    const QString all = text();
    order(all, start, end);
    if (above) {
        const int at = lineStart(all, start);
        const QString indent = indentationOf(all, at);
        if (!replace(at, at, indent + QLatin1Char('\n')))
            return selection(start, end);
        return cursorAt(at + int(indent.size()));
    }
    const int at = touchedLines(all, start, end).last;
    const QString indent = indentationOf(all, lineStart(all, at));
    if (!replace(at, at, QLatin1Char('\n') + indent))
        return selection(start, end);
    return cursorAt(at + 1 + int(indent.size()));
}

QVariantMap ComposerEditing::tab(int start, int end)
{
    const QString all = text();
    order(all, start, end);
    if (all.mid(start, end - start).contains(QLatin1Char('\n')))
        return indentLines(start, end);
    const int column = start - lineStart(all, start);
    const QString spaces(indentWidth - column % indentWidth, QLatin1Char(' '));
    if (!replace(start, end, spaces))
        return selection(start, end);
    return cursorAt(start + int(spaces.size()));
}

QVariantMap ComposerEditing::indentLines(int start, int end)
{
    const QString all = text();
    order(all, start, end);
    const Lines lines = touchedLines(all, start, end);
    QStringList rows = all.mid(lines.first, lines.last - lines.first).split(QLatin1Char('\n'));
    const QString level(indentWidth, QLatin1Char(' '));
    // Blank lines stay blank.
    for (QString &row : rows)
        if (!row.isEmpty())
            row.prepend(level);
    const QString block = rows.join(QLatin1Char('\n'));
    if (!replace(lines.first, lines.last, block))
        return selection(start, end);
    // A lone cursor travels with its text; a selection covers the whole lines.
    if (start == end)
        return cursorAt(start + (lines.first == lines.last ? 0 : indentWidth));
    const bool broken = lines.last < all.size();
    return selection(lines.first, lines.first + int(block.size()) + (broken ? 1 : 0));
}

QVariantMap ComposerEditing::outdentLines(int start, int end)
{
    const QString all = text();
    order(all, start, end);
    const Lines lines = touchedLines(all, start, end);
    QStringList rows = all.mid(lines.first, lines.last - lines.first).split(QLatin1Char('\n'));
    int removedFromFirst = 0;
    for (qsizetype i = 0; i < rows.size(); ++i) {
        QString &row = rows[i];
        int removed = 0;
        if (row.startsWith(QLatin1Char('\t'))) {
            removed = 1;
        } else {
            while (removed < indentWidth && removed < row.size() && row.at(removed) == QLatin1Char(' '))
                ++removed;
        }
        row.remove(0, removed);
        if (i == 0)
            removedFromFirst = removed;
    }
    const QString block = rows.join(QLatin1Char('\n'));
    if (block.size() == lines.last - lines.first || !replace(lines.first, lines.last, block))
        return selection(start, end);
    if (start == end)
        return cursorAt(std::max(lines.first, start - removedFromFirst));
    const bool broken = lines.last < all.size();
    return selection(lines.first, lines.first + int(block.size()) + (broken ? 1 : 0));
}

QVariantMap ComposerEditing::moveLines(int start, int end, bool up)
{
    const QString all = text();
    order(all, start, end);
    const Lines lines = touchedLines(all, start, end);
    const QString block = all.mid(lines.first, lines.last - lines.first);
    if (up) {
        if (lines.first == 0)
            return selection(start, end);
        const int above = lineStart(all, lines.first - 1);
        const QString neighbour = all.mid(above, lines.first - 1 - above);
        if (!replace(above, lines.last, block + QLatin1Char('\n') + neighbour))
            return selection(start, end);
        const int shift = int(neighbour.size()) + 1;
        return selection(start - shift, end - shift);
    }
    if (lines.last >= all.size())
        return selection(start, end);
    const int belowEnd = lineEnd(all, lines.last + 1);
    const QString neighbour = all.mid(lines.last + 1, belowEnd - lines.last - 1);
    if (!replace(lines.first, belowEnd, neighbour + QLatin1Char('\n') + block))
        return selection(start, end);
    const int shift = int(neighbour.size()) + 1;
    return selection(start + shift, end + shift);
}

QVariantMap ComposerEditing::copyLines(int start, int end, bool up)
{
    const QString all = text();
    order(all, start, end);
    const Lines lines = touchedLines(all, start, end);
    const QString block = all.mid(lines.first, lines.last - lines.first);
    // Copying up leaves the selection on the new, upper copy, which now sits
    // where the original was; copying down carries it to the lower one.
    if (up) {
        if (!replace(lines.first, lines.first, block + QLatin1Char('\n')))
            return selection(start, end);
        return selection(start, end);
    }
    if (!replace(lines.last, lines.last, QLatin1Char('\n') + block))
        return selection(start, end);
    const int shift = int(block.size()) + 1;
    return selection(start + shift, end + shift);
}

QVariantMap ComposerEditing::deleteLines(int start, int end)
{
    const QString all = text();
    order(all, start, end);
    const Lines lines = touchedLines(all, start, end);
    // The line's own break goes with it; the last line takes the one before.
    if (lines.last < all.size()) {
        if (!replace(lines.first, lines.last + 1, QString()))
            return selection(start, end);
        return cursorAt(lines.first);
    }
    if (lines.first > 0) {
        if (!replace(lines.first - 1, lines.last, QString()))
            return selection(start, end);
        return cursorAt(lineStart(all, lines.first - 1));
    }
    if (!replace(lines.first, lines.last, QString()))
        return selection(start, end);
    return cursorAt(0);
}

QVariantMap ComposerEditing::selectLines(int start, int end)
{
    const QString all = text();
    order(all, start, end);
    const Lines lines = touchedLines(all, start, end);
    const int through = lines.last < all.size() ? lines.last + 1 : lines.last;
    if (start == lines.first && end == through && start != end && through < all.size()) {
        const int nextEnd = lineEnd(all, through);
        return selection(lines.first, nextEnd < all.size() ? nextEnd + 1 : nextEnd);
    }
    return selection(lines.first, through);
}

QVariantMap ComposerEditing::copyLine(int position)
{
    const QString all = text();
    position = std::clamp(position, 0, int(all.size()));
    const int begin = lineStart(all, position);
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(all.mid(begin, lineEnd(all, position) - begin) + QLatin1Char('\n'));
    return cursorAt(position);
}

QVariantMap ComposerEditing::cutLine(int position)
{
    copyLine(position);
    return deleteLines(position, position);
}

} // namespace OpenChat

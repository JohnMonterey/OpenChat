#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>

class QTextDocument;

namespace OpenChat {

// Proportional line spacing for a TextEdit, which, unlike Text, has no
// lineHeight of its own. Every block of the document gets the spacing, and
// keeps it when the text is replaced, so a selectable message reads with the
// same leading as the rest of the conversation.
class TextLineSpacing : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    // A multiple of the font's natural line height, as Text.lineHeight.
    Q_PROPERTY(qreal lineHeight READ lineHeight WRITE setLineHeight NOTIFY lineHeightChanged)
public:
    explicit TextLineSpacing(QObject *parent = nullptr);

    [[nodiscard]] QQuickTextDocument *document() const { return m_quickDocument; }
    void setDocument(QQuickTextDocument *document);
    // For callers holding the QTextDocument itself.
    void setTextDocument(QTextDocument *document);
    [[nodiscard]] qreal lineHeight() const { return m_lineHeight; }
    void setLineHeight(qreal lineHeight);

signals:
    void documentChanged();
    void lineHeightChanged();

private:
    void watch(QTextDocument *document);
    void apply();

    QPointer<QQuickTextDocument> m_quickDocument;
    QPointer<QTextDocument> m_document;
    QMetaObject::Connection m_contentsChanged;
    qreal m_lineHeight = 1.0;
    bool m_applying = false;
};

} // namespace OpenChat

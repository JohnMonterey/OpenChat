#include "app/TextLineSpacing.h"

#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

#include <cmath>

namespace OpenChat {

TextLineSpacing::TextLineSpacing(QObject *parent)
    : QObject(parent)
{
}

void TextLineSpacing::setDocument(QQuickTextDocument *document)
{
    if (m_quickDocument == document)
        return;
    m_quickDocument = document;
    watch(document != nullptr ? document->textDocument() : nullptr);
    emit documentChanged();
}

void TextLineSpacing::setTextDocument(QTextDocument *document)
{
    m_quickDocument = nullptr;
    watch(document);
    emit documentChanged();
}

void TextLineSpacing::setLineHeight(qreal lineHeight)
{
    if (qFuzzyCompare(m_lineHeight, lineHeight))
        return;
    m_lineHeight = lineHeight;
    apply();
    emit lineHeightChanged();
}

void TextLineSpacing::watch(QTextDocument *document)
{
    QObject::disconnect(m_contentsChanged);
    m_document = document;
    if (document != nullptr) {
        // New text arrives as new blocks with the default format.
        m_contentsChanged = connect(document, &QTextDocument::contentsChanged, this,
                                    &TextLineSpacing::apply);
    }
    apply();
}

void TextLineSpacing::apply()
{
    if (m_document == nullptr || m_applying)
        return;
    const qreal percent = std::round(m_lineHeight * 100.0);
    bool uniform = true;
    for (QTextBlock block = m_document->begin(); block.isValid() && uniform; block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        uniform = format.lineHeightType() == QTextBlockFormat::ProportionalHeight
            && qFuzzyCompare(format.lineHeight(), percent);
    }
    if (uniform)
        return;

    m_applying = true;
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    QTextBlockFormat format;
    format.setLineHeight(percent, QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(format);
    m_applying = false;
}

} // namespace OpenChat

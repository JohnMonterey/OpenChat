#pragma once

#include <QStringView>

namespace OpenChat::AppMetadata {

inline constexpr auto name = QStringView(u"OpenChat");
inline constexpr int defaultWidth = 860;
inline constexpr int defaultHeight = 680;
inline constexpr int minimumWidth = 720;
inline constexpr int minimumHeight = 560;

} // namespace OpenChat::AppMetadata

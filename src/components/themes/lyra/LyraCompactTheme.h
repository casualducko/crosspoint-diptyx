#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Lyra with tighter vertical metrics for the Diptyx panels (480x648 at ~138 ppi: fewer rows, bigger pixels than the
// X4's 480x800 at ~219 ppi). Same drawing code as Lyra.
namespace LyraCompactMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeTopPadding = 46;
  v.listRowHeight = 34;
  v.listWithSubtitleRowHeight = 54;
  v.menuRowHeight = 56;
  v.menuSpacing = 6;
  v.verticalSpacing = 12;
  v.tabBarHeight = 44;
  v.headerHeight = 76;
  return v;
}();
}  // namespace LyraCompactMetrics

class LyraCompactTheme : public LyraTheme {};

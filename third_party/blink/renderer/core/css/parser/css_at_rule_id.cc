// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/parser/css_at_rule_id.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>

#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/platform/instrumentation/use_counter.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace blink {

namespace {

// Metadata for at-rules. Sorted by name for binary search.
struct AtRuleEntry {
  const char* name;
  CSSAtRuleID id;
  WebFeature feature;
};

// clang-format off
constexpr AtRuleEntry kAtRuleEntries[] = {
    {"-webkit-keyframes", CSSAtRuleID::kCSSAtRuleWebkitKeyframes, WebFeature::kCSSAtRuleWebkitKeyframes},
    {"annotation", CSSAtRuleID::kCSSAtRuleAnnotation, WebFeature::kCSSAtRuleAnnotation},
    {"bottom-center", CSSAtRuleID::kCSSAtRuleBottomCenter, WebFeature::kCSSAtRulePageMargin},
    {"bottom-left", CSSAtRuleID::kCSSAtRuleBottomLeft, WebFeature::kCSSAtRulePageMargin},
    {"bottom-left-corner", CSSAtRuleID::kCSSAtRuleBottomLeftCorner, WebFeature::kCSSAtRulePageMargin},
    {"bottom-right", CSSAtRuleID::kCSSAtRuleBottomRight, WebFeature::kCSSAtRulePageMargin},
    {"bottom-right-corner", CSSAtRuleID::kCSSAtRuleBottomRightCorner, WebFeature::kCSSAtRulePageMargin},
    {"character-variant", CSSAtRuleID::kCSSAtRuleCharacterVariant, WebFeature::kCSSAtRuleCharacterVariant},
    {"charset", CSSAtRuleID::kCSSAtRuleCharset, WebFeature::kCSSAtRuleCharset},
    {"container", CSSAtRuleID::kCSSAtRuleContainer, WebFeature::kCSSAtRuleContainer},
    {"counter-style", CSSAtRuleID::kCSSAtRuleCounterStyle, WebFeature::kCSSAtRuleCounterStyle},
    {"font-face", CSSAtRuleID::kCSSAtRuleFontFace, WebFeature::kCSSAtRuleFontFace},
    {"font-feature-values", CSSAtRuleID::kCSSAtRuleFontFeatureValues, WebFeature::kCSSAtRuleFontFeatureValues},
    {"font-palette-values", CSSAtRuleID::kCSSAtRuleFontPaletteValues, WebFeature::kCSSAtRuleFontPaletteValues},
    {"import", CSSAtRuleID::kCSSAtRuleImport, WebFeature::kCSSAtRuleImport},
    {"keyframes", CSSAtRuleID::kCSSAtRuleKeyframes, WebFeature::kCSSAtRuleKeyframes},
    {"layer", CSSAtRuleID::kCSSAtRuleLayer, WebFeature::kCSSCascadeLayers},
    {"left-bottom", CSSAtRuleID::kCSSAtRuleLeftBottom, WebFeature::kCSSAtRulePageMargin},
    {"left-middle", CSSAtRuleID::kCSSAtRuleLeftMiddle, WebFeature::kCSSAtRulePageMargin},
    {"left-top", CSSAtRuleID::kCSSAtRuleLeftTop, WebFeature::kCSSAtRulePageMargin},
    {"media", CSSAtRuleID::kCSSAtRuleMedia, WebFeature::kCSSAtRuleMedia},
    {"namespace", CSSAtRuleID::kCSSAtRuleNamespace, WebFeature::kCSSAtRuleNamespace},
    {"ornaments", CSSAtRuleID::kCSSAtRuleOrnaments, WebFeature::kCSSAtRuleOrnaments},
    {"page", CSSAtRuleID::kCSSAtRulePage, WebFeature::kCSSAtRulePage},
    {"position-try", CSSAtRuleID::kCSSAtRulePositionTry, WebFeature::kCSSAnchorPositioning},
    {"property", CSSAtRuleID::kCSSAtRuleProperty, WebFeature::kCSSAtRuleProperty},
    {"right-bottom", CSSAtRuleID::kCSSAtRuleRightBottom, WebFeature::kCSSAtRulePageMargin},
    {"right-middle", CSSAtRuleID::kCSSAtRuleRightMiddle, WebFeature::kCSSAtRulePageMargin},
    {"right-top", CSSAtRuleID::kCSSAtRuleRightTop, WebFeature::kCSSAtRulePageMargin},
    {"scope", CSSAtRuleID::kCSSAtRuleScope, WebFeature::kCSSAtRuleScope},
    {"starting-style", CSSAtRuleID::kCSSAtRuleStartingStyle, WebFeature::kCSSAtRuleStartingStyle},
    {"styleset", CSSAtRuleID::kCSSAtRuleStyleset, WebFeature::kCSSAtRuleStylistic},
    {"stylistic", CSSAtRuleID::kCSSAtRuleStylistic, WebFeature::kCSSAtRuleStylistic},
    {"supports", CSSAtRuleID::kCSSAtRuleSupports, WebFeature::kCSSAtRuleSupports},
    {"swash", CSSAtRuleID::kCSSAtRuleSwash, WebFeature::kCSSAtRuleSwash},
    {"top-center", CSSAtRuleID::kCSSAtRuleTopCenter, WebFeature::kCSSAtRulePageMargin},
    {"top-left", CSSAtRuleID::kCSSAtRuleTopLeft, WebFeature::kCSSAtRulePageMargin},
    {"top-left-corner", CSSAtRuleID::kCSSAtRuleTopLeftCorner, WebFeature::kCSSAtRulePageMargin},
    {"top-right", CSSAtRuleID::kCSSAtRuleTopRight, WebFeature::kCSSAtRulePageMargin},
    {"top-right-corner", CSSAtRuleID::kCSSAtRuleTopRightCorner, WebFeature::kCSSAtRulePageMargin},
    {"view-transition", CSSAtRuleID::kCSSAtRuleViewTransition, WebFeature::kCSSAtRuleViewTransition},
};
// clang-format on

// At-rules gated behind runtime flags.
// Sorted by name for consistency with kAtRuleEntries.
struct FlaggedAtRuleEntry {
  const char* name;
  CSSAtRuleID id;
  WebFeature feature;
  bool (*is_enabled)();
};

// clang-format off
constexpr FlaggedAtRuleEntry kFlaggedAtRuleEntries[] = {
    {"apply", CSSAtRuleID::kCSSAtRuleApplyMixin, WebFeature::kCSSMixins,
     &RuntimeEnabledFeatures::CSSMixinsEnabled},
    {"contents", CSSAtRuleID::kCSSAtRuleContents, WebFeature::kCSSMixins,
     &RuntimeEnabledFeatures::CSSMixinsEnabled},
    {"custom-media", CSSAtRuleID::kCSSAtRuleCustomMedia, WebFeature::kCSSCustomMedia,
     &RuntimeEnabledFeatures::CSSCustomMediaEnabled},
    {"function", CSSAtRuleID::kCSSAtRuleFunction, WebFeature::kCSSFunctions,
     &RuntimeEnabledFeatures::CSSFunctionsEnabled},
    {"location", CSSAtRuleID::kCSSAtRuleLocation, WebFeature::kCSSAtRuleRoute,
     &RuntimeEnabledFeatures::RouteMatchingEnabled},
    {"mixin", CSSAtRuleID::kCSSAtRuleMixin, WebFeature::kCSSMixins,
     &RuntimeEnabledFeatures::CSSMixinsEnabled},
    {"navigation", CSSAtRuleID::kCSSAtRuleNavigation, WebFeature::kCSSAtRuleRoute,
     &RuntimeEnabledFeatures::RouteMatchingEnabled},
    {"private", CSSAtRuleID::kCSSAtRulePrivate, WebFeature::kCSSAtPrivate,
     &RuntimeEnabledFeatures::CSSPrivateEnabled},
    {"result", CSSAtRuleID::kCSSAtRuleResult, WebFeature::kCSSMixins,
     &RuntimeEnabledFeatures::CSSMixinsEnabled},
};
// clang-format on

// Compile-time validation that tables are sorted for binary search.
constexpr auto AtRuleNameProjection = [](const auto& entry) {
  return std::string_view(entry.name);
};

static_assert(std::ranges::is_sorted(kAtRuleEntries, {}, AtRuleNameProjection),
              "kAtRuleEntries must be sorted by name for binary search");
static_assert(std::ranges::is_sorted(kFlaggedAtRuleEntries,
                                     {},
                                     AtRuleNameProjection),
              "kFlaggedAtRuleEntries must be sorted by name");

}  // namespace

namespace {

// 中文 at-rule 名称支持（移植自 qt-everywhere Chromium134）。
CSSAtRuleID ChineseCssAtRuleID(StringView name) {
  if (name.length() == 4 && name[0] == 0x89c6 && name[1] == 0x56fe &&
      name[2] == 0x8fc7 && name[3] == 0x6e21) {  // 视图过渡
    return CSSAtRuleID::kCSSAtRuleViewTransition;
  }
  if (name.length() == 3 && name[0] == 0x5b57 && name[1] == 0x7b26 &&
      name[2] == 0x96c6) {  // 字符集
    return CSSAtRuleID::kCSSAtRuleCharset;
  }
  if (name.length() == 3 && name[0] == 0x5b57 && name[1] == 0x4f53 &&
      name[2] == 0x9762) {  // 字体面
    return CSSAtRuleID::kCSSAtRuleFontFace;
  }
  if (name.length() == 5 && name[0] == 0x5b57 && name[1] == 0x4f53 &&
      name[2] == 0x8c03 && name[3] == 0x8272 && name[4] == 0x677f) {  // 字体调色板
    return CSSAtRuleID::kCSSAtRuleFontPaletteValues;
  }
  if (name.length() == 5 && name[0] == 0x5b57 && name[1] == 0x4f53 &&
      name[2] == 0x7279 && name[3] == 0x6027 && name[4] == 0x503c) {  // 字体特性值
    return CSSAtRuleID::kCSSAtRuleFontFeatureValues;
  }
  if (name.length() == 3 && name[0] == 0x98ce && name[1] == 0x683c &&
      name[2] == 0x5316) {  // 风格化
    return CSSAtRuleID::kCSSAtRuleStylistic;
  }
  if (name.length() == 3 && name[0] == 0x6837 && name[1] == 0x5f0f &&
      name[2] == 0x96c6) {  // 样式集
    return CSSAtRuleID::kCSSAtRuleStyleset;
  }
  if (name.length() == 4 && name[0] == 0x5b57 && name[1] == 0x7b26 &&
      name[2] == 0x53d8 && name[3] == 0x4f53) {  // 字符变体
    return CSSAtRuleID::kCSSAtRuleCharacterVariant;
  }
  if (name.length() == 2 && name[0] == 0x82b1 && name[1] == 0x9970) {  // 花饰
    return CSSAtRuleID::kCSSAtRuleSwash;
  }
  if (name.length() == 2 && name[0] == 0x88c5 && name[1] == 0x9970) {  // 装饰
    return CSSAtRuleID::kCSSAtRuleOrnaments;
  }
  if (name.length() == 2 && name[0] == 0x6ce8 && name[1] == 0x91ca) {  // 注释
    return CSSAtRuleID::kCSSAtRuleAnnotation;
  }
  if (name.length() == 2 && name[0] == 0x5bfc && name[1] == 0x5165) {  // 导入
    return CSSAtRuleID::kCSSAtRuleImport;
  }
  if (name.length() == 3 && name[0] == 0x5173 && name[1] == 0x952e &&
      name[2] == 0x5e27) {  // 关键帧
    return CSSAtRuleID::kCSSAtRuleKeyframes;
  }
  if (name.length() == 1 && name[0] == 0x5c42) {  // 层
    return CSSAtRuleID::kCSSAtRuleLayer;
  }
  if (name.length() == 2 && name[0] == 0x5a92 && name[1] == 0x4f53) {  // 媒体
    return CSSAtRuleID::kCSSAtRuleMedia;
  }
  if (name.length() == 4 && name[0] == 0x547d && name[1] == 0x540d &&
      name[2] == 0x7a7a && name[3] == 0x95f4) {  // 命名空间
    return CSSAtRuleID::kCSSAtRuleNamespace;
  }
  if (name.length() == 2 && name[0] == 0x9875 && name[1] == 0x9762) {  // 页面
    return CSSAtRuleID::kCSSAtRulePage;
  }
  if (name.length() == 4 && name[0] == 0x5b9a && name[1] == 0x4f4d &&
      name[2] == 0x5c1d && name[3] == 0x8bd5) {  // 定位尝试
    return CSSAtRuleID::kCSSAtRulePositionTry;
  }
  if (name.length() == 2 && name[0] == 0x5c5e && name[1] == 0x6027) {  // 属性
    return CSSAtRuleID::kCSSAtRuleProperty;
  }
  if (name.length() == 2 && name[0] == 0x5bb9 && name[1] == 0x5668) {  // 容器
    return CSSAtRuleID::kCSSAtRuleContainer;
  }
  if (name.length() == 5 && name[0] == 0x8ba1 && name[1] == 0x6570 &&
      name[2] == 0x5668 && name[3] == 0x6837 && name[4] == 0x5f0f) {  // 计数器样式
    return CSSAtRuleID::kCSSAtRuleCounterStyle;
  }
  if (name.length() == 3 && name[0] == 0x4f5c && name[1] == 0x7528 &&
      name[2] == 0x57df) {  // 作用域
    return CSSAtRuleID::kCSSAtRuleScope;
  }
  if (name.length() == 2 && name[0] == 0x652f && name[1] == 0x6301) {  // 支持
    return CSSAtRuleID::kCSSAtRuleSupports;
  }
  if (name.length() == 4 && name[0] == 0x8d77 && name[1] == 0x59cb &&
      name[2] == 0x6837 && name[3] == 0x5f0f) {  // 起始样式
    return CSSAtRuleID::kCSSAtRuleStartingStyle;
  }
  if (name.length() == 9 && name[0] == 0x57 && name[1] == 0x65 &&
      name[2] == 0x62 && name[3] == 0x6b && name[4] == 0x69 &&
      name[5] == 0x74 && name[6] == 0x5173 && name[7] == 0x952e &&
      name[8] == 0x5e27) {  // Webkit关键帧
    return CSSAtRuleID::kCSSAtRuleWebkitKeyframes;
  }
  return CSSAtRuleID::kCSSAtRuleInvalid;
}

}  // namespace

CSSAtRuleID CssAtRuleID(StringView name) {
  if (CSSAtRuleID id = ChineseCssAtRuleID(name);
      id != CSSAtRuleID::kCSSAtRuleInvalid) {
    return id;
  }
  // Binary search the main table.
  const auto* it = std::lower_bound(
      std::begin(kAtRuleEntries), std::end(kAtRuleEntries), name,
      [](const AtRuleEntry& entry, const StringView& target) {
        return CodeUnitCompareIgnoringAsciiCase(StringView(entry.name),
                                                target) < 0;
      });
  if (it != std::end(kAtRuleEntries) &&
      EqualIgnoringAsciiCase(name, it->name)) {
    return it->id;
  }
  // Linear search the smaller flagged entries table.
  for (const auto& entry : kFlaggedAtRuleEntries) {
    if (entry.is_enabled() && EqualIgnoringAsciiCase(name, entry.name)) {
      return entry.id;
    }
  }
  return CSSAtRuleID::kCSSAtRuleInvalid;
}

StringView CssAtRuleIDToString(CSSAtRuleID id) {
  for (const auto& entry : kAtRuleEntries) {
    if (entry.id == id) {
      return entry.name;
    }
  }
  for (const auto& entry : kFlaggedAtRuleEntries) {
    if (entry.id == id) {
      return entry.name;
    }
  }
  NOTREACHED();
}

namespace {

std::optional<WebFeature> AtRuleFeature(CSSAtRuleID rule_id) {
  for (const auto& entry : kAtRuleEntries) {
    if (entry.id == rule_id) {
      return entry.feature;
    }
  }
  for (const auto& entry : kFlaggedAtRuleEntries) {
    if (entry.id == rule_id) {
      return entry.feature;
    }
  }
  NOTREACHED();
}

}  // namespace

void CountAtRule(const CSSParserContext* context, CSSAtRuleID rule_id) {
  if (std::optional<WebFeature> feature = AtRuleFeature(rule_id)) {
    context->Count(*feature);
  }
}

}  // namespace blink

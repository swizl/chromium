// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/parser/css_property_parser.h"

#include <cstring>

#include "base/compiler_specific.h"
#include "third_party/blink/renderer/core/css/css_pending_substitution_value.h"
#include "third_party/blink/renderer/core/css/css_property_name.h"
#include "third_party/blink/renderer/core/css/css_unicode_range_value.h"
#include "third_party/blink/renderer/core/css/css_unparsed_declaration_value.h"
#include "third_party/blink/renderer/core/css/hash_tools.h"
#include "third_party/blink/renderer/core/css/parser/at_rule_descriptor_parser.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_impl.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_local_context.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_mode.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_save_point.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_token_stream.h"
#include "third_party/blink/renderer/core/css/parser/css_variable_parser.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_property.h"
#include "third_party/blink/renderer/core/css/properties/shorthand.h"
#include "third_party/blink/renderer/core/css/property_bitsets.h"
#include "third_party/blink/renderer/core/html/parser/html_parser_idioms.h"
#include "third_party/blink/renderer/core/style_property_shorthand.h"
#include "third_party/blink/renderer/platform/wtf/text/character_visitor.h"

namespace blink {

using css_parsing_utils::ConsumeIdent;
using css_parsing_utils::IsImplicitProperty;
using css_parsing_utils::ParseLonghand;

class CSSIdentifierValue;

namespace {

bool IsPropertyAllowedInRule(const CSSProperty& property,
                             StyleRule::RuleType rule_type) {
  // This function should be called only when parsing a property. Shouldn't
  // reach here with a descriptor.
  DCHECK(property.IsProperty());
  switch (rule_type) {
    case StyleRule::kStyle:
    case StyleRule::kScope:
      return true;
    case StyleRule::kPage:
    case StyleRule::kPageMargin:
      // TODO(sesse): Limit the allowed properties here.
      // https://www.w3.org/TR/css-page-3/#page-property-list
      // https://www.w3.org/TR/css-page-3/#margin-property-list
      return true;
    case StyleRule::kKeyframe:
      return property.IsValidForKeyframe();
    case StyleRule::kPositionTry:
      return property.IsValidForPositionTry();
    default:
      NOTREACHED();
  }
}

}  // namespace

CSSPropertyParser::CSSPropertyParser(
    CSSParserTokenStream& stream,
    const CSSParserContext* context,
    HeapVector<CSSPropertyValue, 64>* parsed_properties)
    : stream_(stream),
      context_(context),
      parsed_properties_(parsed_properties) {
  // Strip initial whitespace/comments from stream_.
  stream_.ConsumeWhitespace();
}

bool CSSPropertyParser::ParseValue(
    CSSPropertyID unresolved_property,
    bool allow_important_annotation,
    CSSParserTokenStream& stream,
    const CSSParserContext* context,
    HeapVector<CSSPropertyValue, 64>& parsed_properties,
    StyleRule::RuleType rule_type) {
  CSSPropertyParser parser(stream, context, &parsed_properties);
  CSSPropertyID resolved_property = ResolveCSSPropertyID(unresolved_property);
  bool parse_success;
  if (rule_type == StyleRule::kFontFace) {
    parse_success = parser.ParseFontFaceDescriptor(resolved_property);
  } else {
    parse_success = parser.ParseValueStart(
        unresolved_property, allow_important_annotation, rule_type);
  }

  // This doesn't count UA style sheets
  if (parse_success) {
    context->Count(unresolved_property);
  }

  return parse_success;
}

// NOTE: “stream” cannot include !important; this is for setting properties
// from CSSOM or similar. |unresolved_property| may be an alias (e.g.,
// kWebkitBackgroundClip); it is resolved internally but the unresolved ID is
// preserved in the local context so that UseAliasParsing() works correctly.
const CSSValue* CSSPropertyParser::ParseSingleValue(
    CSSPropertyID unresolved_property,
    CSSParserTokenStream& stream,
    const CSSParserContext* context) {
  DCHECK(context);
  stream.ConsumeWhitespace();

  const CSSValue* value =
      css_parsing_utils::ConsumeCSSWideKeyword(stream, *context);
  if (!value) {
    auto local_context = CSSParserLocalContext(
        CSSPropertyName(unresolved_property), CSSPropertyID::kInvalid);
    value = ParseLonghand(unresolved_property, *context, local_context, stream);
  }
  if (!value || !stream.AtEnd()) {
    return nullptr;
  }
  return value;
}

StringView StripInitialWhitespace(StringView value) {
  wtf_size_t initial_whitespace_len = 0;
  // SAFETY: index checked against length prior to use via &&-expression.
  while (initial_whitespace_len < value.length() &&
         IsHTMLSpace(UNSAFE_BUFFERS(value[initial_whitespace_len]))) {
    ++initial_whitespace_len;
  }
  return StringView(value, initial_whitespace_len);
}

bool CSSPropertyParser::ParseValueStart(CSSPropertyID unresolved_property,
                                        bool allow_important_annotation,
                                        StyleRule::RuleType rule_type) {
  if (ParseCSSWideKeyword(unresolved_property, rule_type)) {
    return true;
  }

  CSSParserTokenStream::State savepoint = stream_.Save();

  CSSPropertyID property_id = ResolveCSSPropertyID(unresolved_property);
  const CSSProperty& property = CSSProperty::Get(property_id);
  // If a CSSPropertyID is only a known descriptor (@fontface, @property), not a
  // style property, it will not be a valid declaration.
  if (!property.IsProperty()) {
    return false;
  }
  if (!IsPropertyAllowedInRule(property, rule_type)) {
    return false;
  }
  int parsed_properties_size = parsed_properties_->size();

  bool is_shorthand = property.IsShorthand();
  DCHECK(context_);
  CSSParserLocalContext local_context(CSSPropertyName(unresolved_property),
                                      CSSPropertyID::kInvalid);

  // NOTE: The first branch of the if here uses the tokenized form,
  // and the second uses the streaming parser. This is only allowed
  // since they start from the same place and we reset both below,
  // so they cannot go out of sync.
  if (is_shorthand) {
    local_context.SetCurrentShorthand(property_id);
    // Variable references will fail to parse here and will fall out to the
    // variable ref parser below.
    //
    // NOTE: We call ParseShorthand() with important=false, since we don't know
    // yet whether we have !important or not. We'll change the flag for all
    // added properties below (ParseShorthand() makes its own calls to
    // AddProperty(), since there may be more than one of them).
    if (To<Shorthand>(property).ParseShorthand(
            /*important=*/false, stream_, *context_, local_context,
            *parsed_properties_)) {
      bool important = css_parsing_utils::MaybeConsumeImportant(
          stream_, allow_important_annotation);
      if (stream_.AtEnd()) {
        if (important) {
          for (wtf_size_t property_idx = parsed_properties_size;
               property_idx < parsed_properties_->size(); ++property_idx) {
            (*parsed_properties_)[property_idx].SetImportant();
          }
        }
        return true;
      }
    }

    // Remove any properties that may have been added by ParseShorthand()
    // during a failing parse earlier.
    parsed_properties_->Shrink(parsed_properties_size);
  } else {
    if (const CSSValue* parsed_value = ParseLonghand(
            unresolved_property, *context_, local_context, stream_)) {
      bool important = css_parsing_utils::MaybeConsumeImportant(
          stream_, allow_important_annotation);
      if (stream_.AtEnd()) {
        AddProperty(property_id, CSSPropertyID::kInvalid, *parsed_value,
                    important, IsImplicitProperty::kNotImplicit,
                    *parsed_properties_);
        return true;
      }
    }
  }

  // From here we can end up parsing arbitrary CSS properties via
  // if(supports(<property>:<value>)). CSS parsing can in some cases be unsafe
  // cross threads where we rely on static Persistent objects which are not
  // thread local.
  //
  // Some CSSParser APIS are invoked off main thread for canvas APIs, but should
  // never end up here.
  CHECK(IsMainThread());

  // We did not parse properly without variable substitution,
  // so rewind the stream, and see if parsing it as something
  // containing variables will help.
  //
  // Note that if so, this needs the original text, so we need to take
  // note of the original offsets so that we can see what we tokenized.
  stream_.EnsureLookAhead();
  stream_.Restore(savepoint);

  bool important = false;
  CSSVariableData* variable_data =
      CSSVariableParser::ConsumeUnparsedDeclaration(
          stream_,
          /*allow_important_annotation=*/true,
          /*is_animation_tainted=*/false,
          /*must_contain_variable_reference=*/true,
          /*restricted_value=*/true, /*comma_ends_declaration=*/false,
          important, *context_);
  if (!variable_data) {
    return false;
  }

  auto* variable = MakeGarbageCollected<CSSUnparsedDeclarationValue>(
      variable_data, context_);
  if (is_shorthand) {
    const cssvalue::CSSPendingSubstitutionValue& pending_value =
        *MakeGarbageCollected<cssvalue::CSSPendingSubstitutionValue>(
            property_id, variable);
    css_parsing_utils::AddExpandedPropertyForValue(
        property_id, pending_value, important, *parsed_properties_);
  } else {
    AddProperty(property_id, CSSPropertyID::kInvalid, *variable, important,
                IsImplicitProperty::kNotImplicit, *parsed_properties_);
  }
  return true;
}

static inline bool IsExposedInMode(const ExecutionContext* execution_context,
                                   const CSSUnresolvedProperty& property,
                                   CSSParserMode mode) {
  return mode == kUASheetMode ? property.IsUAExposed(execution_context)
                              : property.IsWebExposed(execution_context);
}

// Encode the given characters as UTF-8 into |dst|, lowercasing ASCII
// characters on the way (mirroring QuasiLowercaseIntoBuffer()). Unlike
// QuasiLowercaseIntoBuffer(), non-ASCII input such as Chinese is kept, which
// lets the Chinese lookup tables above match it. The destination is assumed
// to be large enough (kMax*Length * 3 + 1).
//
// Returns the number of bytes written (excluding the null terminator), or 0
// if the string contains a null character.
static inline unsigned EncodeLowercaseUTF8(base::span<const UChar> src,
                                           char* dst) {
  unsigned pos = 0;
  for (unsigned i = 0; i < src.size(); ++i) {
    UChar c = UNSAFE_BUFFERS(src[i]);
    if (c == 0) {
      return 0;  // null character
    }
    if (c < 0x80) {
      dst[pos++] = static_cast<char>(c | ((c & 0x40) >> 1));
    } else if (c < 0x800) {
      dst[pos++] = static_cast<char>(0xC0 | (c >> 6));
      dst[pos++] = static_cast<char>(0x80 | (c & 0x3F));
    } else {
      dst[pos++] = static_cast<char>(0xE0 | (c >> 12));
      dst[pos++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      dst[pos++] = static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  dst[pos] = '\0';
  return pos;
}

// Fast-path version for LChar strings: each byte is already the UTF-8
// encoding of the character. ASCII letters are lowercased by setting the
// 0x20 bit; bytes >= 0x80 (e.g. Chinese UTF-8) are passed through unchanged.
static inline unsigned EncodeLowercaseUTF8(base::span<const LChar> src,
                                           char* dst) {
  for (unsigned i = 0; i < src.size(); ++i) {
    unsigned char c = UNSAFE_BUFFERS(src[i]);
    if (c < 0x80) {
      dst[i] = static_cast<char>(c | ((c & 0x40) >> 1));
    } else {
      dst[i] = static_cast<char>(c);
    }
  }
  dst[src.size()] = '\0';
  return static_cast<unsigned>(src.size());
}


// The "exposed" property is different from the incoming property in the
// following cases:
//
//  - The property has an alternative property [1] which is enabled. Note that
//    alternative properties also can have alternative properties.
//  - The property is not enabled. This is represented by
//    CSSPropertyID::kInvalid.
//
// [1] See documentation near "alternative_of" in css_properties.json5.
static CSSPropertyID ExposedProperty(CSSPropertyID property_id,
                                     const ExecutionContext* execution_context,
                                     CSSParserMode mode) {
  const CSSUnresolvedProperty& property =
      CSSUnresolvedProperty::Get(property_id);
  CSSPropertyID alternative_id = property.GetAlternative();
  if (alternative_id != CSSPropertyID::kInvalid) {
    if (CSSPropertyID exposed_id =
            ExposedProperty(alternative_id, execution_context, mode);
        exposed_id != CSSPropertyID::kInvalid) {
      return exposed_id;
    }
  }
  return IsExposedInMode(execution_context, property, mode)
             ? property_id
             : CSSPropertyID::kInvalid;
}

// Look up a Chinese CSS property name (encoded as UTF-8 in |buffer| by
// EncodeLowercaseUTF8()) and map it to the corresponding CSSPropertyID.
// Returns CSSPropertyID::kInvalid if |buffer| is not a Chinese property name.
static CSSPropertyID ChineseCSSPropertyID(
    const ExecutionContext* execution_context,
    const char* buffer,
    unsigned utf8_len,
    CSSParserMode mode) {
  static constexpr struct ZhCssPropertyEntry {
    const char8_t* zh;
    size_t zh_len;
    CSSPropertyID id;
  } kZhCssPropertyTable[] = {
    {u8"位置", 6, CSSPropertyID::kPosition},
    {u8"偏移", 6, CSSPropertyID::kOffset},
    {u8"光标", 6, CSSPropertyID::kCursor},
    {u8"全部", 6, CSSPropertyID::kAll},
    {u8"内容", 6, CSSPropertyID::kContent},
    {u8"内嵌", 6, CSSPropertyID::kInset},
    {u8"内衬", 6, CSSPropertyID::kPad},
    {u8"列宽", 6, CSSPropertyID::kColumnWidth},
    {u8"列数", 6, CSSPropertyID::kColumnCount},
    {u8"前缀", 6, CSSPropertyID::kPrefix},
    {u8"加粗", 6, CSSPropertyID::kFontWeight},
    {u8"动画", 6, CSSPropertyID::kAnimation},
    {u8"包含", 6, CSSPropertyID::kContain},
    {u8"半径", 6, CSSPropertyID::kR},
    {u8"变换", 6, CSSPropertyID::kTransform},
    {u8"可见", 6, CSSPropertyID::kVisibility},
    {u8"右侧", 6, CSSPropertyID::kRight},
    {u8"后缀", 6, CSSPropertyID::kSuffix},
    {u8"回退", 6, CSSPropertyID::kFallback},
    {u8"圆角", 6, CSSPropertyID::kBorderRadius},
    {u8"填充", 6, CSSPropertyID::kFill},
    {u8"外观", 6, CSSPropertyID::kAppearance},
    {u8"多列", 6, CSSPropertyID::kColumns},
    {u8"字体", 6, CSSPropertyID::kFont},
    {u8"孤行", 6, CSSPropertyID::kWidows},
    {u8"容器", 6, CSSPropertyID::kContainer},
    {u8"宽度", 6, CSSPropertyID::kWidth},
    {u8"寡行", 6, CSSPropertyID::kOrphans},
    {u8"对齐", 6, CSSPropertyID::kAlignItems},
    {u8"导航", 6, CSSPropertyID::kNavigation},
    {u8"尺寸", 6, CSSPropertyID::kSize},
    {u8"层级", 6, CSSPropertyID::kZIndex},
    {u8"左侧", 6, CSSPropertyID::kLeft},
    {u8"平移", 6, CSSPropertyID::kTranslate},
    {u8"底部", 6, CSSPropertyID::kBottom},
    {u8"引号", 6, CSSPropertyID::kQuotes},
    {u8"弹性", 6, CSSPropertyID::kFlex},
    {u8"换行", 6, CSSPropertyID::kLineBreak},
    {u8"排序", 6, CSSPropertyID::kOrder},
    {u8"描边", 6, CSSPropertyID::kStroke},
    {u8"斜体", 6, CSSPropertyID::kFontStyle},
    {u8"断词", 6, CSSPropertyID::kWordBreak},
    {u8"方向", 6, CSSPropertyID::kDirection},
    {u8"旋转", 6, CSSPropertyID::kRotate},
    {u8"显示", 6, CSSPropertyID::kDisplay},
    {u8"朗读", 6, CSSPropertyID::kSpeak},
    {u8"来源", 6, CSSPropertyID::kSrc},
    {u8"标记", 6, CSSPropertyID::kMarker},
    {u8"浮动", 6, CSSPropertyID::kFloat},
    {u8"清除", 6, CSSPropertyID::kClear},
    {u8"溢出", 6, CSSPropertyID::kOverflow},
    {u8"滤镜", 6, CSSPropertyID::kFilter},
    {u8"符号", 6, CSSPropertyID::kSymbols},
    {u8"类型", 6, CSSPropertyID::kTypes},
    {u8"系统", 6, CSSPropertyID::kSystem},
    {u8"结果", 6, CSSPropertyID::kResult},
    {u8"继承", 6, CSSPropertyID::kInherits},
    {u8"缩放", 6, CSSPropertyID::kScale},
    {u8"网格", 6, CSSPropertyID::kGrid},
    {u8"背景", 6, CSSPropertyID::kBackground},
    {u8"范围", 6, CSSPropertyID::kRange},
    {u8"行高", 6, CSSPropertyID::kLineHeight},
    {u8"裁剪", 6, CSSPropertyID::kClip},
    {u8"语法", 6, CSSPropertyID::kSyntax},
    {u8"负数", 6, CSSPropertyID::kNegative},
    {u8"轮廓", 6, CSSPropertyID::kOutline},
    {u8"边框", 6, CSSPropertyID::kBorder},
    {u8"边距", 6, CSSPropertyID::kMargin},
    {u8"过渡", 6, CSSPropertyID::kTransition},
    {u8"透明", 6, CSSPropertyID::kOpacity},
    {u8"透视", 6, CSSPropertyID::kPerspective},
    {u8"遮罩", 6, CSSPropertyID::kMask},
    {u8"间距", 6, CSSPropertyID::kGap},
    {u8"隔离", 6, CSSPropertyID::kIsolation},
    {u8"页面", 6, CSSPropertyID::kPage},
    {u8"顶部", 6, CSSPropertyID::kTop},
    {u8"颜色", 6, CSSPropertyID::kColor},
    {u8"高度", 6, CSSPropertyID::kHeight},
    {u8"X坐标", 7, CSSPropertyID::kX},
    {u8"Y坐标", 7, CSSPropertyID::kY},
    {u8"圆心X", 7, CSSPropertyID::kCx},
    {u8"圆心Y", 7, CSSPropertyID::kCy},
    {u8"上边框", 9, CSSPropertyID::kBorderTop},
    {u8"上边距", 9, CSSPropertyID::kMarginTop},
    {u8"下边框", 9, CSSPropertyID::kBorderBottom},
    {u8"下边距", 9, CSSPropertyID::kMarginBottom},
    {u8"交互性", 9, CSSPropertyID::kInteractivity},
    {u8"内边距", 9, CSSPropertyID::kPadding},
    {u8"列填充", 9, CSSPropertyID::kColumnFill},
    {u8"列开始", 9, CSSPropertyID::kGridColumnStart},
    {u8"列结束", 9, CSSPropertyID::kGridColumnEnd},
    {u8"列规则", 9, CSSPropertyID::kColumnRule},
    {u8"列跨行", 9, CSSPropertyID::kColumnSpan},
    {u8"列间距", 9, CSSPropertyID::kColumnGap},
    {u8"初始值", 9, CSSPropertyID::kInitialValue},
    {u8"变换盒", 9, CSSPropertyID::kTransformBox},
    {u8"右边框", 9, CSSPropertyID::kBorderRight},
    {u8"右边距", 9, CSSPropertyID::kMarginRight},
    {u8"字体族", 9, CSSPropertyID::kFontFamily},
    {u8"字间距", 9, CSSPropertyID::kLetterSpacing},
    {u8"宽高比", 9, CSSPropertyID::kAspectRatio},
    {u8"将变更", 9, CSSPropertyID::kWillChange},
    {u8"左边框", 9, CSSPropertyID::kBorderLeft},
    {u8"左边距", 9, CSSPropertyID::kMarginLeft},
    {u8"弹性流", 9, CSSPropertyID::kFlexFlow},
    {u8"强调色", 9, CSSPropertyID::kAccentColor},
    {u8"文本框", 9, CSSPropertyID::kTextBox},
    {u8"断字符", 9, CSSPropertyID::kHyphenateCharacter},
    {u8"模板列", 9, CSSPropertyID::kGridTemplateColumns},
    {u8"模板行", 9, CSSPropertyID::kGridTemplateRows},
    {u8"盒模型", 9, CSSPropertyID::kBoxSizing},
    {u8"网格列", 9, CSSPropertyID::kGridColumn},
    {u8"网格行", 9, CSSPropertyID::kGridRow},
    {u8"背景色", 9, CSSPropertyID::kBackgroundColor},
    {u8"自动列", 9, CSSPropertyID::kGridAutoColumns},
    {u8"自动行", 9, CSSPropertyID::kGridAutoRows},
    {u8"行开始", 9, CSSPropertyID::kGridRowStart},
    {u8"行结束", 9, CSSPropertyID::kGridRowEnd},
    {u8"行间距", 9, CSSPropertyID::kRowGap},
    {u8"覆盖层", 9, CSSPropertyID::kOverlay},
    {u8"词间距", 9, CSSPropertyID::kWordSpacing},
    {u8"连字符", 9, CSSPropertyID::kHyphens},
    {u8"阅读流", 9, CSSPropertyID::kReadingFlow},
    {u8"首字母", 9, CSSPropertyID::kInitialLetter},
    {u8"EPUB断词", 10, CSSPropertyID::kAliasEpubWordBreak},
    {u8"Webkit列宽", 12, CSSPropertyID::kAliasWebkitColumnWidth},
    {u8"Webkit列数", 12, CSSPropertyID::kAliasWebkitColumnCount},
    {u8"Webkit动画", 12, CSSPropertyID::kAliasWebkitAnimation},
    {u8"Webkit变换", 12, CSSPropertyID::kAliasWebkitTransform},
    {u8"Webkit圆角", 12, CSSPropertyID::kAliasWebkitBorderRadius},
    {u8"Webkit外观", 12, CSSPropertyID::kAliasWebkitAppearance},
    {u8"Webkit多列", 12, CSSPropertyID::kAliasWebkitColumns},
    {u8"Webkit对齐", 12, CSSPropertyID::kAliasWebkitAlignItems},
    {u8"Webkit弹性", 12, CSSPropertyID::kAliasWebkitFlex},
    {u8"Webkit换行", 12, CSSPropertyID::kWebkitLineBreak},
    {u8"Webkit排序", 12, CSSPropertyID::kAliasWebkitOrder},
    {u8"Webkit滤镜", 12, CSSPropertyID::kAliasWebkitFilter},
    {u8"Webkit过渡", 12, CSSPropertyID::kAliasWebkitTransition},
    {u8"Webkit透视", 12, CSSPropertyID::kAliasWebkitPerspective},
    {u8"Webkit遮罩", 12, CSSPropertyID::kAliasWebkitMask},
    {u8"Webkit阴影", 12, CSSPropertyID::kAliasWebkitBoxShadow},
    {u8"上内边距", 12, CSSPropertyID::kPaddingTop},
    {u8"上升覆盖", 12, CSSPropertyID::kAscentOverride},
    {u8"下内边距", 12, CSSPropertyID::kPaddingBottom},
    {u8"下降覆盖", 12, CSSPropertyID::kDescentOverride},
    {u8"中间标记", 12, CSSPropertyID::kMarkerMid},
    {u8"主导基线", 12, CSSPropertyID::kDominantBaseline},
    {u8"主轴对齐", 12, CSSPropertyID::kJustifyContent},
    {u8"之前断页", 12, CSSPropertyID::kBreakBefore},
    {u8"之后断页", 12, CSSPropertyID::kBreakAfter},
    {u8"书写模式", 12, CSSPropertyID::kWritingMode},
    {u8"偏移位置", 12, CSSPropertyID::kOffsetPosition},
    {u8"偏移旋转", 12, CSSPropertyID::kOffsetRotate},
    {u8"偏移距离", 12, CSSPropertyID::kOffsetDistance},
    {u8"偏移路径", 12, CSSPropertyID::kOffsetPath},
    {u8"偏移锚点", 12, CSSPropertyID::kOffsetAnchor},
    {u8"停止颜色", 12, CSSPropertyID::kStopColor},
    {u8"光标动画", 12, CSSPropertyID::kCaretAnimation},
    {u8"光标颜色", 12, CSSPropertyID::kCaretColor},
    {u8"光照颜色", 12, CSSPropertyID::kLightingColor},
    {u8"内容对齐", 12, CSSPropertyID::kAlignContent},
    {u8"内联尺寸", 12, CSSPropertyID::kInlineSize},
    {u8"内联嵌入", 12, CSSPropertyID::kInsetInline},
    {u8"内联溢出", 12, CSSPropertyID::kOverflowInline},
    {u8"内联边框", 12, CSSPropertyID::kBorderInline},
    {u8"内联边距", 12, CSSPropertyID::kMarginInline},
    {u8"内部断页", 12, CSSPropertyID::kBreakInside},
    {u8"列表样式", 12, CSSPropertyID::kListStyle},
    {u8"动画合成", 12, CSSPropertyID::kAnimationComposition},
    {u8"动画名称", 12, CSSPropertyID::kAnimationName},
    {u8"动画延迟", 12, CSSPropertyID::kAnimationDelay},
    {u8"动画方向", 12, CSSPropertyID::kAnimationDirection},
    {u8"动画时长", 12, CSSPropertyID::kAnimationDuration},
    {u8"动画范围", 12, CSSPropertyID::kAnimationRange},
    {u8"动画触发", 12, CSSPropertyID::kAnimationTrigger},
    {u8"单词换行", 12, CSSPropertyID::kAliasWordWrap},
    {u8"变换原点", 12, CSSPropertyID::kTransformOrigin},
    {u8"变换样式", 12, CSSPropertyID::kTransformStyle},
    {u8"盒子阴影", 12, CSSPropertyID::kBoxShadow},
    {u8"右上圆角", 12, CSSPropertyID::kBorderTopRightRadius},
    {u8"右下圆角", 12, CSSPropertyID::kBorderBottomRightRadius},
    {u8"右内边距", 12, CSSPropertyID::kPaddingRight},
    {u8"图像方向", 12, CSSPropertyID::kImageOrientation},
    {u8"图像渲染", 12, CSSPropertyID::kImageRendering},
    {u8"块级尺寸", 12, CSSPropertyID::kBlockSize},
    {u8"块级嵌入", 12, CSSPropertyID::kInsetBlock},
    {u8"块级溢出", 12, CSSPropertyID::kOverflowBlock},
    {u8"块级边框", 12, CSSPropertyID::kBorderBlock},
    {u8"块级边距", 12, CSSPropertyID::kMarginBlock},
    {u8"垂直对齐", 12, CSSPropertyID::kVerticalAlign},
    {u8"垂直溢出", 12, CSSPropertyID::kOverflowY},
    {u8"基线偏移", 12, CSSPropertyID::kBaselineShift},
    {u8"基线来源", 12, CSSPropertyID::kBaselineSource},
    {u8"填充规则", 12, CSSPropertyID::kFillRule},
    {u8"外部形状", 12, CSSPropertyID::kShapeOutside},
    {u8"大小调整", 12, CSSPropertyID::kSizeAdjust},
    {u8"字体变体", 12, CSSPropertyID::kFontVariant},
    {u8"字体合成", 12, CSSPropertyID::kFontSynthesis},
    {u8"字体大小", 12, CSSPropertyID::kFontSize},
    {u8"字体拉伸", 12, CSSPropertyID::kFontStretch},
    {u8"字体显示", 12, CSSPropertyID::kFontDisplay},
    {u8"字段大小", 12, CSSPropertyID::kFieldSizing},
    {u8"字距调整", 12, CSSPropertyID::kFontKerning},
    {u8"定位区域", 12, CSSPropertyID::kPositionArea},
    {u8"定位尝试", 12, CSSPropertyID::kPositionTry},
    {u8"定位锚点", 12, CSSPropertyID::kPositionAnchor},
    {u8"容器名称", 12, CSSPropertyID::kContainerName},
    {u8"容器类型", 12, CSSPropertyID::kContainerType},
    {u8"对象位置", 12, CSSPropertyID::kObjectPosition},
    {u8"对象适配", 12, CSSPropertyID::kObjectFit},
    {u8"对齐基线", 12, CSSPropertyID::kAlignmentBaseline},
    {u8"左上圆角", 12, CSSPropertyID::kBorderTopLeftRadius},
    {u8"左下圆角", 12, CSSPropertyID::kBorderBottomLeftRadius},
    {u8"左内边距", 12, CSSPropertyID::kPaddingLeft},
    {u8"应用区域", 12, CSSPropertyID::kAppRegion},
    {u8"弹性基准", 12, CSSPropertyID::kFlexBasis},
    {u8"弹性增长", 12, CSSPropertyID::kFlexGrow},
    {u8"弹性换行", 12, CSSPropertyID::kFlexWrap},
    {u8"弹性收缩", 12, CSSPropertyID::kFlexShrink},
    {u8"弹性方向", 12, CSSPropertyID::kFlexDirection},
    {u8"形状渲染", 12, CSSPropertyID::kShapeRendering},
    {u8"形状边距", 12, CSSPropertyID::kShapeMargin},
    {u8"指针事件", 12, CSSPropertyID::kPointerEvents},
    {u8"描边宽度", 12, CSSPropertyID::kStrokeWidth},
    {u8"描边线端", 12, CSSPropertyID::kStrokeLinecap},
    {u8"描边连接", 12, CSSPropertyID::kStrokeLinejoin},
    {u8"插值大小", 12, CSSPropertyID::kInterpolateSize},
    {u8"放置内容", 12, CSSPropertyID::kPlaceContent},
    {u8"放置自身", 12, CSSPropertyID::kPlaceSelf},
    {u8"放置项目", 12, CSSPropertyID::kPlaceItems},
    {u8"数学样式", 12, CSSPropertyID::kMathStyle},
    {u8"数学深度", 12, CSSPropertyID::kMathDepth},
    {u8"数学移位", 12, CSSPropertyID::kMathShift},
    {u8"文本对齐", 12, CSSPropertyID::kTextAlign},
    {u8"文本强调", 12, CSSPropertyID::kTextEmphasis},
    {u8"文本换行", 12, CSSPropertyID::kTextWrap},
    {u8"文本方向", 12, CSSPropertyID::kTextOrientation},
    {u8"文本渲染", 12, CSSPropertyID::kTextRendering},
    {u8"文本溢出", 12, CSSPropertyID::kTextOverflow},
    {u8"文本缩进", 12, CSSPropertyID::kTextIndent},
    {u8"文本装饰", 12, CSSPropertyID::kTextDecoration},
    {u8"文本转换", 12, CSSPropertyID::kTextTransform},
    {u8"文本锚点", 12, CSSPropertyID::kTextAnchor},
    {u8"文本间距", 12, CSSPropertyID::kTextSpacing},
    {u8"文本阴影", 12, CSSPropertyID::kTextShadow},
    {u8"最大宽度", 12, CSSPropertyID::kMaxWidth},
    {u8"最大高度", 12, CSSPropertyID::kMaxHeight},
    {u8"最小宽度", 12, CSSPropertyID::kMinWidth},
    {u8"最小高度", 12, CSSPropertyID::kMinHeight},
    {u8"朗读方式", 12, CSSPropertyID::kSpeakAs},
    {u8"末端标记", 12, CSSPropertyID::kMarkerEnd},
    {u8"末行对齐", 12, CSSPropertyID::kTextAlignLast},
    {u8"标题位置", 12, CSSPropertyID::kCaptionSide},
    {u8"模板区域", 12, CSSPropertyID::kGridTemplateAreas},
    {u8"横向半径", 12, CSSPropertyID::kRx},
    {u8"水平溢出", 12, CSSPropertyID::kOverflowX},
    {u8"泛光颜色", 12, CSSPropertyID::kFloodColor},
    {u8"注音位置", 12, CSSPropertyID::kRubyPosition},
    {u8"注音对齐", 12, CSSPropertyID::kRubyAlign},
    {u8"混合模式", 12, CSSPropertyID::kMixBlendMode},
    {u8"溢出换行", 12, CSSPropertyID::kOverflowWrap},
    {u8"溢出锚定", 12, CSSPropertyID::kOverflowAnchor},
    {u8"滚动条槽", 12, CSSPropertyID::kScrollbarGutter},
    {u8"滚动行为", 12, CSSPropertyID::kScrollBehavior},
    {u8"滚动起点", 12, CSSPropertyID::kScrollInitialTarget},
    {u8"滚动边距", 12, CSSPropertyID::kScrollMargin},
    {u8"用户选择", 12, CSSPropertyID::kUserSelect},
    {u8"矢量效果", 12, CSSPropertyID::kVectorEffect},
    {u8"空单元格", 12, CSSPropertyID::kEmptyCells},
    {u8"空白处理", 12, CSSPropertyID::kWhiteSpace},
    {u8"空白折叠", 12, CSSPropertyID::kWhiteSpaceCollapse},
    {u8"累加符号", 12, CSSPropertyID::kAdditiveSymbols},
    {u8"纵向半径", 12, CSSPropertyID::kRy},
    {u8"绘制顺序", 12, CSSPropertyID::kPaintOrder},
    {u8"缓冲渲染", 12, CSSPropertyID::kBufferedRendering},
    {u8"网格区域", 12, CSSPropertyID::kGridArea},
    {u8"网格模板", 12, CSSPropertyID::kGridTemplate},
    {u8"网格间距", 12, CSSPropertyID::kAliasGridGap},
    {u8"背景位置", 12, CSSPropertyID::kBackgroundPosition},
    {u8"背景原点", 12, CSSPropertyID::kBackgroundOrigin},
    {u8"背景图像", 12, CSSPropertyID::kBackgroundImage},
    {u8"背景大小", 12, CSSPropertyID::kBackgroundSize},
    {u8"背景滤镜", 12, CSSPropertyID::kBackdropFilter},
    {u8"背景裁剪", 12, CSSPropertyID::kBackgroundClip},
    {u8"背景重复", 12, CSSPropertyID::kBackgroundRepeat},
    {u8"背景附着", 12, CSSPropertyID::kBackgroundAttachment},
    {u8"自动排列", 12, CSSPropertyID::kGridAutoFlow},
    {u8"行数限制", 12, CSSPropertyID::kLineClamp},
    {u8"行距覆盖", 12, CSSPropertyID::kLineGapOverride},
    {u8"表格布局", 12, CSSPropertyID::kTableLayout},
    {u8"裁剪规则", 12, CSSPropertyID::kClipRule},
    {u8"裁剪路径", 12, CSSPropertyID::kClipPath},
    {u8"覆盖颜色", 12, CSSPropertyID::kOverrideColors},
    {u8"触摸动作", 12, CSSPropertyID::kTouchAction},
    {u8"调整大小", 12, CSSPropertyID::kResize},
    {u8"起始标记", 12, CSSPropertyID::kMarkerStart},
    {u8"路径数据", 12, CSSPropertyID::kD},
    {u8"轮廓偏移", 12, CSSPropertyID::kOutlineOffset},
    {u8"轮廓宽度", 12, CSSPropertyID::kOutlineWidth},
    {u8"轮廓样式", 12, CSSPropertyID::kOutlineStyle},
    {u8"轮廓颜色", 12, CSSPropertyID::kOutlineColor},
    {u8"边框合并", 12, CSSPropertyID::kBorderCollapse},
    {u8"边框图像", 12, CSSPropertyID::kBorderImage},
    {u8"边框宽度", 12, CSSPropertyID::kBorderWidth},
    {u8"边框样式", 12, CSSPropertyID::kBorderStyle},
    {u8"边框间距", 12, CSSPropertyID::kBorderSpacing},
    {u8"边框颜色", 12, CSSPropertyID::kBorderColor},
    {u8"过渡属性", 12, CSSPropertyID::kTransitionProperty},
    {u8"过渡延迟", 12, CSSPropertyID::kTransitionDelay},
    {u8"过渡时长", 12, CSSPropertyID::kTransitionDuration},
    {u8"过渡行为", 12, CSSPropertyID::kTransitionBehavior},
    {u8"透视原点", 12, CSSPropertyID::kPerspectiveOrigin},
    {u8"遮罩位置", 12, CSSPropertyID::kMaskPosition},
    {u8"遮罩原点", 12, CSSPropertyID::kMaskOrigin},
    {u8"遮罩合成", 12, CSSPropertyID::kMaskComposite},
    {u8"遮罩图像", 12, CSSPropertyID::kMaskImage},
    {u8"遮罩大小", 12, CSSPropertyID::kMaskSize},
    {u8"遮罩模式", 12, CSSPropertyID::kMaskMode},
    {u8"遮罩类型", 12, CSSPropertyID::kMaskType},
    {u8"遮罩裁剪", 12, CSSPropertyID::kMaskClip},
    {u8"遮罩重复", 12, CSSPropertyID::kMaskRepeat},
    {u8"配色方案", 12, CSSPropertyID::kColorScheme},
    {u8"锚点名称", 12, CSSPropertyID::kAnchorName},
    {u8"页内断页", 12, CSSPropertyID::kPageBreakInside},
    {u8"页前断页", 12, CSSPropertyID::kPageBreakBefore},
    {u8"页后断页", 12, CSSPropertyID::kPageBreakAfter},
    {u8"页面方向", 12, CSSPropertyID::kPageOrientation},
    {u8"页面缩放", 12, CSSPropertyID::kZoom},
    {u8"项目对齐", 12, CSSPropertyID::kJustifyItems},
    {u8"颜色插值", 12, CSSPropertyID::kColorInterpolation},
    {u8"颜色渲染", 12, CSSPropertyID::kColorRendering},
    {u8"Unicode双向", 13, CSSPropertyID::kUnicodeBidi},
    {u8"Unicode范围", 13, CSSPropertyID::kUnicodeRange},
    {u8"Webkit列规则", 15, CSSPropertyID::kAliasWebkitColumnRule},
    {u8"Webkit列跨行", 15, CSSPropertyID::kAliasWebkitColumnSpan},
    {u8"Webkit列间距", 15, CSSPropertyID::kAliasWebkitColumnGap},
    {u8"Webkit弹性流", 15, CSSPropertyID::kAliasWebkitFlexFlow},
    {u8"Webkit断字符", 15, CSSPropertyID::kAliasWebkitHyphenateCharacter},
    {u8"Webkit方方向", 15, CSSPropertyID::kWebkitBoxDirection},
    {u8"Webkit方朝向", 15, CSSPropertyID::kWebkitBoxOrient},
    {u8"Webkit盒分布", 15, CSSPropertyID::kWebkitBoxPack},
    {u8"Webkit盒反射", 15, CSSPropertyID::kWebkitBoxReflect},
    {u8"Webkit盒对齐", 15, CSSPropertyID::kWebkitBoxAlign},
    {u8"Webkit盒弹性", 15, CSSPropertyID::kWebkitBoxFlex},
    {u8"Webkit盒模型", 15, CSSPropertyID::kAliasWebkitBoxSizing},
    {u8"Webkit透明度", 15, CSSPropertyID::kAliasWebkitOpacity},
    {u8"上边框宽度", 15, CSSPropertyID::kBorderTopWidth},
    {u8"上边框样式", 15, CSSPropertyID::kBorderTopStyle},
    {u8"上边框颜色", 15, CSSPropertyID::kBorderTopColor},
    {u8"下边框宽度", 15, CSSPropertyID::kBorderBottomWidth},
    {u8"下边框样式", 15, CSSPropertyID::kBorderBottomStyle},
    {u8"下边框颜色", 15, CSSPropertyID::kBorderBottomColor},
    {u8"停止透明度", 15, CSSPropertyID::kStopOpacity},
    {u8"内容可见性", 15, CSSPropertyID::kContentVisibility},
    {u8"内联内边距", 15, CSSPropertyID::kPaddingInline},
    {u8"内部空行高", 15, CSSPropertyID::kInternalEmptyLineHeight},
    {u8"列规则宽度", 15, CSSPropertyID::kColumnRuleWidth},
    {u8"列规则断页", 15, CSSPropertyID::kColumnRuleBreak},
    {u8"列规则样式", 15, CSSPropertyID::kColumnRuleStyle},
    {u8"列规则颜色", 15, CSSPropertyID::kColumnRuleColor},
    {u8"制表符大小", 15, CSSPropertyID::kTabSize},
    {u8"动画时间线", 15, CSSPropertyID::kAnimationTimeline},
    {u8"右边框宽度", 15, CSSPropertyID::kBorderRightWidth},
    {u8"右边框样式", 15, CSSPropertyID::kBorderRightStyle},
    {u8"右边框颜色", 15, CSSPropertyID::kBorderRightColor},
    {u8"块级内边距", 15, CSSPropertyID::kPaddingBlock},
    {u8"基础调色板", 15, CSSPropertyID::kBasePalette},
    {u8"填充透明度", 15, CSSPropertyID::kFillOpacity},
    {u8"字体调色板", 15, CSSPropertyID::kFontPalette},
    {u8"定位可见性", 15, CSSPropertyID::kPositionVisibility},
    {u8"对象视图框", 15, CSSPropertyID::kObjectViewBox},
    {u8"左边框宽度", 15, CSSPropertyID::kBorderLeftWidth},
    {u8"左边框样式", 15, CSSPropertyID::kBorderLeftStyle},
    {u8"左边框颜色", 15, CSSPropertyID::kBorderLeftColor},
    {u8"描边透明度", 15, CSSPropertyID::kStrokeOpacity},
    {u8"文本框修剪", 15, CSSPropertyID::kTextBoxTrim},
    {u8"文本框边缘", 15, CSSPropertyID::kTextBoxEdge},
    {u8"文本装饰线", 15, CSSPropertyID::kTextDecorationLine},
    {u8"泛光透明度", 15, CSSPropertyID::kFloodOpacity},
    {u8"滚动上边距", 15, CSSPropertyID::kScrollMarginTop},
    {u8"滚动下边距", 15, CSSPropertyID::kScrollMarginBottom},
    {u8"滚动内边距", 15, CSSPropertyID::kScrollPadding},
    {u8"滚动右边距", 15, CSSPropertyID::kScrollMarginRight},
    {u8"滚动左边距", 15, CSSPropertyID::kScrollMarginLeft},
    {u8"滚动时间线", 15, CSSPropertyID::kScrollTimeline},
    {u8"滚动条宽度", 15, CSSPropertyID::kScrollbarWidth},
    {u8"滚动条颜色", 15, CSSPropertyID::kScrollbarColor},
    {u8"滚动标记组", 15, CSSPropertyID::kScrollMarkerGroup},
    {u8"网格列间距", 15, CSSPropertyID::kAliasGridColumnGap},
    {u8"网格行间距", 15, CSSPropertyID::kAliasGridRowGap},
    {u8"背面可见性", 15, CSSPropertyID::kBackfaceVisibility},
    {u8"视图时间线", 15, CSSPropertyID::kViewTimeline},
    {u8"视图过渡类", 15, CSSPropertyID::kViewTransitionClass},
    {u8"视图过渡组", 15, CSSPropertyID::kViewTransitionGroup},
    {u8"计数器设置", 15, CSSPropertyID::kCounterSet},
    {u8"计数器递增", 15, CSSPropertyID::kCounterIncrement},
    {u8"计数器重置", 15, CSSPropertyID::kCounterReset},
    {u8"边框图像源", 15, CSSPropertyID::kBorderImageSource},
    {u8"锚点作用域", 15, CSSPropertyID::kAnchorScope},
    {u8"EPUB书写模式", 16, CSSPropertyID::kAliasEpubWritingMode},
    {u8"EPUB文本合并", 16, CSSPropertyID::kAliasEpubTextCombine},
    {u8"EPUB文本强调", 16, CSSPropertyID::kAliasEpubTextEmphasis},
    {u8"EPUB文本方向", 16, CSSPropertyID::kAliasEpubTextOrientation},
    {u8"EPUB文本转换", 16, CSSPropertyID::kAliasEpubTextTransform},
    {u8"EPUB标题位置", 16, CSSPropertyID::kAliasEpubCaptionSide},
    {u8"Webkit主轴对齐", 18, CSSPropertyID::kAliasWebkitJustifyContent},
    {u8"Webkit书写模式", 18, CSSPropertyID::kWebkitWritingMode},
    {u8"Webkit内容对齐", 18, CSSPropertyID::kAliasWebkitAlignContent},
    {u8"Webkit列内断页", 18, CSSPropertyID::kWebkitColumnBreakInside},
    {u8"Webkit列前断页", 18, CSSPropertyID::kWebkitColumnBreakBefore},
    {u8"Webkit列后断页", 18, CSSPropertyID::kWebkitColumnBreakAfter},
    {u8"Webkit前置边框", 18, CSSPropertyID::kAliasWebkitBorderBefore},
    {u8"Webkit前置边距", 18, CSSPropertyID::kAliasWebkitMarginBefore},
    {u8"Webkit动画名称", 18, CSSPropertyID::kAliasWebkitAnimationName},
    {u8"Webkit动画延迟", 18, CSSPropertyID::kAliasWebkitAnimationDelay},
    {u8"Webkit动画方向", 18, CSSPropertyID::kAliasWebkitAnimationDirection},
    {u8"Webkit动画时长", 18, CSSPropertyID::kAliasWebkitAnimationDuration},
    {u8"Webkit区域设置", 18, CSSPropertyID::kWebkitLocale},
    {u8"Webkit变换原点", 18, CSSPropertyID::kAliasWebkitTransformOrigin},
    {u8"Webkit变换样式", 18, CSSPropertyID::kAliasWebkitTransformStyle},
    {u8"Webkit右上圆角", 18, CSSPropertyID::kAliasWebkitBorderTopRightRadius},
    {u8"Webkit右下圆角", 18, CSSPropertyID::kAliasWebkitBorderBottomRightRadius},
    {u8"Webkit后置边框", 18, CSSPropertyID::kAliasWebkitBorderAfter},
    {u8"Webkit后置边距", 18, CSSPropertyID::kAliasWebkitMarginAfter},
    {u8"Webkit外部形状", 18, CSSPropertyID::kAliasWebkitShapeOutside},
    {u8"Webkit字体平滑", 18, CSSPropertyID::kWebkitFontSmoothing},
    {u8"Webkit左上圆角", 18, CSSPropertyID::kAliasWebkitBorderTopLeftRadius},
    {u8"Webkit左下圆角", 18, CSSPropertyID::kAliasWebkitBorderBottomLeftRadius},
    {u8"Webkit应用区域", 18, CSSPropertyID::kAliasWebkitAppRegion},
    {u8"Webkit弹性基准", 18, CSSPropertyID::kAliasWebkitFlexBasis},
    {u8"Webkit弹性增长", 18, CSSPropertyID::kAliasWebkitFlexGrow},
    {u8"Webkit弹性换行", 18, CSSPropertyID::kAliasWebkitFlexWrap},
    {u8"Webkit弹性收缩", 18, CSSPropertyID::kAliasWebkitFlexShrink},
    {u8"Webkit弹性方向", 18, CSSPropertyID::kAliasWebkitFlexDirection},
    {u8"Webkit形状边距", 18, CSSPropertyID::kAliasWebkitShapeMargin},
    {u8"Webkit文本合并", 18, CSSPropertyID::kWebkitTextCombine},
    {u8"Webkit文本安全", 18, CSSPropertyID::kWebkitTextSecurity},
    {u8"Webkit文本强调", 18, CSSPropertyID::kAliasWebkitTextEmphasis},
    {u8"Webkit文本描边", 18, CSSPropertyID::kWebkitTextStroke},
    {u8"Webkit文本方向", 18, CSSPropertyID::kWebkitTextOrientation},
    {u8"Webkit注音位置", 18, CSSPropertyID::kWebkitRubyPosition},
    {u8"Webkit用户修改", 18, CSSPropertyID::kWebkitUserModify},
    {u8"Webkit用户拖拽", 18, CSSPropertyID::kWebkitUserDrag},
    {u8"Webkit用户选择", 18, CSSPropertyID::kAliasWebkitUserSelect},
    {u8"Webkit盒排序组", 18, CSSPropertyID::kWebkitBoxOrdinalGroup},
    {u8"Webkit结束边框", 18, CSSPropertyID::kAliasWebkitBorderEnd},
    {u8"Webkit结束边距", 18, CSSPropertyID::kAliasWebkitMarginEnd},
    {u8"Webkit背景原点", 18, CSSPropertyID::kAliasWebkitBackgroundOrigin},
    {u8"Webkit背景大小", 18, CSSPropertyID::kAliasWebkitBackgroundSize},
    {u8"Webkit背景裁剪", 18, CSSPropertyID::kAliasWebkitBackgroundClip},
    {u8"Webkit自身对齐", 18, CSSPropertyID::kAliasWebkitAlignSelf},
    {u8"Webkit行数限制", 18, CSSPropertyID::kWebkitLineClamp},
    {u8"Webkit裁剪路径", 18, CSSPropertyID::kAliasWebkitClipPath},
    {u8"Webkit起始边框", 18, CSSPropertyID::kAliasWebkitBorderStart},
    {u8"Webkit起始边距", 18, CSSPropertyID::kAliasWebkitMarginStart},
    {u8"Webkit边框图像", 18, CSSPropertyID::kWebkitBorderImage},
    {u8"Webkit过渡属性", 18, CSSPropertyID::kAliasWebkitTransitionProperty},
    {u8"Webkit过渡延迟", 18, CSSPropertyID::kAliasWebkitTransitionDelay},
    {u8"Webkit过渡时长", 18, CSSPropertyID::kAliasWebkitTransitionDuration},
    {u8"Webkit透视原点", 18, CSSPropertyID::kAliasWebkitPerspectiveOrigin},
    {u8"Webkit逻辑宽度", 18, CSSPropertyID::kAliasWebkitLogicalWidth},
    {u8"Webkit逻辑高度", 18, CSSPropertyID::kAliasWebkitLogicalHeight},
    {u8"Webkit遮罩位置", 18, CSSPropertyID::kAliasWebkitMaskPosition},
    {u8"Webkit遮罩原点", 18, CSSPropertyID::kAliasWebkitMaskOrigin},
    {u8"Webkit遮罩合成", 18, CSSPropertyID::kAliasWebkitMaskComposite},
    {u8"Webkit遮罩图像", 18, CSSPropertyID::kAliasWebkitMaskImage},
    {u8"Webkit遮罩大小", 18, CSSPropertyID::kAliasWebkitMaskSize},
    {u8"Webkit遮罩裁剪", 18, CSSPropertyID::kAliasWebkitMaskClip},
    {u8"Webkit遮罩重复", 18, CSSPropertyID::kAliasWebkitMaskRepeat},
    {u8"主轴自身对齐", 18, CSSPropertyID::kJustifySelf},
    {u8"内联结束嵌入", 18, CSSPropertyID::kInsetInlineEnd},
    {u8"内联结束边框", 18, CSSPropertyID::kBorderInlineEnd},
    {u8"内联结束边距", 18, CSSPropertyID::kMarginInlineEnd},
    {u8"内联起始嵌入", 18, CSSPropertyID::kInsetInlineStart},
    {u8"内联起始边框", 18, CSSPropertyID::kBorderInlineStart},
    {u8"内联起始边距", 18, CSSPropertyID::kMarginInlineStart},
    {u8"内联边框宽度", 18, CSSPropertyID::kBorderInlineWidth},
    {u8"内联边框样式", 18, CSSPropertyID::kBorderInlineStyle},
    {u8"内联边框颜色", 18, CSSPropertyID::kBorderInlineColor},
    {u8"内部强制颜色", 18, CSSPropertyID::kInternalForcedColor},
    {u8"列表样式位置", 18, CSSPropertyID::kListStylePosition},
    {u8"列表样式图像", 18, CSSPropertyID::kListStyleImage},
    {u8"列表样式类型", 18, CSSPropertyID::kListStyleType},
    {u8"动态范围限制", 18, CSSPropertyID::kDynamicRangeLimit},
    {u8"动画填充模式", 18, CSSPropertyID::kAnimationFillMode},
    {u8"动画播放状态", 18, CSSPropertyID::kAnimationPlayState},
    {u8"动画时间函数", 18, CSSPropertyID::kAnimationTimingFunction},
    {u8"动画范围终点", 18, CSSPropertyID::kAnimationRangeEnd},
    {u8"动画范围起点", 18, CSSPropertyID::kAnimationRangeStart},
    {u8"动画迭代次数", 18, CSSPropertyID::kAnimationIterationCount},
    {u8"固有包含宽度", 18, CSSPropertyID::kContainIntrinsicWidth},
    {u8"固有包含尺寸", 18, CSSPropertyID::kContainIntrinsicSize},
    {u8"固有包含高度", 18, CSSPropertyID::kContainIntrinsicHeight},
    {u8"块级结束嵌入", 18, CSSPropertyID::kInsetBlockEnd},
    {u8"块级结束边框", 18, CSSPropertyID::kBorderBlockEnd},
    {u8"块级结束边距", 18, CSSPropertyID::kMarginBlockEnd},
    {u8"块级起始嵌入", 18, CSSPropertyID::kInsetBlockStart},
    {u8"块级起始边框", 18, CSSPropertyID::kBorderBlockStart},
    {u8"块级起始边距", 18, CSSPropertyID::kMarginBlockStart},
    {u8"块级边框宽度", 18, CSSPropertyID::kBorderBlockWidth},
    {u8"块级边框样式", 18, CSSPropertyID::kBorderBlockStyle},
    {u8"块级边框颜色", 18, CSSPropertyID::kBorderBlockColor},
    {u8"字体东亚变体", 18, CSSPropertyID::kFontVariantEastAsian},
    {u8"字体位置变体", 18, CSSPropertyID::kFontVariantPosition},
    {u8"字体光学大小", 18, CSSPropertyID::kFontOpticalSizing},
    {u8"字体变化设置", 18, CSSPropertyID::kFontVariationSettings},
    {u8"字体合成样式", 18, CSSPropertyID::kFontSynthesisStyle},
    {u8"字体合成粗细", 18, CSSPropertyID::kFontSynthesisWeight},
    {u8"字体大写变体", 18, CSSPropertyID::kFontVariantCaps},
    {u8"字体大小调整", 18, CSSPropertyID::kFontSizeAdjust},
    {u8"字体数字变体", 18, CSSPropertyID::kFontVariantNumeric},
    {u8"字体替代变体", 18, CSSPropertyID::kFontVariantAlternates},
    {u8"字体特性设置", 18, CSSPropertyID::kFontFeatureSettings},
    {u8"字体表情变体", 18, CSSPropertyID::kFontVariantEmoji},
    {u8"字体连字变体", 18, CSSPropertyID::kFontVariantLigatures},
    {u8"定位尝试回退", 18, CSSPropertyID::kPositionTryFallbacks},
    {u8"定位尝试顺序", 18, CSSPropertyID::kPositionTryOrder},
    {u8"强制颜色调整", 18, CSSPropertyID::kForcedColorAdjust},
    {u8"形状图像阈值", 18, CSSPropertyID::kShapeImageThreshold},
    {u8"描边斜接限制", 18, CSSPropertyID::kStrokeMiterlimit},
    {u8"描边虚线偏移", 18, CSSPropertyID::kStrokeDashoffset},
    {u8"描边虚线阵列", 18, CSSPropertyID::kStrokeDasharray},
    {u8"文本大小调整", 18, CSSPropertyID::kTextSizeAdjust},
    {u8"文本强调位置", 18, CSSPropertyID::kTextEmphasisPosition},
    {u8"文本强调样式", 18, CSSPropertyID::kTextEmphasisStyle},
    {u8"文本强调颜色", 18, CSSPropertyID::kTextEmphasisColor},
    {u8"文本换行样式", 18, CSSPropertyID::kTextWrapStyle},
    {u8"文本换行模式", 18, CSSPropertyID::kTextWrapMode},
    {u8"文本竖排合并", 18, CSSPropertyID::kTextCombineUpright},
    {u8"文本自动间距", 18, CSSPropertyID::kTextAutospace},
    {u8"文本装饰样式", 18, CSSPropertyID::kTextDecorationStyle},
    {u8"文本装饰粗细", 18, CSSPropertyID::kTextDecorationThickness},
    {u8"文本装饰颜色", 18, CSSPropertyID::kTextDecorationColor},
    {u8"文本间距修剪", 18, CSSPropertyID::kTextSpacingTrim},
    {u8"断字符数限制", 18, CSSPropertyID::kHyphenateLimitChars},
    {u8"时间线作用域", 18, CSSPropertyID::kTimelineScope},
    {u8"最大内联尺寸", 18, CSSPropertyID::kMaxInlineSize},
    {u8"最大块级尺寸", 18, CSSPropertyID::kMaxBlockSize},
    {u8"最小内联尺寸", 18, CSSPropertyID::kMinInlineSize},
    {u8"最小块级尺寸", 18, CSSPropertyID::kMinBlockSize},
    {u8"溢出裁剪边距", 18, CSSPropertyID::kOverflowClipMargin},
    {u8"滚动上内边距", 18, CSSPropertyID::kScrollPaddingTop},
    {u8"滚动下内边距", 18, CSSPropertyID::kScrollPaddingBottom},
    {u8"滚动内联边距", 18, CSSPropertyID::kScrollMarginInline},
    {u8"滚动右内边距", 18, CSSPropertyID::kScrollPaddingRight},
    {u8"滚动吸附停止", 18, CSSPropertyID::kScrollSnapStop},
    {u8"滚动吸附对齐", 18, CSSPropertyID::kScrollSnapAlign},
    {u8"滚动吸附类型", 18, CSSPropertyID::kScrollSnapType},
    {u8"滚动块级边距", 18, CSSPropertyID::kScrollMarginBlock},
    {u8"滚动左内边距", 18, CSSPropertyID::kScrollPaddingLeft},
    {u8"滚动时间线轴", 18, CSSPropertyID::kScrollTimelineAxis},
    {u8"滤镜颜色插值", 18, CSSPropertyID::kColorInterpolationFilters},
    {u8"盒子装饰断行", 18, CSSPropertyID::kBoxDecorationBreak},
    {u8"结束结束圆角", 18, CSSPropertyID::kBorderEndEndRadius},
    {u8"结束起始圆角", 18, CSSPropertyID::kBorderEndStartRadius},
    {u8"背景垂直位置", 18, CSSPropertyID::kBackgroundPositionY},
    {u8"背景水平位置", 18, CSSPropertyID::kBackgroundPositionX},
    {u8"背景混合模式", 18, CSSPropertyID::kBackgroundBlendMode},
    {u8"视图时间线轴", 18, CSSPropertyID::kViewTimelineAxis},
    {u8"视图过渡名称", 18, CSSPropertyID::kViewTransitionName},
    {u8"起始结束圆角", 18, CSSPropertyID::kBorderStartEndRadius},
    {u8"起始起始圆角", 18, CSSPropertyID::kBorderStartStartRadius},
    {u8"边框图像切片", 18, CSSPropertyID::kBorderImageSlice},
    {u8"边框图像外扩", 18, CSSPropertyID::kBorderImageOutset},
    {u8"边框图像宽度", 18, CSSPropertyID::kBorderImageWidth},
    {u8"边框图像重复", 18, CSSPropertyID::kBorderImageRepeat},
    {u8"过度滚动行为", 18, CSSPropertyID::kOverscrollBehavior},
    {u8"过渡时间函数", 18, CSSPropertyID::kTransitionTimingFunction},
    {u8"Webkit变换原点X", 19, CSSPropertyID::kWebkitTransformOriginX},
    {u8"Webkit变换原点Y", 19, CSSPropertyID::kWebkitTransformOriginY},
    {u8"Webkit变换原点Z", 19, CSSPropertyID::kWebkitTransformOriginZ},
    {u8"Webkit透视原点X", 19, CSSPropertyID::kWebkitPerspectiveOriginX},
    {u8"Webkit透视原点Y", 19, CSSPropertyID::kWebkitPerspectiveOriginY},
    {u8"Webkit列规则宽度", 21, CSSPropertyID::kAliasWebkitColumnRuleWidth},
    {u8"Webkit列规则样式", 21, CSSPropertyID::kAliasWebkitColumnRuleStyle},
    {u8"Webkit列规则颜色", 21, CSSPropertyID::kAliasWebkitColumnRuleColor},
    {u8"Webkit前置内边距", 21, CSSPropertyID::kAliasWebkitPaddingBefore},
    {u8"Webkit后置内边距", 21, CSSPropertyID::kAliasWebkitPaddingAfter},
    {u8"Webkit结束内边距", 21, CSSPropertyID::kAliasWebkitPaddingEnd},
    {u8"Webkit背面可见性", 21, CSSPropertyID::kAliasWebkitBackfaceVisibility},
    {u8"Webkit起始内边距", 21, CSSPropertyID::kAliasWebkitPaddingStart},
    {u8"Webkit遮罩框图像", 21, CSSPropertyID::kWebkitMaskBoxImage},
    {u8"交叉轴自身对齐", 21, CSSPropertyID::kAlignSelf},
    {u8"内联结束内边距", 21, CSSPropertyID::kPaddingInlineEnd},
    {u8"内联起始内边距", 21, CSSPropertyID::kPaddingInlineStart},
    {u8"内部访问后填充", 21, CSSPropertyID::kInternalVisitedFill},
    {u8"内部访问后描边", 21, CSSPropertyID::kInternalVisitedStroke},
    {u8"内部访问后颜色", 21, CSSPropertyID::kInternalVisitedColor},
    {u8"块级结束内边距", 21, CSSPropertyID::kPaddingBlockEnd},
    {u8"块级起始内边距", 21, CSSPropertyID::kPaddingBlockStart},
    {u8"文本下划线位置", 21, CSSPropertyID::kTextUnderlinePosition},
    {u8"文本下划线偏移", 21, CSSPropertyID::kTextUnderlineOffset},
    {u8"源试用测试属性", 21, CSSPropertyID::kOriginTrialTestProperty},
    {u8"滚动内联内边距", 21, CSSPropertyID::kScrollPaddingInline},
    {u8"滚动块级内边距", 21, CSSPropertyID::kScrollPaddingBlock},
    {u8"滚动时间线名称", 21, CSSPropertyID::kScrollTimelineName},
    {u8"视图时间线名称", 21, CSSPropertyID::kViewTimelineName},
    {u8"视图时间线嵌入", 21, CSSPropertyID::kViewTimelineInset},
    {u8"EPUB文本强调样式", 22, CSSPropertyID::kAliasEpubTextEmphasisStyle},
    {u8"EPUB文本强调颜色", 22, CSSPropertyID::kAliasEpubTextEmphasisColor},
    {u8"Webkit从右到左排序", 24, CSSPropertyID::kWebkitRtlOrdering},
    {u8"Webkit前置边框宽度", 24, CSSPropertyID::kAliasWebkitBorderBeforeWidth},
    {u8"Webkit前置边框样式", 24, CSSPropertyID::kAliasWebkitBorderBeforeStyle},
    {u8"Webkit前置边框颜色", 24, CSSPropertyID::kAliasWebkitBorderBeforeColor},
    {u8"Webkit动画填充模式", 24, CSSPropertyID::kAliasWebkitAnimationFillMode},
    {u8"Webkit动画播放状态", 24, CSSPropertyID::kAliasWebkitAnimationPlayState},
    {u8"Webkit动画时间函数", 24, CSSPropertyID::kAliasWebkitAnimationTimingFunction},
    {u8"Webkit动画迭代次数", 24, CSSPropertyID::kAliasWebkitAnimationIterationCount},
    {u8"Webkit后置边框宽度", 24, CSSPropertyID::kAliasWebkitBorderAfterWidth},
    {u8"Webkit后置边框样式", 24, CSSPropertyID::kAliasWebkitBorderAfterStyle},
    {u8"Webkit后置边框颜色", 24, CSSPropertyID::kAliasWebkitBorderAfterColor},
    {u8"Webkit垂直边框间距", 24, CSSPropertyID::kWebkitBorderVerticalSpacing},
    {u8"Webkit字体特性设置", 24, CSSPropertyID::kAliasWebkitFontFeatureSettings},
    {u8"Webkit形状图像阈值", 24, CSSPropertyID::kAliasWebkitShapeImageThreshold},
    {u8"Webkit文本填充颜色", 24, CSSPropertyID::kWebkitTextFillColor},
    {u8"Webkit文本大小调整", 24, CSSPropertyID::kAliasWebkitTextSizeAdjust},
    {u8"Webkit文本强调位置", 24, CSSPropertyID::kAliasWebkitTextEmphasisPosition},
    {u8"Webkit文本强调样式", 24, CSSPropertyID::kAliasWebkitTextEmphasisStyle},
    {u8"Webkit文本强调颜色", 24, CSSPropertyID::kAliasWebkitTextEmphasisColor},
    {u8"Webkit文本描边宽度", 24, CSSPropertyID::kWebkitTextStrokeWidth},
    {u8"Webkit文本描边颜色", 24, CSSPropertyID::kWebkitTextStrokeColor},
    {u8"Webkit最大逻辑宽度", 24, CSSPropertyID::kAliasWebkitMaxLogicalWidth},
    {u8"Webkit最大逻辑高度", 24, CSSPropertyID::kAliasWebkitMaxLogicalHeight},
    {u8"Webkit最小逻辑宽度", 24, CSSPropertyID::kAliasWebkitMinLogicalWidth},
    {u8"Webkit最小逻辑高度", 24, CSSPropertyID::kAliasWebkitMinLogicalHeight},
    {u8"Webkit水平边框间距", 24, CSSPropertyID::kWebkitBorderHorizontalSpacing},
    {u8"Webkit点击高亮颜色", 24, CSSPropertyID::kWebkitTapHighlightColor},
    {u8"Webkit生效文本装饰", 24, CSSPropertyID::kWebkitTextDecorationsInEffect},
    {u8"Webkit盒子装饰断行", 24, CSSPropertyID::kWebkitBoxDecorationBreak},
    {u8"Webkit结束边框宽度", 24, CSSPropertyID::kAliasWebkitBorderEndWidth},
    {u8"Webkit结束边框样式", 24, CSSPropertyID::kAliasWebkitBorderEndStyle},
    {u8"Webkit结束边框颜色", 24, CSSPropertyID::kAliasWebkitBorderEndColor},
    {u8"Webkit起始边框宽度", 24, CSSPropertyID::kAliasWebkitBorderStartWidth},
    {u8"Webkit起始边框样式", 24, CSSPropertyID::kAliasWebkitBorderStartStyle},
    {u8"Webkit起始边框颜色", 24, CSSPropertyID::kAliasWebkitBorderStartColor},
    {u8"Webkit过渡时间函数", 24, CSSPropertyID::kAliasWebkitTransitionTimingFunction},
    {u8"Webkit遮罩垂直位置", 24, CSSPropertyID::kWebkitMaskPositionY},
    {u8"Webkit遮罩框图像源", 24, CSSPropertyID::kWebkitMaskBoxImageSource},
    {u8"Webkit遮罩水平位置", 24, CSSPropertyID::kWebkitMaskPositionX},
    {u8"内联结束边框宽度", 24, CSSPropertyID::kBorderInlineEndWidth},
    {u8"内联结束边框样式", 24, CSSPropertyID::kBorderInlineEndStyle},
    {u8"内联结束边框颜色", 24, CSSPropertyID::kBorderInlineEndColor},
    {u8"内联起始边框宽度", 24, CSSPropertyID::kBorderInlineStartWidth},
    {u8"内联起始边框样式", 24, CSSPropertyID::kBorderInlineStartStyle},
    {u8"内联起始边框颜色", 24, CSSPropertyID::kBorderInlineStartColor},
    {u8"内联过度滚动行为", 24, CSSPropertyID::kOverscrollBehaviorInline},
    {u8"内部块级内容对齐", 24, CSSPropertyID::kInternalAlignContentBlock},
    {u8"内部字体大小增量", 24, CSSPropertyID::kInternalFontSizeDelta},
    {u8"内部强制背景颜色", 24, CSSPropertyID::kInternalForcedBackgroundColor},
    {u8"内部强制轮廓颜色", 24, CSSPropertyID::kInternalForcedOutlineColor},
    {u8"内部强制边框颜色", 24, CSSPropertyID::kInternalForcedBorderColor},
    {u8"固有包含内联尺寸", 24, CSSPropertyID::kContainIntrinsicInlineSize},
    {u8"固有包含块级尺寸", 24, CSSPropertyID::kContainIntrinsicBlockSize},
    {u8"块级结束边框宽度", 24, CSSPropertyID::kBorderBlockEndWidth},
    {u8"块级结束边框样式", 24, CSSPropertyID::kBorderBlockEndStyle},
    {u8"块级结束边框颜色", 24, CSSPropertyID::kBorderBlockEndColor},
    {u8"块级起始边框宽度", 24, CSSPropertyID::kBorderBlockStartWidth},
    {u8"块级起始边框样式", 24, CSSPropertyID::kBorderBlockStartStyle},
    {u8"块级起始边框颜色", 24, CSSPropertyID::kBorderBlockStartColor},
    {u8"块级过度滚动行为", 24, CSSPropertyID::kOverscrollBehaviorBlock},
    {u8"垂直过度滚动行为", 24, CSSPropertyID::kOverscrollBehaviorY},
    {u8"字体合成小型大写", 24, CSSPropertyID::kFontSynthesisSmallCaps},
    {u8"文本装饰避让笔画", 24, CSSPropertyID::kTextDecorationSkipInk},
    {u8"水平过度滚动行为", 24, CSSPropertyID::kOverscrollBehaviorX},
    {u8"滚动内联结束边距", 24, CSSPropertyID::kScrollMarginInlineEnd},
    {u8"滚动内联起始边距", 24, CSSPropertyID::kScrollMarginInlineStart},
    {u8"滚动块级结束边距", 24, CSSPropertyID::kScrollMarginBlockEnd},
    {u8"滚动块级起始边距", 24, CSSPropertyID::kScrollMarginBlockStart},
    {u8"Webkit遮罩框图像切片", 27, CSSPropertyID::kWebkitMaskBoxImageSlice},
    {u8"Webkit遮罩框图像外扩", 27, CSSPropertyID::kWebkitMaskBoxImageOutset},
    {u8"Webkit遮罩框图像宽度", 27, CSSPropertyID::kWebkitMaskBoxImageWidth},
    {u8"Webkit遮罩框图像重复", 27, CSSPropertyID::kWebkitMaskBoxImageRepeat},
    {u8"内部强制访问后颜色", 27, CSSPropertyID::kInternalForcedVisitedColor},
    {u8"内部访问后光标颜色", 27, CSSPropertyID::kInternalVisitedCaretColor},
    {u8"内部访问后背景颜色", 27, CSSPropertyID::kInternalVisitedBackgroundColor},
    {u8"内部访问后轮廓颜色", 27, CSSPropertyID::kInternalVisitedOutlineColor},
    {u8"滚动内联结束内边距", 27, CSSPropertyID::kScrollPaddingInlineEnd},
    {u8"滚动内联起始内边距", 27, CSSPropertyID::kScrollPaddingInlineStart},
    {u8"滚动块级结束内边距", 27, CSSPropertyID::kScrollPaddingBlockEnd},
    {u8"滚动块级起始内边距", 27, CSSPropertyID::kScrollPaddingBlockStart},
    {u8"内部访问后上边框颜色", 30, CSSPropertyID::kInternalVisitedBorderTopColor},
    {u8"内部访问后下边框颜色", 30, CSSPropertyID::kInternalVisitedBorderBottomColor},
    {u8"内部访问后列规则颜色", 30, CSSPropertyID::kInternalVisitedColumnRuleColor},
    {u8"内部访问后右边框颜色", 30, CSSPropertyID::kInternalVisitedBorderRightColor},
    {u8"内部访问后左边框颜色", 30, CSSPropertyID::kInternalVisitedBorderLeftColor},
    {u8"内部访问后文本填充颜色", 33, CSSPropertyID::kInternalVisitedTextFillColor},
    {u8"内部访问后文本强调颜色", 33, CSSPropertyID::kInternalVisitedTextEmphasisColor},
    {u8"内部访问后文本描边颜色", 33, CSSPropertyID::kInternalVisitedTextStrokeColor},
    {u8"内部访问后文本装饰颜色", 33, CSSPropertyID::kInternalVisitedTextDecorationColor},
    {u8"内部访问后内联结束边框颜色", 39, CSSPropertyID::kInternalVisitedBorderInlineEndColor},
    {u8"内部访问后内联起始边框颜色", 39, CSSPropertyID::kInternalVisitedBorderInlineStartColor},
    {u8"内部访问后块级结束边框颜色", 39, CSSPropertyID::kInternalVisitedBorderBlockEndColor},
    {u8"内部访问后块级起始边框颜色", 39, CSSPropertyID::kInternalVisitedBorderBlockStartColor},
  };
  if (utf8_len >= 6 && static_cast<unsigned char>(buffer[0]) >= 0xE4) {
    for (const auto& e : kZhCssPropertyTable) {
      if (e.zh_len != utf8_len) continue;
      if (!memcmp(buffer, e.zh, e.zh_len)) {
        return ExposedProperty(e.id, execution_context, mode);
      }
    }
  }
  return CSSPropertyID::kInvalid;
}

template <typename CharacterType>
static CSSPropertyID UnresolvedCSSPropertyID(
    const ExecutionContext* execution_context,
    base::span<const CharacterType> property_name,
    CSSParserMode mode) {
  unsigned length = static_cast<unsigned>(property_name.size());
  if (length == 0) {
    return CSSPropertyID::kInvalid;
  }
  if (length >= 3 && property_name[0] == '-' &&
      UNSAFE_BUFFERS(property_name[1]) == '-') {
    return CSSPropertyID::kVariable;
  }
  if (length > kMaxCSSPropertyNameLength) {
    return CSSPropertyID::kInvalid;
  }

  // Chinese CSS property names are resolved first, because the ASCII-only
  // QuasiLowercaseIntoBuffer() below rejects code units >= 0x80.
  char buffer[kMaxCSSPropertyNameLength * 3 + 1];
  unsigned utf8_len = EncodeLowercaseUTF8(property_name, buffer);
  if (!utf8_len) {
    return CSSPropertyID::kInvalid;  // Contains a null character.
  }
  if (CSSPropertyID chinese_id = ChineseCSSPropertyID(
          execution_context, buffer, utf8_len, mode);
      chinese_id != CSSPropertyID::kInvalid) {
    return chinese_id;
  }
  if (utf8_len != length) {
    // Not a Chinese property name, and it contains non-ASCII characters, so
    // it can never be a standard property name.
    return CSSPropertyID::kInvalid;
  }

  const char* name = buffer;
  const Property* hash_table_entry = FindProperty(name, length);
#if DCHECK_IS_ON()
  // Verify that we get the same answer with standard lowercasing.
  for (unsigned i = 0; i < length; ++i) {
    UNSAFE_BUFFERS(buffer[i] = ToAsciiLower(property_name[i]));
  }
  DCHECK_EQ(hash_table_entry, FindProperty(buffer, length));
#endif
  if (!hash_table_entry) {
    return CSSPropertyID::kInvalid;
  }

  int id_and_exposed_bit = hash_table_entry->id_and_exposed_bit;
  if (id_and_exposed_bit & kNotKnownExposedPropertyBit) {
    // The property is behind a runtime flag, so we need to go ahead
    // and actually do the resolution to see if that flag is on or not.
    // This should happen only occasionally.
    CSSPropertyID property_id = static_cast<CSSPropertyID>(
        id_and_exposed_bit & ~kNotKnownExposedPropertyBit);
    return ExposedProperty(property_id, execution_context, mode);
  } else {
    CSSPropertyID property_id = static_cast<CSSPropertyID>(id_and_exposed_bit);
    DCHECK_EQ(property_id,
              ExposedProperty(property_id, execution_context, mode));
    return property_id;
  }
}

CSSPropertyID UnresolvedCSSPropertyID(const ExecutionContext* execution_context,
                                      StringView string,
                                      CSSParserMode mode) {
  return VisitCharacters(string, [&](auto chars) {
    return UnresolvedCSSPropertyID(execution_context, chars, mode);
  });
}

// Look up a Chinese CSS value keyword (encoded as UTF-8 in |buffer| by
// EncodeLowercaseUTF8()) and map it to the corresponding CSSValueID.
// Returns CSSValueID::kInvalid if |buffer| is not a Chinese value keyword.
static CSSValueID ChineseCSSValueKeywordID(const char* buffer,
                                         unsigned utf8_len) {
  static constexpr struct ZhCssValueEntry {
    const char8_t* zh;
    size_t zh_len;
    CSSValueID id;
  } kZhCssValueTable[] = {
    {u8"列", 3, CSSValueID::kColumn},
    {u8"圆", 3, CSSValueID::kRound},
    {u8"小", 3, CSSValueID::kSmall},
    {u8"棕", 3, CSSValueID::kBrown},
    {u8"橙", 3, CSSValueID::kOrange},
    {u8"灰", 3, CSSValueID::kGray},
    {u8"白", 3, CSSValueID::kWhite},
    {u8"粉", 3, CSSValueID::kPink},
    {u8"粗", 3, CSSValueID::kThick},
    {u8"紫", 3, CSSValueID::kPurple},
    {u8"红", 3, CSSValueID::kRed},
    {u8"金", 3, CSSValueID::kGold},
    {u8"银", 3, CSSValueID::kSilver},
    {u8"深红", 6, CSSValueID::kDarkred},
    {u8"浅红", 6, CSSValueID::kLightcoral},
    {u8"暗红", 6, CSSValueID::kMaroon},
    {u8"橙红", 6, CSSValueID::kOrangered},
    {u8"金黄", 6, CSSValueID::kGoldenrod},
    {u8"乳白", 6, CSSValueID::kIvory},
    {u8"米白", 6, CSSValueID::kBeige},
    {u8"天蓝", 6, CSSValueID::kSkyblue},
    {u8"深蓝", 6, CSSValueID::kDarkblue},
    {u8"浅蓝", 6, CSSValueID::kLightblue},
    {u8"藏青", 6, CSSValueID::kNavy},
    {u8"翠绿", 6, CSSValueID::kSpringgreen},
    {u8"深绿", 6, CSSValueID::kDarkgreen},
    {u8"浅绿", 6, CSSValueID::kLightgreen},
    {u8"橄榄", 6, CSSValueID::kOlive},
    {u8"银灰", 6, CSSValueID::kLightgrey},
    {u8"炭灰", 6, CSSValueID::kDimgray},
    {u8"鲜红", 6, CSSValueID::kFirebrick},
    {u8"亮绿", 6, CSSValueID::kLimegreen},
    {u8"浅粉", 6, CSSValueID::kLightpink},
    {u8"淡紫", 6, CSSValueID::kLavender},
    {u8"绛红", 6, CSSValueID::kCrimson},
    {u8"湛蓝", 6, CSSValueID::kAliceblue},
    {u8"细", 3, CSSValueID::kThin},
    {u8"绿", 3, CSSValueID::kGreen},
    {u8"蓝", 3, CSSValueID::kBlue},
    {u8"行", 3, CSSValueID::kRow},
    {u8"青", 3, CSSValueID::kCyan},
    {u8"黄", 3, CSSValueID::kYellow},
    {u8"黑", 3, CSSValueID::kBlack},
    {u8"无", 3, CSSValueID::kNone},
    {u8"上标", 6, CSSValueID::kSuper},
    {u8"下标", 6, CSSValueID::kSub},
    {u8"两端", 6, CSSValueID::kJustify},
    {u8"中央", 6, CSSValueID::kCentral},
    {u8"中等", 6, CSSValueID::kMedium},
    {u8"中间", 6, CSSValueID::kMiddle},
    {u8"交替", 6, CSSValueID::kAlternate},
    {u8"任意", 6, CSSValueID::kAnywhere},
    {u8"优美", 6, CSSValueID::kPretty},
    {u8"侧向", 6, CSSValueID::kSideways},
    {u8"倾斜", 6, CSSValueID::kOblique},
    {u8"全宽", 6, CSSValueID::kFullWidth},
    {u8"全部", 6, CSSValueID::kAll},
    {u8"关闭", 6, CSSValueID::kClose},
    {u8"内容", 6, CSSValueID::kContents},
    {u8"内嵌", 6, CSSValueID::kInset},
    {u8"内联", 6, CSSValueID::kInline},
    {u8"内部", 6, CSSValueID::kInside},
    {u8"凹槽", 6, CSSValueID::kGroove},
    {u8"分离", 6, CSSValueID::kSeparate},
    {u8"切片", 6, CSSValueID::kSlice},
    {u8"初始", 6, CSSValueID::kInitial},
    {u8"别名", 6, CSSValueID::kAlias},
    {u8"前进", 6, CSSValueID::kForwards},
    {u8"包含", 6, CSSValueID::kContain},
    {u8"包裹", 6, CSSValueID::kWrap},
    {u8"十字", 6, CSSValueID::kCrosshair},
    {u8"半径", 6, CSSValueID::kR},
    {u8"模糊", 6, CSSValueID::kBlur},
    {u8"双向", 6, CSSValueID::kBoth},
    {u8"双线", 6, CSSValueID::kDouble},
    {u8"反面", 6, CSSValueID::kVerso},
    {u8"变亮", 6, CSSValueID::kLighten},
    {u8"变暗", 6, CSSValueID::kDarken},
    {u8"可见", 6, CSSValueID::kVisible},
    {u8"右侧", 6, CSSValueID::kRight},
    {u8"后退", 6, CSSValueID::kBackwards},
    {u8"回退", 6, CSSValueID::kFallback},
    {u8"固定", 6, CSSValueID::kFixed},
    {u8"圆形", 6, CSSValueID::kCircle},
    {u8"圆点", 6, CSSValueID::kDisc},
    {u8"块级", 6, CSSValueID::kBlock},
    {u8"基线", 6, CSSValueID::kBaseline},
    {u8"填充", 6, CSSValueID::kFill},
    {u8"复制", 6, CSSValueID::kCopy},
    {u8"外扩", 6, CSSValueID::kOutset},
    {u8"外部", 6, CSSValueID::kOutside},
    {u8"大写", 6, CSSValueID::kUppercase},
    {u8"奇偶", 6, CSSValueID::kEvenodd},
    {u8"始终", 6, CSSValueID::kAlways},
    {u8"安全", 6, CSSValueID::kSafe},
    {u8"实线", 6, CSSValueID::kSolid},
    {u8"宽度", 6, CSSValueID::kWidth},
    {u8"小写", 6, CSSValueID::kLowercase},
    {u8"尺寸", 6, CSSValueID::kSize},
    {u8"局部", 6, CSSValueID::kLocal},
    {u8"居中", 6, CSSValueID::kCenter},
    {u8"嵌入", 6, CSSValueID::kEmbed},
    {u8"左侧", 6, CSSValueID::kLeft},
    {u8"差值", 6, CSSValueID::kDifference},
    {u8"布局", 6, CSSValueID::kLayout},
    {u8"帮助", 6, CSSValueID::kHelp},
    {u8"平滑", 6, CSSValueID::kSmooth},
    {u8"平移", 6, CSSValueID::kTranslate},
    {u8"平端", 6, CSSValueID::kButt},
    {u8"平衡", 6, CSSValueID::kBalance},
    {u8"平面", 6, CSSValueID::kFlat},
    {u8"序数", 6, CSSValueID::kOrdinal},
    {u8"底部", 6, CSSValueID::kBottom},
    {u8"弹性", 6, CSSValueID::kFlex},
    {u8"强光", 6, CSSValueID::kHardLight},
    {u8"手动", 6, CSSValueID::kManual},
    {u8"打开", 6, CSSValueID::kOpen},
    {u8"扩展", 6, CSSValueID::kExpanded},
    {u8"抓取", 6, CSSValueID::kGrab},
    {u8"折叠", 6, CSSValueID::kCollapse},
    {u8"拉伸", 6, CSSValueID::kStretch},
    {u8"指针", 6, CSSValueID::kPointer},
    {u8"排除", 6, CSSValueID::kExclusion},
    {u8"描边", 6, CSSValueID::kStroke},
    {u8"放大", 6, CSSValueID::kZoomIn},
    {u8"文本", 6, CSSValueID::kText},
    {u8"斜体", 6, CSSValueID::kItalic},
    {u8"斜接", 6, CSSValueID::kMiter},
    {u8"斜面", 6, CSSValueID::kBevel},
    {u8"断词", 6, CSSValueID::kBreakWord},
    {u8"方形", 6, CSSValueID::kSquare},
    {u8"旋转", 6, CSSValueID::kRotate},
    {u8"无限", 6, CSSValueID::kInfinite},
    {u8"明度", 6, CSSValueID::kLuminosity},
    {u8"更大", 6, CSSValueID::kXLarge},
    {u8"更小", 6, CSSValueID::kXSmall},
    {u8"更粗", 6, CSSValueID::kBolder},
    {u8"更细", 6, CSSValueID::kLighter},
    {u8"柔光", 6, CSSValueID::kSoftLight},
    {u8"样式", 6, CSSValueID::kStyle},
    {u8"椭圆", 6, CSSValueID::kEllipse},
    {u8"正常", 6, CSSValueID::kNormal},
    {u8"正面", 6, CSSValueID::kRecto},
    {u8"混合", 6, CSSValueID::kMixed},
    {u8"清除", 6, CSSValueID::kClear},
    {u8"滚动", 6, CSSValueID::kScroll},
    {u8"滤色", 6, CSSValueID::kScreen},
    {u8"点线", 6, CSSValueID::kDotted},
    {u8"特大", 6, CSSValueID::kXxLarge},
    {u8"特小", 6, CSSValueID::kXxSmall},
    {u8"直立", 6, CSSValueID::kUpright},
    {u8"相对", 6, CSSValueID::kRelative},
    {u8"矩形", 6, CSSValueID::kRect},
    {u8"矩阵", 6, CSSValueID::kMatrix},
    {u8"禁止", 6, CSSValueID::kNotAllowed},
    {u8"移动", 6, CSSValueID::kMove},
    {u8"稳定", 6, CSSValueID::kStable},
    {u8"等待", 6, CSSValueID::kWait},
    {u8"粗体", 6, CSSValueID::kBold},
    {u8"粘性", 6, CSSValueID::kSticky},
    {u8"紧缩", 6, CSSValueID::kCondensed},
    {u8"线性", 6, CSSValueID::kLinear},
    {u8"终点", 6, CSSValueID::kEnd},
    {u8"绘制", 6, CSSValueID::kPaint},
    {u8"绝对", 6, CSSValueID::kAbsolute},
    {u8"继承", 6, CSSValueID::kInherit},
    {u8"缓入", 6, CSSValueID::kEaseIn},
    {u8"缓出", 6, CSSValueID::kEaseOut},
    {u8"缓动", 6, CSSValueID::kEase},
    {u8"缩小", 6, CSSValueID::kZoomOut},
    {u8"缩放", 6, CSSValueID::kScale},
    {u8"网格", 6, CSSValueID::kGrid},
    {u8"背景", 6, CSSValueID::kBackground},
    {u8"脊线", 6, CSSValueID::kRidge},
    {u8"自动", 6, CSSValueID::kAuto},
    {u8"色相", 6, CSSValueID::kHue},
    {u8"虚线", 6, CSSValueID::kDashed},
    {u8"表格", 6, CSSValueID::kTable},
    {u8"裁剪", 6, CSSValueID::kClip},
    {u8"覆盖", 6, CSSValueID::kCover},
    {u8"起点", 6, CSSValueID::kStart},
    {u8"边框", 6, CSSValueID::kBorder},
    {u8"还原", 6, CSSValueID::kUnset},
    {u8"透明", 6, CSSValueID::kTransparent},
    {u8"透视", 6, CSSValueID::kPerspective},
    {u8"避免", 6, CSSValueID::kAvoid},
    {u8"重复", 6, CSSValueID::kRepeat},
    {u8"闪烁", 6, CSSValueID::kBlink},
    {u8"间隔", 6, CSSValueID::kSpace},
    {u8"隐藏", 6, CSSValueID::kHidden},
    {u8"隔离", 6, CSSValueID::kIsolate},
    {u8"静态", 6, CSSValueID::kStatic},
    {u8"非零", 6, CSSValueID::kNonzero},
    {u8"页面", 6, CSSValueID::kPage},
    {u8"顶部", 6, CSSValueID::kTop},
    {u8"颜色", 6, CSSValueID::kColor},
    {u8"高度", 6, CSSValueID::kHeight},
    {u8"默认", 6, CSSValueID::kDefault},
    {u8"X坐标", 7, CSSValueID::kX},
    {u8"Y坐标", 7, CSSValueID::kY},
    {u8"保持3D", 8, CSSValueID::kPreserve3d},
    {u8"上下文", 9, CSSValueID::kContextual},
    {u8"上划线", 9, CSSValueID::kOverline},
    {u8"下划线", 9, CSSValueID::kUnderline},
    {u8"不安全", 9, CSSValueID::kUnsafe},
    {u8"不换行", 9, CSSValueID::kNowrap},
    {u8"不重复", 9, CSSValueID::kNoRepeat},
    {u8"像素化", 9, CSSValueID::kPixelated},
    {u8"内容盒", 9, CSSValueID::kContentBox},
    {u8"内边距", 9, CSSValueID::kPadding},
    {u8"列表项", 9, CSSValueID::kListItem},
    {u8"列调整", 9, CSSValueID::kColResize},
    {u8"删除线", 9, CSSValueID::kLineThrough},
    {u8"十进制", 9, CSSValueID::kDecimal},
    {u8"半扩展", 9, CSSValueID::kSemiExpanded},
    {u8"半紧缩", 9, CSSValueID::kSemiCondensed},
    {u8"单元格", 9, CSSValueID::kCell},
    {u8"反向列", 9, CSSValueID::kColumnReverse},
    {u8"反向行", 9, CSSValueID::kRowReverse},
    {u8"填充盒", 9, CSSValueID::kFillBox},
    {u8"弹性流", 9, CSSValueID::kFlexFlow},
    {u8"抓取中", 9, CSSValueID::kGrabbing},
    {u8"描边盒", 9, CSSValueID::kStrokeBox},
    {u8"斜分数", 9, CSSValueID::kDiagonalFractions},
    {u8"斜杠零", 9, CSSValueID::kSlashedZero},
    {u8"更扩展", 9, CSSValueID::kExtraExpanded},
    {u8"更紧缩", 9, CSSValueID::kExtraCondensed},
    {u8"最近角", 9, CSSValueID::kClosestCorner},
    {u8"最近边", 9, CSSValueID::kClosestSide},
    {u8"最远角", 9, CSSValueID::kFarthestCorner},
    {u8"最远边", 9, CSSValueID::kFarthestSide},
    {u8"省略号", 9, CSSValueID::kEllipsis},
    {u8"纯文本", 9, CSSValueID::kPlaintext},
    {u8"行调整", 9, CSSValueID::kRowResize},
    {u8"表格行", 9, CSSValueID::kTableRow},
    {u8"覆盖层", 9, CSSValueID::kOverlay},
    {u8"视图盒", 9, CSSValueID::kViewBox},
    {u8"超扩展", 9, CSSValueID::kUltraExpanded},
    {u8"超紧缩", 9, CSSValueID::kUltraCondensed},
    {u8"边框盒", 9, CSSValueID::kBorderBox},
    {u8"进行中", 9, CSSValueID::kProgress},
    {u8"饱和度", 9, CSSValueID::kSaturation},
    {u8"红绿蓝", 9, CSSValueID::kRgb},
    {u8"Webkit弹性", 12, CSSValueID::kWebkitFlex},
    {u8"红绿蓝透", 12, CSSValueID::kRgba},
    {u8"两端对齐", 12, CSSValueID::kSpaceBetween},
    {u8"从右到左", 12, CSSValueID::kRtl},
    {u8"从左到右", 12, CSSValueID::kLtr},
    {u8"任意断行", 12, CSSValueID::kBreakAll},
    {u8"保持整体", 12, CSSValueID::kKeepAll},
    {u8"保留换行", 12, CSSValueID::kPreLine},
    {u8"保留空白", 12, CSSValueID::kPre},
    {u8"全部滚动", 12, CSSValueID::kAllScroll},
    {u8"内联块级", 12, CSSValueID::kInlineBlock},
    {u8"内联尺寸", 12, CSSValueID::kInlineSize},
    {u8"内联弹性", 12, CSSValueID::kInlineFlex},
    {u8"内联网格", 12, CSSValueID::kInlineGrid},
    {u8"内联表格", 12, CSSValueID::kInlineTable},
    {u8"内边距盒", 12, CSSValueID::kPaddingBox},
    {u8"历史连字", 12, CSSValueID::kHistoricalLigatures},
    {u8"双向覆盖", 12, CSSValueID::kBidiOverride},
    {u8"反向交替", 12, CSSValueID::kAlternateReverse},
    {u8"反向换行", 12, CSSValueID::kWrapReverse},
    {u8"可选连字", 12, CSSValueID::kDiscretionaryLigatures},
    {u8"右上调整", 12, CSSValueID::kNeResize},
    {u8"右下调整", 12, CSSValueID::kSeResize},
    {u8"向下调整", 12, CSSValueID::kSResize},
    {u8"向左调整", 12, CSSValueID::kWResize},
    {u8"回退图层", 12, CSSValueID::kRevertLayer},
    {u8"均匀对齐", 12, CSSValueID::kSpaceEvenly},
    {u8"块级尺寸", 12, CSSValueID::kBlockSize},
    {u8"垂直文本", 12, CSSValueID::kVerticalText},
    {u8"垂直调整", 12, CSSValueID::kNResize},
    {u8"垂直重复", 12, CSSValueID::kRepeatY},
    {u8"堆叠分数", 12, CSSValueID::kStackedFractions},
    {u8"字母基线", 12, CSSValueID::kAlphabetic},
    {u8"定位区域", 12, CSSValueID::kPositionArea},
    {u8"小型大写", 12, CSSValueID::kSmallCaps},
    {u8"左上调整", 12, CSSValueID::kNwResize},
    {u8"左下调整", 12, CSSValueID::kSwResize},
    {u8"常见连字", 12, CSSValueID::kCommonLigatures},
    {u8"弹性终点", 12, CSSValueID::kFlexEnd},
    {u8"弹性起点", 12, CSSValueID::kFlexStart},
    {u8"悬挂基线", 12, CSSValueID::kHanging},
    {u8"数学基线", 12, CSSValueID::kMathematical},
    {u8"文本底部", 12, CSSValueID::kTextBottom},
    {u8"文本顶部", 12, CSSValueID::kTextTop},
    {u8"断行空白", 12, CSSValueID::kBreakSpaces},
    {u8"无上下文", 12, CSSValueID::kNoContextual},
    {u8"旧式数字", 12, CSSValueID::kOldstyleNums},
    {u8"最大内容", 12, CSSValueID::kMaxContent},
    {u8"最小内容", 12, CSSValueID::kMinContent},
    {u8"标题大写", 12, CSSValueID::kTitlingCaps},
    {u8"正片叠底", 12, CSSValueID::kMultiply},
    {u8"比例数字", 12, CSSValueID::kProportionalNums},
    {u8"水平调整", 12, CSSValueID::kEResize},
    {u8"水平重复", 12, CSSValueID::kRepeatX},
    {u8"环绕对齐", 12, CSSValueID::kSpaceAround},
    {u8"禁止放置", 12, CSSValueID::kNoDrop},
    {u8"缓入缓出", 12, CSSValueID::kEaseInOut},
    {u8"自身终点", 12, CSSValueID::kSelfEnd},
    {u8"自身起点", 12, CSSValueID::kSelfStart},
    {u8"表意基线", 12, CSSValueID::kIdeographic},
    {u8"表格数字", 12, CSSValueID::kTabularNums},
    {u8"表格标题", 12, CSSValueID::kTableCaption},
    {u8"衬线数字", 12, CSSValueID::kLiningNums},
    {u8"避免分列", 12, CSSValueID::kAvoidColumn},
    {u8"避免分页", 12, CSSValueID::kAvoidPage},
    {u8"阶跃终点", 12, CSSValueID::kStepEnd},
    {u8"阶跃起点", 12, CSSValueID::kStepStart},
    {u8"隔离覆盖", 12, CSSValueID::kIsolateOverride},
    {u8"页面缩放", 12, CSSValueID::kZoom},
    {u8"颜色减淡", 12, CSSValueID::kColorDodge},
    {u8"颜色加深", 12, CSSValueID::kColorBurn},
    {u8"上下文菜单", 15, CSSValueID::kContextMenu},
    {u8"无常见连字", 15, CSSValueID::kNoCommonLigatures},
    {u8"自适应内容", 15, CSSValueID::kFitContent},
    {u8"表格单元格", 15, CSSValueID::kTableCell},
    {u8"首字母大写", 15, CSSValueID::kCapitalize},
    {u8"侧向从右到左", 18, CSSValueID::kSidewaysRl},
    {u8"侧向从左到右", 18, CSSValueID::kSidewaysLr},
    {u8"保留空白换行", 18, CSSValueID::kPreWrap},
    {u8"全部小型大写", 18, CSSValueID::kAllSmallCaps},
    {u8"垂直从右到左", 18, CSSValueID::kVerticalRl},
    {u8"垂直从左到右", 18, CSSValueID::kVerticalLr},
    {u8"垂直双向调整", 18, CSSValueID::kNsResize},
    {u8"对角双向调整", 18, CSSValueID::kNeswResize},
    {u8"水平双向调整", 18, CSSValueID::kEwResize},
    {u8"水平自上而下", 18, CSSValueID::kHorizontalTb},
    {u8"块级格式化上下文", 24, CSSValueID::kFlowRoot},
  };
  if (utf8_len >= 3 && static_cast<unsigned char>(buffer[0]) >= 0xE4) {
    for (const auto& e : kZhCssValueTable) {
      if (e.zh_len != utf8_len) continue;
      if (!memcmp(buffer, e.zh, e.zh_len)) {
        return e.id;
      }
    }
  }
  return CSSValueID::kInvalid;
}
template <typename CharacterType>
static CSSValueID CssValueKeywordID(
    base::span<const CharacterType> value_keyword) {
  unsigned length = static_cast<unsigned>(value_keyword.size());
  // Chinese CSS value keywords are resolved first, because the ASCII-only
  // QuasiLowercaseIntoBuffer() below rejects code units >= 0x80.
  char buffer[kMaxCSSValueKeywordLength * 3 + 1];
  unsigned utf8_len = EncodeLowercaseUTF8(value_keyword, buffer);
  if (!utf8_len) {
    return CSSValueID::kInvalid;  // Contains a null character.
  }
  if (CSSValueID chinese_id = ChineseCSSValueKeywordID(buffer, utf8_len);
      chinese_id != CSSValueID::kInvalid) {
    return chinese_id;
  }
  if (utf8_len != length) {
    // Not a Chinese value keyword, and it contains non-ASCII characters, so
    // it can never be a standard value keyword.
    return CSSValueID::kInvalid;
  }

  const Value* hash_table_entry = FindValue(buffer, length);
#if DCHECK_IS_ON()
  // Verify that we get the same answer with standard lowercasing.
  for (unsigned i = 0; i < length; ++i) {
    UNSAFE_BUFFERS(buffer[i] = ToAsciiLower(value_keyword[i]));
  }
  DCHECK_EQ(hash_table_entry, FindValue(buffer, length));
#endif
  return hash_table_entry ? static_cast<CSSValueID>(hash_table_entry->id)
                          : CSSValueID::kInvalid;
}

CSSValueID CssValueKeywordID(StringView string) {
  unsigned length = string.length();
  if (!length) {
    return CSSValueID::kInvalid;
  }
  if (length > kMaxCSSValueKeywordLength) {
    return CSSValueID::kInvalid;
  }

  return string.Is8Bit() ? CssValueKeywordID(string.Span8())
                         : CssValueKeywordID(string.Span16());
}

const CSSValue* CSSPropertyParser::ConsumeCSSWideKeyword(
    CSSParserTokenStream& stream,
    const CSSParserContext& context,
    bool allow_important_annotation,
    bool& important) {
  CSSParserTokenStream::State savepoint = stream.Save();

  const CSSValue* value =
      css_parsing_utils::ConsumeCSSWideKeyword(stream, context);
  if (!value) {
    // No need to Restore(), we are at the right spot anyway.
    // (We do this instead of relying on CSSParserTokenStream's
    // Restore() optimization, as this path is so hot.)
    return nullptr;
  }

  important = css_parsing_utils::MaybeConsumeImportant(
      stream, allow_important_annotation);
  if (!stream.AtEnd()) {
    stream.Restore(savepoint);
    return nullptr;
  }

  return value;
}

bool CSSPropertyParser::ParseCSSWideKeyword(CSSPropertyID unresolved_property,
                                            bool allow_important_annotation) {
  bool important;
  const CSSValue* value = ConsumeCSSWideKeyword(
      stream_, *context_, allow_important_annotation, important);
  if (!value) {
    return false;
  }

  CSSPropertyID property = ResolveCSSPropertyID(unresolved_property);
  const StylePropertyShorthand& shorthand = shorthandForProperty(property);
  if (!shorthand.length()) {
    if (!CSSProperty::Get(property).IsProperty()) {
      return false;
    }
    AddProperty(property, CSSPropertyID::kInvalid, *value, important,
                IsImplicitProperty::kNotImplicit, *parsed_properties_);
  } else {
    css_parsing_utils::AddExpandedPropertyForValue(property, *value, important,
                                                   *parsed_properties_);
  }
  return true;
}

bool CSSPropertyParser::ParseFontFaceDescriptor(
    CSSPropertyID resolved_property) {
  // TODO(meade): This function should eventually take an AtRuleDescriptorID.
  const AtRuleDescriptorID id =
      CSSPropertyIDAsAtRuleDescriptor(resolved_property);
  if (id == AtRuleDescriptorID::Invalid) {
    return false;
  }

  CSSValue* parsed_value =
      AtRuleDescriptorParser::ParseFontFaceDescriptor(id, stream_, *context_);
  if (!parsed_value) {
    return false;
  }

  AddProperty(resolved_property,
              CSSPropertyID::kInvalid /* current_shorthand */, *parsed_value,
              false /* important */, IsImplicitProperty::kNotImplicit,
              *parsed_properties_);
  return true;
}

}  // namespace blink

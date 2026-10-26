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
  if (utf8_len >= 6 && static_cast<unsigned char>(buffer[0]) >= 0xE4) {
    if (utf8_len == 6) {
      if (!memcmp(buffer, u8"位置", 6)) return ExposedProperty(CSSPropertyID::kPosition, execution_context, mode);
      if (!memcmp(buffer, u8"偏移", 6)) return ExposedProperty(CSSPropertyID::kOffset, execution_context, mode);
      if (!memcmp(buffer, u8"光标", 6)) return ExposedProperty(CSSPropertyID::kCursor, execution_context, mode);
      if (!memcmp(buffer, u8"全部", 6)) return ExposedProperty(CSSPropertyID::kAll, execution_context, mode);
      if (!memcmp(buffer, u8"内容", 6)) return ExposedProperty(CSSPropertyID::kContent, execution_context, mode);
      if (!memcmp(buffer, u8"内嵌", 6)) return ExposedProperty(CSSPropertyID::kInset, execution_context, mode);
      if (!memcmp(buffer, u8"内衬", 6)) return ExposedProperty(CSSPropertyID::kPad, execution_context, mode);
      if (!memcmp(buffer, u8"列宽", 6)) return ExposedProperty(CSSPropertyID::kColumnWidth, execution_context, mode);
      if (!memcmp(buffer, u8"列数", 6)) return ExposedProperty(CSSPropertyID::kColumnCount, execution_context, mode);
      if (!memcmp(buffer, u8"前缀", 6)) return ExposedProperty(CSSPropertyID::kPrefix, execution_context, mode);
      if (!memcmp(buffer, u8"加粗", 6)) return ExposedProperty(CSSPropertyID::kFontWeight, execution_context, mode);
      if (!memcmp(buffer, u8"动画", 6)) return ExposedProperty(CSSPropertyID::kAnimation, execution_context, mode);
      if (!memcmp(buffer, u8"包含", 6)) return ExposedProperty(CSSPropertyID::kContain, execution_context, mode);
      if (!memcmp(buffer, u8"半径", 6)) return ExposedProperty(CSSPropertyID::kR, execution_context, mode);
      if (!memcmp(buffer, u8"变换", 6)) return ExposedProperty(CSSPropertyID::kTransform, execution_context, mode);
      if (!memcmp(buffer, u8"可见", 6)) return ExposedProperty(CSSPropertyID::kVisibility, execution_context, mode);
      if (!memcmp(buffer, u8"右侧", 6)) return ExposedProperty(CSSPropertyID::kRight, execution_context, mode);
      if (!memcmp(buffer, u8"后缀", 6)) return ExposedProperty(CSSPropertyID::kSuffix, execution_context, mode);
      if (!memcmp(buffer, u8"回退", 6)) return ExposedProperty(CSSPropertyID::kFallback, execution_context, mode);
      if (!memcmp(buffer, u8"圆角", 6)) return ExposedProperty(CSSPropertyID::kBorderRadius, execution_context, mode);
      if (!memcmp(buffer, u8"填充", 6)) return ExposedProperty(CSSPropertyID::kFill, execution_context, mode);
      if (!memcmp(buffer, u8"外观", 6)) return ExposedProperty(CSSPropertyID::kAppearance, execution_context, mode);
      if (!memcmp(buffer, u8"多列", 6)) return ExposedProperty(CSSPropertyID::kColumns, execution_context, mode);
      if (!memcmp(buffer, u8"字体", 6)) return ExposedProperty(CSSPropertyID::kFont, execution_context, mode);
      if (!memcmp(buffer, u8"孤行", 6)) return ExposedProperty(CSSPropertyID::kWidows, execution_context, mode);
      if (!memcmp(buffer, u8"容器", 6)) return ExposedProperty(CSSPropertyID::kContainer, execution_context, mode);
      if (!memcmp(buffer, u8"宽度", 6)) return ExposedProperty(CSSPropertyID::kWidth, execution_context, mode);
      if (!memcmp(buffer, u8"寡行", 6)) return ExposedProperty(CSSPropertyID::kOrphans, execution_context, mode);
      if (!memcmp(buffer, u8"对齐", 6)) return ExposedProperty(CSSPropertyID::kAlignItems, execution_context, mode);
      if (!memcmp(buffer, u8"导航", 6)) return ExposedProperty(CSSPropertyID::kNavigation, execution_context, mode);
      if (!memcmp(buffer, u8"尺寸", 6)) return ExposedProperty(CSSPropertyID::kSize, execution_context, mode);
      if (!memcmp(buffer, u8"层级", 6)) return ExposedProperty(CSSPropertyID::kZIndex, execution_context, mode);
      if (!memcmp(buffer, u8"左侧", 6)) return ExposedProperty(CSSPropertyID::kLeft, execution_context, mode);
      if (!memcmp(buffer, u8"平移", 6)) return ExposedProperty(CSSPropertyID::kTranslate, execution_context, mode);
      if (!memcmp(buffer, u8"底部", 6)) return ExposedProperty(CSSPropertyID::kBottom, execution_context, mode);
      if (!memcmp(buffer, u8"引号", 6)) return ExposedProperty(CSSPropertyID::kQuotes, execution_context, mode);
      if (!memcmp(buffer, u8"弹性", 6)) return ExposedProperty(CSSPropertyID::kFlex, execution_context, mode);
      if (!memcmp(buffer, u8"换行", 6)) return ExposedProperty(CSSPropertyID::kLineBreak, execution_context, mode);
      if (!memcmp(buffer, u8"排序", 6)) return ExposedProperty(CSSPropertyID::kOrder, execution_context, mode);
      if (!memcmp(buffer, u8"描边", 6)) return ExposedProperty(CSSPropertyID::kStroke, execution_context, mode);
      if (!memcmp(buffer, u8"斜体", 6)) return ExposedProperty(CSSPropertyID::kFontStyle, execution_context, mode);
      if (!memcmp(buffer, u8"断词", 6)) return ExposedProperty(CSSPropertyID::kWordBreak, execution_context, mode);
      if (!memcmp(buffer, u8"方向", 6)) return ExposedProperty(CSSPropertyID::kDirection, execution_context, mode);
      if (!memcmp(buffer, u8"旋转", 6)) return ExposedProperty(CSSPropertyID::kRotate, execution_context, mode);
      if (!memcmp(buffer, u8"显示", 6)) return ExposedProperty(CSSPropertyID::kDisplay, execution_context, mode);
      if (!memcmp(buffer, u8"朗读", 6)) return ExposedProperty(CSSPropertyID::kSpeak, execution_context, mode);
      if (!memcmp(buffer, u8"来源", 6)) return ExposedProperty(CSSPropertyID::kSrc, execution_context, mode);
      if (!memcmp(buffer, u8"标记", 6)) return ExposedProperty(CSSPropertyID::kMarker, execution_context, mode);
      if (!memcmp(buffer, u8"浮动", 6)) return ExposedProperty(CSSPropertyID::kFloat, execution_context, mode);
      if (!memcmp(buffer, u8"清除", 6)) return ExposedProperty(CSSPropertyID::kClear, execution_context, mode);
      if (!memcmp(buffer, u8"溢出", 6)) return ExposedProperty(CSSPropertyID::kOverflow, execution_context, mode);
      if (!memcmp(buffer, u8"滤镜", 6)) return ExposedProperty(CSSPropertyID::kFilter, execution_context, mode);
      if (!memcmp(buffer, u8"符号", 6)) return ExposedProperty(CSSPropertyID::kSymbols, execution_context, mode);
      if (!memcmp(buffer, u8"类型", 6)) return ExposedProperty(CSSPropertyID::kTypes, execution_context, mode);
      if (!memcmp(buffer, u8"系统", 6)) return ExposedProperty(CSSPropertyID::kSystem, execution_context, mode);
      if (!memcmp(buffer, u8"结果", 6)) return ExposedProperty(CSSPropertyID::kResult, execution_context, mode);
      if (!memcmp(buffer, u8"继承", 6)) return ExposedProperty(CSSPropertyID::kInherits, execution_context, mode);
      if (!memcmp(buffer, u8"缩放", 6)) return ExposedProperty(CSSPropertyID::kScale, execution_context, mode);
      if (!memcmp(buffer, u8"网格", 6)) return ExposedProperty(CSSPropertyID::kGrid, execution_context, mode);
      if (!memcmp(buffer, u8"背景", 6)) return ExposedProperty(CSSPropertyID::kBackground, execution_context, mode);
      if (!memcmp(buffer, u8"范围", 6)) return ExposedProperty(CSSPropertyID::kRange, execution_context, mode);
      if (!memcmp(buffer, u8"行高", 6)) return ExposedProperty(CSSPropertyID::kLineHeight, execution_context, mode);
      if (!memcmp(buffer, u8"裁剪", 6)) return ExposedProperty(CSSPropertyID::kClip, execution_context, mode);
      if (!memcmp(buffer, u8"语法", 6)) return ExposedProperty(CSSPropertyID::kSyntax, execution_context, mode);
      if (!memcmp(buffer, u8"负数", 6)) return ExposedProperty(CSSPropertyID::kNegative, execution_context, mode);
      if (!memcmp(buffer, u8"轮廓", 6)) return ExposedProperty(CSSPropertyID::kOutline, execution_context, mode);
      if (!memcmp(buffer, u8"边框", 6)) return ExposedProperty(CSSPropertyID::kBorder, execution_context, mode);
      if (!memcmp(buffer, u8"边距", 6)) return ExposedProperty(CSSPropertyID::kMargin, execution_context, mode);
      if (!memcmp(buffer, u8"过渡", 6)) return ExposedProperty(CSSPropertyID::kTransition, execution_context, mode);
      if (!memcmp(buffer, u8"透明", 6)) return ExposedProperty(CSSPropertyID::kOpacity, execution_context, mode);
      if (!memcmp(buffer, u8"透视", 6)) return ExposedProperty(CSSPropertyID::kPerspective, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩", 6)) return ExposedProperty(CSSPropertyID::kMask, execution_context, mode);
      if (!memcmp(buffer, u8"间距", 6)) return ExposedProperty(CSSPropertyID::kGap, execution_context, mode);
      if (!memcmp(buffer, u8"隔离", 6)) return ExposedProperty(CSSPropertyID::kIsolation, execution_context, mode);
      if (!memcmp(buffer, u8"页面", 6)) return ExposedProperty(CSSPropertyID::kPage, execution_context, mode);
      if (!memcmp(buffer, u8"顶部", 6)) return ExposedProperty(CSSPropertyID::kTop, execution_context, mode);
      if (!memcmp(buffer, u8"颜色", 6)) return ExposedProperty(CSSPropertyID::kColor, execution_context, mode);
      if (!memcmp(buffer, u8"高度", 6)) return ExposedProperty(CSSPropertyID::kHeight, execution_context, mode);
    } else if (utf8_len == 7) {
      if (!memcmp(buffer, u8"X坐标", 7)) return ExposedProperty(CSSPropertyID::kX, execution_context, mode);
      if (!memcmp(buffer, u8"Y坐标", 7)) return ExposedProperty(CSSPropertyID::kY, execution_context, mode);
      if (!memcmp(buffer, u8"圆心X", 7)) return ExposedProperty(CSSPropertyID::kCx, execution_context, mode);
      if (!memcmp(buffer, u8"圆心Y", 7)) return ExposedProperty(CSSPropertyID::kCy, execution_context, mode);
    } else if (utf8_len == 9) {
      if (!memcmp(buffer, u8"上边框", 9)) return ExposedProperty(CSSPropertyID::kBorderTop, execution_context, mode);
      if (!memcmp(buffer, u8"上边距", 9)) return ExposedProperty(CSSPropertyID::kMarginTop, execution_context, mode);
      if (!memcmp(buffer, u8"下边框", 9)) return ExposedProperty(CSSPropertyID::kBorderBottom, execution_context, mode);
      if (!memcmp(buffer, u8"下边距", 9)) return ExposedProperty(CSSPropertyID::kMarginBottom, execution_context, mode);
      if (!memcmp(buffer, u8"交互性", 9)) return ExposedProperty(CSSPropertyID::kInteractivity, execution_context, mode);
      if (!memcmp(buffer, u8"内边距", 9)) return ExposedProperty(CSSPropertyID::kPadding, execution_context, mode);
      if (!memcmp(buffer, u8"列填充", 9)) return ExposedProperty(CSSPropertyID::kColumnFill, execution_context, mode);
      if (!memcmp(buffer, u8"列开始", 9)) return ExposedProperty(CSSPropertyID::kGridColumnStart, execution_context, mode);
      if (!memcmp(buffer, u8"列结束", 9)) return ExposedProperty(CSSPropertyID::kGridColumnEnd, execution_context, mode);
      if (!memcmp(buffer, u8"列规则", 9)) return ExposedProperty(CSSPropertyID::kColumnRule, execution_context, mode);
      if (!memcmp(buffer, u8"列跨行", 9)) return ExposedProperty(CSSPropertyID::kColumnSpan, execution_context, mode);
      if (!memcmp(buffer, u8"列间距", 9)) return ExposedProperty(CSSPropertyID::kColumnGap, execution_context, mode);
      if (!memcmp(buffer, u8"初始值", 9)) return ExposedProperty(CSSPropertyID::kInitialValue, execution_context, mode);
      if (!memcmp(buffer, u8"变换盒", 9)) return ExposedProperty(CSSPropertyID::kTransformBox, execution_context, mode);
      if (!memcmp(buffer, u8"右边框", 9)) return ExposedProperty(CSSPropertyID::kBorderRight, execution_context, mode);
      if (!memcmp(buffer, u8"右边距", 9)) return ExposedProperty(CSSPropertyID::kMarginRight, execution_context, mode);
      if (!memcmp(buffer, u8"字体族", 9)) return ExposedProperty(CSSPropertyID::kFontFamily, execution_context, mode);
      if (!memcmp(buffer, u8"字间距", 9)) return ExposedProperty(CSSPropertyID::kLetterSpacing, execution_context, mode);
      if (!memcmp(buffer, u8"宽高比", 9)) return ExposedProperty(CSSPropertyID::kAspectRatio, execution_context, mode);
      if (!memcmp(buffer, u8"将变更", 9)) return ExposedProperty(CSSPropertyID::kWillChange, execution_context, mode);
      if (!memcmp(buffer, u8"左边框", 9)) return ExposedProperty(CSSPropertyID::kBorderLeft, execution_context, mode);
      if (!memcmp(buffer, u8"左边距", 9)) return ExposedProperty(CSSPropertyID::kMarginLeft, execution_context, mode);
      if (!memcmp(buffer, u8"弹性流", 9)) return ExposedProperty(CSSPropertyID::kFlexFlow, execution_context, mode);
      if (!memcmp(buffer, u8"强调色", 9)) return ExposedProperty(CSSPropertyID::kAccentColor, execution_context, mode);
      if (!memcmp(buffer, u8"文本框", 9)) return ExposedProperty(CSSPropertyID::kTextBox, execution_context, mode);
      if (!memcmp(buffer, u8"断字符", 9)) return ExposedProperty(CSSPropertyID::kHyphenateCharacter, execution_context, mode);
      if (!memcmp(buffer, u8"模板列", 9)) return ExposedProperty(CSSPropertyID::kGridTemplateColumns, execution_context, mode);
      if (!memcmp(buffer, u8"模板行", 9)) return ExposedProperty(CSSPropertyID::kGridTemplateRows, execution_context, mode);
      if (!memcmp(buffer, u8"盒模型", 9)) return ExposedProperty(CSSPropertyID::kBoxSizing, execution_context, mode);
      if (!memcmp(buffer, u8"网格列", 9)) return ExposedProperty(CSSPropertyID::kGridColumn, execution_context, mode);
      if (!memcmp(buffer, u8"网格行", 9)) return ExposedProperty(CSSPropertyID::kGridRow, execution_context, mode);
      if (!memcmp(buffer, u8"背景色", 9)) return ExposedProperty(CSSPropertyID::kBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, u8"自动列", 9)) return ExposedProperty(CSSPropertyID::kGridAutoColumns, execution_context, mode);
      if (!memcmp(buffer, u8"自动行", 9)) return ExposedProperty(CSSPropertyID::kGridAutoRows, execution_context, mode);
      if (!memcmp(buffer, u8"行开始", 9)) return ExposedProperty(CSSPropertyID::kGridRowStart, execution_context, mode);
      if (!memcmp(buffer, u8"行结束", 9)) return ExposedProperty(CSSPropertyID::kGridRowEnd, execution_context, mode);
      if (!memcmp(buffer, u8"行间距", 9)) return ExposedProperty(CSSPropertyID::kRowGap, execution_context, mode);
      if (!memcmp(buffer, u8"覆盖层", 9)) return ExposedProperty(CSSPropertyID::kOverlay, execution_context, mode);
      if (!memcmp(buffer, u8"词间距", 9)) return ExposedProperty(CSSPropertyID::kWordSpacing, execution_context, mode);
      if (!memcmp(buffer, u8"连字符", 9)) return ExposedProperty(CSSPropertyID::kHyphens, execution_context, mode);
      if (!memcmp(buffer, u8"阅读流", 9)) return ExposedProperty(CSSPropertyID::kReadingFlow, execution_context, mode);
      if (!memcmp(buffer, u8"首字母", 9)) return ExposedProperty(CSSPropertyID::kInitialLetter, execution_context, mode);
    } else if (utf8_len == 10) {
      if (!memcmp(buffer, u8"EPUB断词", 10)) return ExposedProperty(CSSPropertyID::kAliasEpubWordBreak, execution_context, mode);
    } else if (utf8_len == 12) {
      if (!memcmp(buffer, u8"Webkit列宽", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列数", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnCount, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimation, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit变换", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransform, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit圆角", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderRadius, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit外观", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitAppearance, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit多列", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumns, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit对齐", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitAlignItems, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlex, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit换行", 12)) return ExposedProperty(CSSPropertyID::kWebkitLineBreak, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit排序", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitOrder, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit滤镜", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitFilter, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit过渡", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransition, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit透视", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitPerspective, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitMask, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit阴影", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitBoxShadow, execution_context, mode);
      if (!memcmp(buffer, u8"上内边距", 12)) return ExposedProperty(CSSPropertyID::kPaddingTop, execution_context, mode);
      if (!memcmp(buffer, u8"上升覆盖", 12)) return ExposedProperty(CSSPropertyID::kAscentOverride, execution_context, mode);
      if (!memcmp(buffer, u8"下内边距", 12)) return ExposedProperty(CSSPropertyID::kPaddingBottom, execution_context, mode);
      if (!memcmp(buffer, u8"下降覆盖", 12)) return ExposedProperty(CSSPropertyID::kDescentOverride, execution_context, mode);
      if (!memcmp(buffer, u8"中间标记", 12)) return ExposedProperty(CSSPropertyID::kMarkerMid, execution_context, mode);
      if (!memcmp(buffer, u8"主导基线", 12)) return ExposedProperty(CSSPropertyID::kDominantBaseline, execution_context, mode);
      if (!memcmp(buffer, u8"主轴对齐", 12)) return ExposedProperty(CSSPropertyID::kJustifyContent, execution_context, mode);
      if (!memcmp(buffer, u8"之前断页", 12)) return ExposedProperty(CSSPropertyID::kBreakBefore, execution_context, mode);
      if (!memcmp(buffer, u8"之后断页", 12)) return ExposedProperty(CSSPropertyID::kBreakAfter, execution_context, mode);
      if (!memcmp(buffer, u8"书写模式", 12)) return ExposedProperty(CSSPropertyID::kWritingMode, execution_context, mode);
      if (!memcmp(buffer, u8"偏移位置", 12)) return ExposedProperty(CSSPropertyID::kOffsetPosition, execution_context, mode);
      if (!memcmp(buffer, u8"偏移旋转", 12)) return ExposedProperty(CSSPropertyID::kOffsetRotate, execution_context, mode);
      if (!memcmp(buffer, u8"偏移距离", 12)) return ExposedProperty(CSSPropertyID::kOffsetDistance, execution_context, mode);
      if (!memcmp(buffer, u8"偏移路径", 12)) return ExposedProperty(CSSPropertyID::kOffsetPath, execution_context, mode);
      if (!memcmp(buffer, u8"偏移锚点", 12)) return ExposedProperty(CSSPropertyID::kOffsetAnchor, execution_context, mode);
      if (!memcmp(buffer, u8"停止颜色", 12)) return ExposedProperty(CSSPropertyID::kStopColor, execution_context, mode);
      if (!memcmp(buffer, u8"光标动画", 12)) return ExposedProperty(CSSPropertyID::kCaretAnimation, execution_context, mode);
      if (!memcmp(buffer, u8"光标颜色", 12)) return ExposedProperty(CSSPropertyID::kCaretColor, execution_context, mode);
      if (!memcmp(buffer, u8"光照颜色", 12)) return ExposedProperty(CSSPropertyID::kLightingColor, execution_context, mode);
      if (!memcmp(buffer, u8"内容对齐", 12)) return ExposedProperty(CSSPropertyID::kAlignContent, execution_context, mode);
      if (!memcmp(buffer, u8"内联尺寸", 12)) return ExposedProperty(CSSPropertyID::kInlineSize, execution_context, mode);
      if (!memcmp(buffer, u8"内联嵌入", 12)) return ExposedProperty(CSSPropertyID::kInsetInline, execution_context, mode);
      if (!memcmp(buffer, u8"内联溢出", 12)) return ExposedProperty(CSSPropertyID::kOverflowInline, execution_context, mode);
      if (!memcmp(buffer, u8"内联边框", 12)) return ExposedProperty(CSSPropertyID::kBorderInline, execution_context, mode);
      if (!memcmp(buffer, u8"内联边距", 12)) return ExposedProperty(CSSPropertyID::kMarginInline, execution_context, mode);
      if (!memcmp(buffer, u8"内部断页", 12)) return ExposedProperty(CSSPropertyID::kBreakInside, execution_context, mode);
      if (!memcmp(buffer, u8"列表样式", 12)) return ExposedProperty(CSSPropertyID::kListStyle, execution_context, mode);
      if (!memcmp(buffer, u8"动画合成", 12)) return ExposedProperty(CSSPropertyID::kAnimationComposition, execution_context, mode);
      if (!memcmp(buffer, u8"动画名称", 12)) return ExposedProperty(CSSPropertyID::kAnimationName, execution_context, mode);
      if (!memcmp(buffer, u8"动画延迟", 12)) return ExposedProperty(CSSPropertyID::kAnimationDelay, execution_context, mode);
      if (!memcmp(buffer, u8"动画方向", 12)) return ExposedProperty(CSSPropertyID::kAnimationDirection, execution_context, mode);
      if (!memcmp(buffer, u8"动画时长", 12)) return ExposedProperty(CSSPropertyID::kAnimationDuration, execution_context, mode);
      if (!memcmp(buffer, u8"动画范围", 12)) return ExposedProperty(CSSPropertyID::kAnimationRange, execution_context, mode);
      if (!memcmp(buffer, u8"动画触发", 12)) return ExposedProperty(CSSPropertyID::kAnimationTrigger, execution_context, mode);
      if (!memcmp(buffer, u8"单词换行", 12)) return ExposedProperty(CSSPropertyID::kAliasWordWrap, execution_context, mode);
      if (!memcmp(buffer, u8"变换原点", 12)) return ExposedProperty(CSSPropertyID::kTransformOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"变换样式", 12)) return ExposedProperty(CSSPropertyID::kTransformStyle, execution_context, mode);
      if (!memcmp(buffer, u8"盒子阴影", 12)) return ExposedProperty(CSSPropertyID::kBoxShadow, execution_context, mode);
      if (!memcmp(buffer, u8"右上圆角", 12)) return ExposedProperty(CSSPropertyID::kBorderTopRightRadius, execution_context, mode);
      if (!memcmp(buffer, u8"右下圆角", 12)) return ExposedProperty(CSSPropertyID::kBorderBottomRightRadius, execution_context, mode);
      if (!memcmp(buffer, u8"右内边距", 12)) return ExposedProperty(CSSPropertyID::kPaddingRight, execution_context, mode);
      if (!memcmp(buffer, u8"图像方向", 12)) return ExposedProperty(CSSPropertyID::kImageOrientation, execution_context, mode);
      if (!memcmp(buffer, u8"图像渲染", 12)) return ExposedProperty(CSSPropertyID::kImageRendering, execution_context, mode);
      if (!memcmp(buffer, u8"块级尺寸", 12)) return ExposedProperty(CSSPropertyID::kBlockSize, execution_context, mode);
      if (!memcmp(buffer, u8"块级嵌入", 12)) return ExposedProperty(CSSPropertyID::kInsetBlock, execution_context, mode);
      if (!memcmp(buffer, u8"块级溢出", 12)) return ExposedProperty(CSSPropertyID::kOverflowBlock, execution_context, mode);
      if (!memcmp(buffer, u8"块级边框", 12)) return ExposedProperty(CSSPropertyID::kBorderBlock, execution_context, mode);
      if (!memcmp(buffer, u8"块级边距", 12)) return ExposedProperty(CSSPropertyID::kMarginBlock, execution_context, mode);
      if (!memcmp(buffer, u8"垂直对齐", 12)) return ExposedProperty(CSSPropertyID::kVerticalAlign, execution_context, mode);
      if (!memcmp(buffer, u8"垂直溢出", 12)) return ExposedProperty(CSSPropertyID::kOverflowY, execution_context, mode);
      if (!memcmp(buffer, u8"基线偏移", 12)) return ExposedProperty(CSSPropertyID::kBaselineShift, execution_context, mode);
      if (!memcmp(buffer, u8"基线来源", 12)) return ExposedProperty(CSSPropertyID::kBaselineSource, execution_context, mode);
      if (!memcmp(buffer, u8"填充规则", 12)) return ExposedProperty(CSSPropertyID::kFillRule, execution_context, mode);
      if (!memcmp(buffer, u8"外部形状", 12)) return ExposedProperty(CSSPropertyID::kShapeOutside, execution_context, mode);
      if (!memcmp(buffer, u8"大小调整", 12)) return ExposedProperty(CSSPropertyID::kSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, u8"字体变体", 12)) return ExposedProperty(CSSPropertyID::kFontVariant, execution_context, mode);
      if (!memcmp(buffer, u8"字体合成", 12)) return ExposedProperty(CSSPropertyID::kFontSynthesis, execution_context, mode);
      if (!memcmp(buffer, u8"字体大小", 12)) return ExposedProperty(CSSPropertyID::kFontSize, execution_context, mode);
      if (!memcmp(buffer, u8"字体拉伸", 12)) return ExposedProperty(CSSPropertyID::kFontStretch, execution_context, mode);
      if (!memcmp(buffer, u8"字体显示", 12)) return ExposedProperty(CSSPropertyID::kFontDisplay, execution_context, mode);
      if (!memcmp(buffer, u8"字段大小", 12)) return ExposedProperty(CSSPropertyID::kFieldSizing, execution_context, mode);
      if (!memcmp(buffer, u8"字距调整", 12)) return ExposedProperty(CSSPropertyID::kFontKerning, execution_context, mode);
      if (!memcmp(buffer, u8"定位区域", 12)) return ExposedProperty(CSSPropertyID::kPositionArea, execution_context, mode);
      if (!memcmp(buffer, u8"定位尝试", 12)) return ExposedProperty(CSSPropertyID::kPositionTry, execution_context, mode);
      if (!memcmp(buffer, u8"定位锚点", 12)) return ExposedProperty(CSSPropertyID::kPositionAnchor, execution_context, mode);
      if (!memcmp(buffer, u8"容器名称", 12)) return ExposedProperty(CSSPropertyID::kContainerName, execution_context, mode);
      if (!memcmp(buffer, u8"容器类型", 12)) return ExposedProperty(CSSPropertyID::kContainerType, execution_context, mode);
      if (!memcmp(buffer, u8"对象位置", 12)) return ExposedProperty(CSSPropertyID::kObjectPosition, execution_context, mode);
      if (!memcmp(buffer, u8"对象适配", 12)) return ExposedProperty(CSSPropertyID::kObjectFit, execution_context, mode);
      if (!memcmp(buffer, u8"对齐基线", 12)) return ExposedProperty(CSSPropertyID::kAlignmentBaseline, execution_context, mode);
      if (!memcmp(buffer, u8"左上圆角", 12)) return ExposedProperty(CSSPropertyID::kBorderTopLeftRadius, execution_context, mode);
      if (!memcmp(buffer, u8"左下圆角", 12)) return ExposedProperty(CSSPropertyID::kBorderBottomLeftRadius, execution_context, mode);
      if (!memcmp(buffer, u8"左内边距", 12)) return ExposedProperty(CSSPropertyID::kPaddingLeft, execution_context, mode);
      if (!memcmp(buffer, u8"应用区域", 12)) return ExposedProperty(CSSPropertyID::kAppRegion, execution_context, mode);
      if (!memcmp(buffer, u8"弹性基准", 12)) return ExposedProperty(CSSPropertyID::kFlexBasis, execution_context, mode);
      if (!memcmp(buffer, u8"弹性增长", 12)) return ExposedProperty(CSSPropertyID::kFlexGrow, execution_context, mode);
      if (!memcmp(buffer, u8"弹性换行", 12)) return ExposedProperty(CSSPropertyID::kFlexWrap, execution_context, mode);
      if (!memcmp(buffer, u8"弹性收缩", 12)) return ExposedProperty(CSSPropertyID::kFlexShrink, execution_context, mode);
      if (!memcmp(buffer, u8"弹性方向", 12)) return ExposedProperty(CSSPropertyID::kFlexDirection, execution_context, mode);
      if (!memcmp(buffer, u8"形状渲染", 12)) return ExposedProperty(CSSPropertyID::kShapeRendering, execution_context, mode);
      if (!memcmp(buffer, u8"形状边距", 12)) return ExposedProperty(CSSPropertyID::kShapeMargin, execution_context, mode);
      if (!memcmp(buffer, u8"指针事件", 12)) return ExposedProperty(CSSPropertyID::kPointerEvents, execution_context, mode);
      if (!memcmp(buffer, u8"描边宽度", 12)) return ExposedProperty(CSSPropertyID::kStrokeWidth, execution_context, mode);
      if (!memcmp(buffer, u8"描边线端", 12)) return ExposedProperty(CSSPropertyID::kStrokeLinecap, execution_context, mode);
      if (!memcmp(buffer, u8"描边连接", 12)) return ExposedProperty(CSSPropertyID::kStrokeLinejoin, execution_context, mode);
      if (!memcmp(buffer, u8"插值大小", 12)) return ExposedProperty(CSSPropertyID::kInterpolateSize, execution_context, mode);
      if (!memcmp(buffer, u8"放置内容", 12)) return ExposedProperty(CSSPropertyID::kPlaceContent, execution_context, mode);
      if (!memcmp(buffer, u8"放置自身", 12)) return ExposedProperty(CSSPropertyID::kPlaceSelf, execution_context, mode);
      if (!memcmp(buffer, u8"放置项目", 12)) return ExposedProperty(CSSPropertyID::kPlaceItems, execution_context, mode);
      if (!memcmp(buffer, u8"数学样式", 12)) return ExposedProperty(CSSPropertyID::kMathStyle, execution_context, mode);
      if (!memcmp(buffer, u8"数学深度", 12)) return ExposedProperty(CSSPropertyID::kMathDepth, execution_context, mode);
      if (!memcmp(buffer, u8"数学移位", 12)) return ExposedProperty(CSSPropertyID::kMathShift, execution_context, mode);
      if (!memcmp(buffer, u8"文本对齐", 12)) return ExposedProperty(CSSPropertyID::kTextAlign, execution_context, mode);
      if (!memcmp(buffer, u8"文本强调", 12)) return ExposedProperty(CSSPropertyID::kTextEmphasis, execution_context, mode);
      if (!memcmp(buffer, u8"文本换行", 12)) return ExposedProperty(CSSPropertyID::kTextWrap, execution_context, mode);
      if (!memcmp(buffer, u8"文本方向", 12)) return ExposedProperty(CSSPropertyID::kTextOrientation, execution_context, mode);
      if (!memcmp(buffer, u8"文本渲染", 12)) return ExposedProperty(CSSPropertyID::kTextRendering, execution_context, mode);
      if (!memcmp(buffer, u8"文本溢出", 12)) return ExposedProperty(CSSPropertyID::kTextOverflow, execution_context, mode);
      if (!memcmp(buffer, u8"文本缩进", 12)) return ExposedProperty(CSSPropertyID::kTextIndent, execution_context, mode);
      if (!memcmp(buffer, u8"文本装饰", 12)) return ExposedProperty(CSSPropertyID::kTextDecoration, execution_context, mode);
      if (!memcmp(buffer, u8"文本转换", 12)) return ExposedProperty(CSSPropertyID::kTextTransform, execution_context, mode);
      if (!memcmp(buffer, u8"文本锚点", 12)) return ExposedProperty(CSSPropertyID::kTextAnchor, execution_context, mode);
      if (!memcmp(buffer, u8"文本间距", 12)) return ExposedProperty(CSSPropertyID::kTextSpacing, execution_context, mode);
      if (!memcmp(buffer, u8"文本阴影", 12)) return ExposedProperty(CSSPropertyID::kTextShadow, execution_context, mode);
      if (!memcmp(buffer, u8"最大宽度", 12)) return ExposedProperty(CSSPropertyID::kMaxWidth, execution_context, mode);
      if (!memcmp(buffer, u8"最大高度", 12)) return ExposedProperty(CSSPropertyID::kMaxHeight, execution_context, mode);
      if (!memcmp(buffer, u8"最小宽度", 12)) return ExposedProperty(CSSPropertyID::kMinWidth, execution_context, mode);
      if (!memcmp(buffer, u8"最小高度", 12)) return ExposedProperty(CSSPropertyID::kMinHeight, execution_context, mode);
      if (!memcmp(buffer, u8"朗读方式", 12)) return ExposedProperty(CSSPropertyID::kSpeakAs, execution_context, mode);
      if (!memcmp(buffer, u8"末端标记", 12)) return ExposedProperty(CSSPropertyID::kMarkerEnd, execution_context, mode);
      if (!memcmp(buffer, u8"末行对齐", 12)) return ExposedProperty(CSSPropertyID::kTextAlignLast, execution_context, mode);
      if (!memcmp(buffer, u8"标题位置", 12)) return ExposedProperty(CSSPropertyID::kCaptionSide, execution_context, mode);
      if (!memcmp(buffer, u8"模板区域", 12)) return ExposedProperty(CSSPropertyID::kGridTemplateAreas, execution_context, mode);
      if (!memcmp(buffer, u8"横向半径", 12)) return ExposedProperty(CSSPropertyID::kRx, execution_context, mode);
      if (!memcmp(buffer, u8"水平溢出", 12)) return ExposedProperty(CSSPropertyID::kOverflowX, execution_context, mode);
      if (!memcmp(buffer, u8"泛光颜色", 12)) return ExposedProperty(CSSPropertyID::kFloodColor, execution_context, mode);
      if (!memcmp(buffer, u8"注音位置", 12)) return ExposedProperty(CSSPropertyID::kRubyPosition, execution_context, mode);
      if (!memcmp(buffer, u8"注音对齐", 12)) return ExposedProperty(CSSPropertyID::kRubyAlign, execution_context, mode);
      if (!memcmp(buffer, u8"混合模式", 12)) return ExposedProperty(CSSPropertyID::kMixBlendMode, execution_context, mode);
      if (!memcmp(buffer, u8"溢出换行", 12)) return ExposedProperty(CSSPropertyID::kOverflowWrap, execution_context, mode);
      if (!memcmp(buffer, u8"溢出锚定", 12)) return ExposedProperty(CSSPropertyID::kOverflowAnchor, execution_context, mode);
      if (!memcmp(buffer, u8"滚动条槽", 12)) return ExposedProperty(CSSPropertyID::kScrollbarGutter, execution_context, mode);
      if (!memcmp(buffer, u8"滚动行为", 12)) return ExposedProperty(CSSPropertyID::kScrollBehavior, execution_context, mode);
      if (!memcmp(buffer, u8"滚动起点", 12)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, u8"滚动边距", 12)) return ExposedProperty(CSSPropertyID::kScrollMargin, execution_context, mode);
      if (!memcmp(buffer, u8"用户选择", 12)) return ExposedProperty(CSSPropertyID::kUserSelect, execution_context, mode);
      if (!memcmp(buffer, u8"矢量效果", 12)) return ExposedProperty(CSSPropertyID::kVectorEffect, execution_context, mode);
      if (!memcmp(buffer, u8"空单元格", 12)) return ExposedProperty(CSSPropertyID::kEmptyCells, execution_context, mode);
      if (!memcmp(buffer, u8"空白处理", 12)) return ExposedProperty(CSSPropertyID::kWhiteSpace, execution_context, mode);
      if (!memcmp(buffer, u8"空白折叠", 12)) return ExposedProperty(CSSPropertyID::kWhiteSpaceCollapse, execution_context, mode);
      if (!memcmp(buffer, u8"累加符号", 12)) return ExposedProperty(CSSPropertyID::kAdditiveSymbols, execution_context, mode);
      if (!memcmp(buffer, u8"纵向半径", 12)) return ExposedProperty(CSSPropertyID::kRy, execution_context, mode);
      if (!memcmp(buffer, u8"绘制顺序", 12)) return ExposedProperty(CSSPropertyID::kPaintOrder, execution_context, mode);
      if (!memcmp(buffer, u8"缓冲渲染", 12)) return ExposedProperty(CSSPropertyID::kBufferedRendering, execution_context, mode);
      if (!memcmp(buffer, u8"网格区域", 12)) return ExposedProperty(CSSPropertyID::kGridArea, execution_context, mode);
      if (!memcmp(buffer, u8"网格模板", 12)) return ExposedProperty(CSSPropertyID::kGridTemplate, execution_context, mode);
      if (!memcmp(buffer, u8"网格间距", 12)) return ExposedProperty(CSSPropertyID::kAliasGridGap, execution_context, mode);
      if (!memcmp(buffer, u8"背景位置", 12)) return ExposedProperty(CSSPropertyID::kBackgroundPosition, execution_context, mode);
      if (!memcmp(buffer, u8"背景原点", 12)) return ExposedProperty(CSSPropertyID::kBackgroundOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"背景图像", 12)) return ExposedProperty(CSSPropertyID::kBackgroundImage, execution_context, mode);
      if (!memcmp(buffer, u8"背景大小", 12)) return ExposedProperty(CSSPropertyID::kBackgroundSize, execution_context, mode);
      if (!memcmp(buffer, u8"背景滤镜", 12)) return ExposedProperty(CSSPropertyID::kBackdropFilter, execution_context, mode);
      if (!memcmp(buffer, u8"背景裁剪", 12)) return ExposedProperty(CSSPropertyID::kBackgroundClip, execution_context, mode);
      if (!memcmp(buffer, u8"背景重复", 12)) return ExposedProperty(CSSPropertyID::kBackgroundRepeat, execution_context, mode);
      if (!memcmp(buffer, u8"背景附着", 12)) return ExposedProperty(CSSPropertyID::kBackgroundAttachment, execution_context, mode);
      if (!memcmp(buffer, u8"自动排列", 12)) return ExposedProperty(CSSPropertyID::kGridAutoFlow, execution_context, mode);
      if (!memcmp(buffer, u8"行数限制", 12)) return ExposedProperty(CSSPropertyID::kLineClamp, execution_context, mode);
      if (!memcmp(buffer, u8"行距覆盖", 12)) return ExposedProperty(CSSPropertyID::kLineGapOverride, execution_context, mode);
      if (!memcmp(buffer, u8"表格布局", 12)) return ExposedProperty(CSSPropertyID::kTableLayout, execution_context, mode);
      if (!memcmp(buffer, u8"裁剪规则", 12)) return ExposedProperty(CSSPropertyID::kClipRule, execution_context, mode);
      if (!memcmp(buffer, u8"裁剪路径", 12)) return ExposedProperty(CSSPropertyID::kClipPath, execution_context, mode);
      if (!memcmp(buffer, u8"覆盖颜色", 12)) return ExposedProperty(CSSPropertyID::kOverrideColors, execution_context, mode);
      if (!memcmp(buffer, u8"触摸动作", 12)) return ExposedProperty(CSSPropertyID::kTouchAction, execution_context, mode);
      if (!memcmp(buffer, u8"调整大小", 12)) return ExposedProperty(CSSPropertyID::kResize, execution_context, mode);
      if (!memcmp(buffer, u8"起始标记", 12)) return ExposedProperty(CSSPropertyID::kMarkerStart, execution_context, mode);
      if (!memcmp(buffer, u8"路径数据", 12)) return ExposedProperty(CSSPropertyID::kD, execution_context, mode);
      if (!memcmp(buffer, u8"轮廓偏移", 12)) return ExposedProperty(CSSPropertyID::kOutlineOffset, execution_context, mode);
      if (!memcmp(buffer, u8"轮廓宽度", 12)) return ExposedProperty(CSSPropertyID::kOutlineWidth, execution_context, mode);
      if (!memcmp(buffer, u8"轮廓样式", 12)) return ExposedProperty(CSSPropertyID::kOutlineStyle, execution_context, mode);
      if (!memcmp(buffer, u8"轮廓颜色", 12)) return ExposedProperty(CSSPropertyID::kOutlineColor, execution_context, mode);
      if (!memcmp(buffer, u8"边框合并", 12)) return ExposedProperty(CSSPropertyID::kBorderCollapse, execution_context, mode);
      if (!memcmp(buffer, u8"边框图像", 12)) return ExposedProperty(CSSPropertyID::kBorderImage, execution_context, mode);
      if (!memcmp(buffer, u8"边框宽度", 12)) return ExposedProperty(CSSPropertyID::kBorderWidth, execution_context, mode);
      if (!memcmp(buffer, u8"边框样式", 12)) return ExposedProperty(CSSPropertyID::kBorderStyle, execution_context, mode);
      if (!memcmp(buffer, u8"边框间距", 12)) return ExposedProperty(CSSPropertyID::kBorderSpacing, execution_context, mode);
      if (!memcmp(buffer, u8"边框颜色", 12)) return ExposedProperty(CSSPropertyID::kBorderColor, execution_context, mode);
      if (!memcmp(buffer, u8"过渡属性", 12)) return ExposedProperty(CSSPropertyID::kTransitionProperty, execution_context, mode);
      if (!memcmp(buffer, u8"过渡延迟", 12)) return ExposedProperty(CSSPropertyID::kTransitionDelay, execution_context, mode);
      if (!memcmp(buffer, u8"过渡时长", 12)) return ExposedProperty(CSSPropertyID::kTransitionDuration, execution_context, mode);
      if (!memcmp(buffer, u8"过渡行为", 12)) return ExposedProperty(CSSPropertyID::kTransitionBehavior, execution_context, mode);
      if (!memcmp(buffer, u8"透视原点", 12)) return ExposedProperty(CSSPropertyID::kPerspectiveOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩位置", 12)) return ExposedProperty(CSSPropertyID::kMaskPosition, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩原点", 12)) return ExposedProperty(CSSPropertyID::kMaskOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩合成", 12)) return ExposedProperty(CSSPropertyID::kMaskComposite, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩图像", 12)) return ExposedProperty(CSSPropertyID::kMaskImage, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩大小", 12)) return ExposedProperty(CSSPropertyID::kMaskSize, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩模式", 12)) return ExposedProperty(CSSPropertyID::kMaskMode, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩类型", 12)) return ExposedProperty(CSSPropertyID::kMaskType, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩裁剪", 12)) return ExposedProperty(CSSPropertyID::kMaskClip, execution_context, mode);
      if (!memcmp(buffer, u8"遮罩重复", 12)) return ExposedProperty(CSSPropertyID::kMaskRepeat, execution_context, mode);
      if (!memcmp(buffer, u8"配色方案", 12)) return ExposedProperty(CSSPropertyID::kColorScheme, execution_context, mode);
      if (!memcmp(buffer, u8"锚点名称", 12)) return ExposedProperty(CSSPropertyID::kAnchorName, execution_context, mode);
      if (!memcmp(buffer, u8"页内断页", 12)) return ExposedProperty(CSSPropertyID::kPageBreakInside, execution_context, mode);
      if (!memcmp(buffer, u8"页前断页", 12)) return ExposedProperty(CSSPropertyID::kPageBreakBefore, execution_context, mode);
      if (!memcmp(buffer, u8"页后断页", 12)) return ExposedProperty(CSSPropertyID::kPageBreakAfter, execution_context, mode);
      if (!memcmp(buffer, u8"页面方向", 12)) return ExposedProperty(CSSPropertyID::kPageOrientation, execution_context, mode);
      if (!memcmp(buffer, u8"页面缩放", 12)) return ExposedProperty(CSSPropertyID::kZoom, execution_context, mode);
      if (!memcmp(buffer, u8"项目对齐", 12)) return ExposedProperty(CSSPropertyID::kJustifyItems, execution_context, mode);
      if (!memcmp(buffer, u8"颜色插值", 12)) return ExposedProperty(CSSPropertyID::kColorInterpolation, execution_context, mode);
      if (!memcmp(buffer, u8"颜色渲染", 12)) return ExposedProperty(CSSPropertyID::kColorRendering, execution_context, mode);
    } else if (utf8_len == 13) {
      if (!memcmp(buffer, u8"Unicode双向", 13)) return ExposedProperty(CSSPropertyID::kUnicodeBidi, execution_context, mode);
      if (!memcmp(buffer, u8"Unicode范围", 13)) return ExposedProperty(CSSPropertyID::kUnicodeRange, execution_context, mode);
    } else if (utf8_len == 15) {
      if (!memcmp(buffer, u8"Webkit列规则", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRule, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列跨行", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnSpan, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列间距", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnGap, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性流", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexFlow, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit断字符", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitHyphenateCharacter, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit方方向", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxDirection, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit方朝向", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxOrient, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒分布", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxPack, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒反射", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxReflect, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒对齐", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxAlign, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒弹性", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxFlex, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒模型", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitBoxSizing, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit透明度", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitOpacity, execution_context, mode);
      if (!memcmp(buffer, u8"上边框宽度", 15)) return ExposedProperty(CSSPropertyID::kBorderTopWidth, execution_context, mode);
      if (!memcmp(buffer, u8"上边框样式", 15)) return ExposedProperty(CSSPropertyID::kBorderTopStyle, execution_context, mode);
      if (!memcmp(buffer, u8"上边框颜色", 15)) return ExposedProperty(CSSPropertyID::kBorderTopColor, execution_context, mode);
      if (!memcmp(buffer, u8"下边框宽度", 15)) return ExposedProperty(CSSPropertyID::kBorderBottomWidth, execution_context, mode);
      if (!memcmp(buffer, u8"下边框样式", 15)) return ExposedProperty(CSSPropertyID::kBorderBottomStyle, execution_context, mode);
      if (!memcmp(buffer, u8"下边框颜色", 15)) return ExposedProperty(CSSPropertyID::kBorderBottomColor, execution_context, mode);
      if (!memcmp(buffer, u8"停止透明度", 15)) return ExposedProperty(CSSPropertyID::kStopOpacity, execution_context, mode);
      if (!memcmp(buffer, u8"内容可见性", 15)) return ExposedProperty(CSSPropertyID::kContentVisibility, execution_context, mode);
      if (!memcmp(buffer, u8"内联内边距", 15)) return ExposedProperty(CSSPropertyID::kPaddingInline, execution_context, mode);
      if (!memcmp(buffer, u8"内部空行高", 15)) return ExposedProperty(CSSPropertyID::kInternalEmptyLineHeight, execution_context, mode);
      if (!memcmp(buffer, u8"列规则宽度", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleWidth, execution_context, mode);
      if (!memcmp(buffer, u8"列规则断页", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleBreak, execution_context, mode);
      if (!memcmp(buffer, u8"列规则样式", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleStyle, execution_context, mode);
      if (!memcmp(buffer, u8"列规则颜色", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleColor, execution_context, mode);
      if (!memcmp(buffer, u8"制表符大小", 15)) return ExposedProperty(CSSPropertyID::kTabSize, execution_context, mode);
      if (!memcmp(buffer, u8"动画时间线", 15)) return ExposedProperty(CSSPropertyID::kAnimationTimeline, execution_context, mode);
      if (!memcmp(buffer, u8"右边框宽度", 15)) return ExposedProperty(CSSPropertyID::kBorderRightWidth, execution_context, mode);
      if (!memcmp(buffer, u8"右边框样式", 15)) return ExposedProperty(CSSPropertyID::kBorderRightStyle, execution_context, mode);
      if (!memcmp(buffer, u8"右边框颜色", 15)) return ExposedProperty(CSSPropertyID::kBorderRightColor, execution_context, mode);
      if (!memcmp(buffer, u8"块级内边距", 15)) return ExposedProperty(CSSPropertyID::kPaddingBlock, execution_context, mode);
      if (!memcmp(buffer, u8"基础调色板", 15)) return ExposedProperty(CSSPropertyID::kBasePalette, execution_context, mode);
      if (!memcmp(buffer, u8"填充透明度", 15)) return ExposedProperty(CSSPropertyID::kFillOpacity, execution_context, mode);
      if (!memcmp(buffer, u8"字体调色板", 15)) return ExposedProperty(CSSPropertyID::kFontPalette, execution_context, mode);
      if (!memcmp(buffer, u8"定位可见性", 15)) return ExposedProperty(CSSPropertyID::kPositionVisibility, execution_context, mode);
      if (!memcmp(buffer, u8"对象视图框", 15)) return ExposedProperty(CSSPropertyID::kObjectViewBox, execution_context, mode);
      if (!memcmp(buffer, u8"左边框宽度", 15)) return ExposedProperty(CSSPropertyID::kBorderLeftWidth, execution_context, mode);
      if (!memcmp(buffer, u8"左边框样式", 15)) return ExposedProperty(CSSPropertyID::kBorderLeftStyle, execution_context, mode);
      if (!memcmp(buffer, u8"左边框颜色", 15)) return ExposedProperty(CSSPropertyID::kBorderLeftColor, execution_context, mode);
      if (!memcmp(buffer, u8"描边透明度", 15)) return ExposedProperty(CSSPropertyID::kStrokeOpacity, execution_context, mode);
      if (!memcmp(buffer, u8"文本框修剪", 15)) return ExposedProperty(CSSPropertyID::kTextBoxTrim, execution_context, mode);
      if (!memcmp(buffer, u8"文本框边缘", 15)) return ExposedProperty(CSSPropertyID::kTextBoxEdge, execution_context, mode);
      if (!memcmp(buffer, u8"文本装饰线", 15)) return ExposedProperty(CSSPropertyID::kTextDecorationLine, execution_context, mode);
      if (!memcmp(buffer, u8"泛光透明度", 15)) return ExposedProperty(CSSPropertyID::kFloodOpacity, execution_context, mode);
      if (!memcmp(buffer, u8"滚动上边距", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginTop, execution_context, mode);
      if (!memcmp(buffer, u8"滚动下边距", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginBottom, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内边距", 15)) return ExposedProperty(CSSPropertyID::kScrollPadding, execution_context, mode);
      if (!memcmp(buffer, u8"滚动右边距", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginRight, execution_context, mode);
      if (!memcmp(buffer, u8"滚动左边距", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginLeft, execution_context, mode);
      if (!memcmp(buffer, u8"滚动时间线", 15)) return ExposedProperty(CSSPropertyID::kScrollTimeline, execution_context, mode);
      if (!memcmp(buffer, u8"滚动条宽度", 15)) return ExposedProperty(CSSPropertyID::kScrollbarWidth, execution_context, mode);
      if (!memcmp(buffer, u8"滚动条颜色", 15)) return ExposedProperty(CSSPropertyID::kScrollbarColor, execution_context, mode);
      if (!memcmp(buffer, u8"滚动标记组", 15)) return ExposedProperty(CSSPropertyID::kScrollMarkerGroup, execution_context, mode);
      if (!memcmp(buffer, u8"网格列间距", 15)) return ExposedProperty(CSSPropertyID::kAliasGridColumnGap, execution_context, mode);
      if (!memcmp(buffer, u8"网格行间距", 15)) return ExposedProperty(CSSPropertyID::kAliasGridRowGap, execution_context, mode);
      if (!memcmp(buffer, u8"背面可见性", 15)) return ExposedProperty(CSSPropertyID::kBackfaceVisibility, execution_context, mode);
      if (!memcmp(buffer, u8"视图时间线", 15)) return ExposedProperty(CSSPropertyID::kViewTimeline, execution_context, mode);
      if (!memcmp(buffer, u8"视图过渡类", 15)) return ExposedProperty(CSSPropertyID::kViewTransitionClass, execution_context, mode);
      if (!memcmp(buffer, u8"视图过渡组", 15)) return ExposedProperty(CSSPropertyID::kViewTransitionGroup, execution_context, mode);
      if (!memcmp(buffer, u8"计数器设置", 15)) return ExposedProperty(CSSPropertyID::kCounterSet, execution_context, mode);
      if (!memcmp(buffer, u8"计数器递增", 15)) return ExposedProperty(CSSPropertyID::kCounterIncrement, execution_context, mode);
      if (!memcmp(buffer, u8"计数器重置", 15)) return ExposedProperty(CSSPropertyID::kCounterReset, execution_context, mode);
      if (!memcmp(buffer, u8"边框图像源", 15)) return ExposedProperty(CSSPropertyID::kBorderImageSource, execution_context, mode);
      if (!memcmp(buffer, u8"锚点作用域", 15)) return ExposedProperty(CSSPropertyID::kAnchorScope, execution_context, mode);
    } else if (utf8_len == 16) {
      if (!memcmp(buffer, u8"EPUB书写模式", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubWritingMode, execution_context, mode);
      if (!memcmp(buffer, u8"EPUB文本合并", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextCombine, execution_context, mode);
      if (!memcmp(buffer, u8"EPUB文本强调", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextEmphasis, execution_context, mode);
      if (!memcmp(buffer, u8"EPUB文本方向", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextOrientation, execution_context, mode);
      if (!memcmp(buffer, u8"EPUB文本转换", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextTransform, execution_context, mode);
      if (!memcmp(buffer, u8"EPUB标题位置", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubCaptionSide, execution_context, mode);
    } else if (utf8_len == 18) {
      if (!memcmp(buffer, u8"Webkit主轴对齐", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitJustifyContent, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit书写模式", 18)) return ExposedProperty(CSSPropertyID::kWebkitWritingMode, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit内容对齐", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAlignContent, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列内断页", 18)) return ExposedProperty(CSSPropertyID::kWebkitColumnBreakInside, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列前断页", 18)) return ExposedProperty(CSSPropertyID::kWebkitColumnBreakBefore, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列后断页", 18)) return ExposedProperty(CSSPropertyID::kWebkitColumnBreakAfter, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit前置边框", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBefore, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit前置边距", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginBefore, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画名称", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationName, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画延迟", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationDelay, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画方向", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationDirection, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画时长", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationDuration, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit区域设置", 18)) return ExposedProperty(CSSPropertyID::kWebkitLocale, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit变换原点", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransformOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit变换样式", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransformStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit右上圆角", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderTopRightRadius, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit右下圆角", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBottomRightRadius, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit后置边框", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfter, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit后置边距", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginAfter, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit外部形状", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitShapeOutside, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit字体平滑", 18)) return ExposedProperty(CSSPropertyID::kWebkitFontSmoothing, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit左上圆角", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderTopLeftRadius, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit左下圆角", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBottomLeftRadius, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit应用区域", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAppRegion, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性基准", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexBasis, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性增长", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexGrow, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性换行", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexWrap, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性收缩", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexShrink, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit弹性方向", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexDirection, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit形状边距", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitShapeMargin, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本合并", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextCombine, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本安全", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextSecurity, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本强调", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasis, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本描边", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextStroke, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本方向", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextOrientation, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit注音位置", 18)) return ExposedProperty(CSSPropertyID::kWebkitRubyPosition, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit用户修改", 18)) return ExposedProperty(CSSPropertyID::kWebkitUserModify, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit用户拖拽", 18)) return ExposedProperty(CSSPropertyID::kWebkitUserDrag, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit用户选择", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitUserSelect, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒排序组", 18)) return ExposedProperty(CSSPropertyID::kWebkitBoxOrdinalGroup, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit结束边框", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEnd, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit结束边距", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginEnd, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit背景原点", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackgroundOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit背景大小", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackgroundSize, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit背景裁剪", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackgroundClip, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit自身对齐", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAlignSelf, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit行数限制", 18)) return ExposedProperty(CSSPropertyID::kWebkitLineClamp, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit裁剪路径", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitClipPath, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit起始边框", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStart, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit起始边距", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginStart, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit边框图像", 18)) return ExposedProperty(CSSPropertyID::kWebkitBorderImage, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit过渡属性", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionProperty, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit过渡延迟", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionDelay, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit过渡时长", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionDuration, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit透视原点", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitPerspectiveOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit逻辑宽度", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitLogicalWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit逻辑高度", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitLogicalHeight, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩位置", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskPosition, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩原点", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskOrigin, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩合成", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskComposite, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩图像", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskImage, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩大小", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskSize, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩裁剪", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskClip, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩重复", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskRepeat, execution_context, mode);
      if (!memcmp(buffer, u8"主轴自身对齐", 18)) return ExposedProperty(CSSPropertyID::kJustifySelf, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束嵌入", 18)) return ExposedProperty(CSSPropertyID::kInsetInlineEnd, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束边框", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineEnd, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束边距", 18)) return ExposedProperty(CSSPropertyID::kMarginInlineEnd, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始嵌入", 18)) return ExposedProperty(CSSPropertyID::kInsetInlineStart, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始边框", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineStart, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始边距", 18)) return ExposedProperty(CSSPropertyID::kMarginInlineStart, execution_context, mode);
      if (!memcmp(buffer, u8"内联边框宽度", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineWidth, execution_context, mode);
      if (!memcmp(buffer, u8"内联边框样式", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineStyle, execution_context, mode);
      if (!memcmp(buffer, u8"内联边框颜色", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部强制颜色", 18)) return ExposedProperty(CSSPropertyID::kInternalForcedColor, execution_context, mode);
      if (!memcmp(buffer, u8"列表样式位置", 18)) return ExposedProperty(CSSPropertyID::kListStylePosition, execution_context, mode);
      if (!memcmp(buffer, u8"列表样式图像", 18)) return ExposedProperty(CSSPropertyID::kListStyleImage, execution_context, mode);
      if (!memcmp(buffer, u8"列表样式类型", 18)) return ExposedProperty(CSSPropertyID::kListStyleType, execution_context, mode);
      if (!memcmp(buffer, u8"动态范围限制", 18)) return ExposedProperty(CSSPropertyID::kDynamicRangeLimit, execution_context, mode);
      if (!memcmp(buffer, u8"动画填充模式", 18)) return ExposedProperty(CSSPropertyID::kAnimationFillMode, execution_context, mode);
      if (!memcmp(buffer, u8"动画播放状态", 18)) return ExposedProperty(CSSPropertyID::kAnimationPlayState, execution_context, mode);
      if (!memcmp(buffer, u8"动画时间函数", 18)) return ExposedProperty(CSSPropertyID::kAnimationTimingFunction, execution_context, mode);
      if (!memcmp(buffer, u8"动画范围终点", 18)) return ExposedProperty(CSSPropertyID::kAnimationRangeEnd, execution_context, mode);
      if (!memcmp(buffer, u8"动画范围起点", 18)) return ExposedProperty(CSSPropertyID::kAnimationRangeStart, execution_context, mode);
      if (!memcmp(buffer, u8"动画迭代次数", 18)) return ExposedProperty(CSSPropertyID::kAnimationIterationCount, execution_context, mode);
      if (!memcmp(buffer, u8"固有包含宽度", 18)) return ExposedProperty(CSSPropertyID::kContainIntrinsicWidth, execution_context, mode);
      if (!memcmp(buffer, u8"固有包含尺寸", 18)) return ExposedProperty(CSSPropertyID::kContainIntrinsicSize, execution_context, mode);
      if (!memcmp(buffer, u8"固有包含高度", 18)) return ExposedProperty(CSSPropertyID::kContainIntrinsicHeight, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束嵌入", 18)) return ExposedProperty(CSSPropertyID::kInsetBlockEnd, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束边框", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockEnd, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束边距", 18)) return ExposedProperty(CSSPropertyID::kMarginBlockEnd, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始嵌入", 18)) return ExposedProperty(CSSPropertyID::kInsetBlockStart, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始边框", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockStart, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始边距", 18)) return ExposedProperty(CSSPropertyID::kMarginBlockStart, execution_context, mode);
      if (!memcmp(buffer, u8"块级边框宽度", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockWidth, execution_context, mode);
      if (!memcmp(buffer, u8"块级边框样式", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockStyle, execution_context, mode);
      if (!memcmp(buffer, u8"块级边框颜色", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockColor, execution_context, mode);
      if (!memcmp(buffer, u8"字体东亚变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantEastAsian, execution_context, mode);
      if (!memcmp(buffer, u8"字体位置变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantPosition, execution_context, mode);
      if (!memcmp(buffer, u8"字体光学大小", 18)) return ExposedProperty(CSSPropertyID::kFontOpticalSizing, execution_context, mode);
      if (!memcmp(buffer, u8"字体变化设置", 18)) return ExposedProperty(CSSPropertyID::kFontVariationSettings, execution_context, mode);
      if (!memcmp(buffer, u8"字体合成样式", 18)) return ExposedProperty(CSSPropertyID::kFontSynthesisStyle, execution_context, mode);
      if (!memcmp(buffer, u8"字体合成粗细", 18)) return ExposedProperty(CSSPropertyID::kFontSynthesisWeight, execution_context, mode);
      if (!memcmp(buffer, u8"字体大写变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantCaps, execution_context, mode);
      if (!memcmp(buffer, u8"字体大小调整", 18)) return ExposedProperty(CSSPropertyID::kFontSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, u8"字体数字变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantNumeric, execution_context, mode);
      if (!memcmp(buffer, u8"字体替代变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantAlternates, execution_context, mode);
      if (!memcmp(buffer, u8"字体特性设置", 18)) return ExposedProperty(CSSPropertyID::kFontFeatureSettings, execution_context, mode);
      if (!memcmp(buffer, u8"字体表情变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantEmoji, execution_context, mode);
      if (!memcmp(buffer, u8"字体连字变体", 18)) return ExposedProperty(CSSPropertyID::kFontVariantLigatures, execution_context, mode);
      if (!memcmp(buffer, u8"定位尝试回退", 18)) return ExposedProperty(CSSPropertyID::kPositionTryFallbacks, execution_context, mode);
      if (!memcmp(buffer, u8"定位尝试顺序", 18)) return ExposedProperty(CSSPropertyID::kPositionTryOrder, execution_context, mode);
      if (!memcmp(buffer, u8"强制颜色调整", 18)) return ExposedProperty(CSSPropertyID::kForcedColorAdjust, execution_context, mode);
      if (!memcmp(buffer, u8"形状图像阈值", 18)) return ExposedProperty(CSSPropertyID::kShapeImageThreshold, execution_context, mode);
      if (!memcmp(buffer, u8"描边斜接限制", 18)) return ExposedProperty(CSSPropertyID::kStrokeMiterlimit, execution_context, mode);
      if (!memcmp(buffer, u8"描边虚线偏移", 18)) return ExposedProperty(CSSPropertyID::kStrokeDashoffset, execution_context, mode);
      if (!memcmp(buffer, u8"描边虚线阵列", 18)) return ExposedProperty(CSSPropertyID::kStrokeDasharray, execution_context, mode);
      if (!memcmp(buffer, u8"文本大小调整", 18)) return ExposedProperty(CSSPropertyID::kTextSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, u8"文本强调位置", 18)) return ExposedProperty(CSSPropertyID::kTextEmphasisPosition, execution_context, mode);
      if (!memcmp(buffer, u8"文本强调样式", 18)) return ExposedProperty(CSSPropertyID::kTextEmphasisStyle, execution_context, mode);
      if (!memcmp(buffer, u8"文本强调颜色", 18)) return ExposedProperty(CSSPropertyID::kTextEmphasisColor, execution_context, mode);
      if (!memcmp(buffer, u8"文本换行样式", 18)) return ExposedProperty(CSSPropertyID::kTextWrapStyle, execution_context, mode);
      if (!memcmp(buffer, u8"文本换行模式", 18)) return ExposedProperty(CSSPropertyID::kTextWrapMode, execution_context, mode);
      if (!memcmp(buffer, u8"文本竖排合并", 18)) return ExposedProperty(CSSPropertyID::kTextCombineUpright, execution_context, mode);
      if (!memcmp(buffer, u8"文本自动间距", 18)) return ExposedProperty(CSSPropertyID::kTextAutospace, execution_context, mode);
      if (!memcmp(buffer, u8"文本装饰样式", 18)) return ExposedProperty(CSSPropertyID::kTextDecorationStyle, execution_context, mode);
      if (!memcmp(buffer, u8"文本装饰粗细", 18)) return ExposedProperty(CSSPropertyID::kTextDecorationThickness, execution_context, mode);
      if (!memcmp(buffer, u8"文本装饰颜色", 18)) return ExposedProperty(CSSPropertyID::kTextDecorationColor, execution_context, mode);
      if (!memcmp(buffer, u8"文本间距修剪", 18)) return ExposedProperty(CSSPropertyID::kTextSpacingTrim, execution_context, mode);
      if (!memcmp(buffer, u8"断字符数限制", 18)) return ExposedProperty(CSSPropertyID::kHyphenateLimitChars, execution_context, mode);
      if (!memcmp(buffer, u8"时间线作用域", 18)) return ExposedProperty(CSSPropertyID::kTimelineScope, execution_context, mode);
      if (!memcmp(buffer, u8"最大内联尺寸", 18)) return ExposedProperty(CSSPropertyID::kMaxInlineSize, execution_context, mode);
      if (!memcmp(buffer, u8"最大块级尺寸", 18)) return ExposedProperty(CSSPropertyID::kMaxBlockSize, execution_context, mode);
      if (!memcmp(buffer, u8"最小内联尺寸", 18)) return ExposedProperty(CSSPropertyID::kMinInlineSize, execution_context, mode);
      if (!memcmp(buffer, u8"最小块级尺寸", 18)) return ExposedProperty(CSSPropertyID::kMinBlockSize, execution_context, mode);
      if (!memcmp(buffer, u8"溢出裁剪边距", 18)) return ExposedProperty(CSSPropertyID::kOverflowClipMargin, execution_context, mode);
      if (!memcmp(buffer, u8"滚动上内边距", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingTop, execution_context, mode);
      if (!memcmp(buffer, u8"滚动下内边距", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingBottom, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内联边距", 18)) return ExposedProperty(CSSPropertyID::kScrollMarginInline, execution_context, mode);
      if (!memcmp(buffer, u8"滚动右内边距", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingRight, execution_context, mode);
      if (!memcmp(buffer, u8"滚动吸附停止", 18)) return ExposedProperty(CSSPropertyID::kScrollSnapStop, execution_context, mode);
      if (!memcmp(buffer, u8"滚动吸附对齐", 18)) return ExposedProperty(CSSPropertyID::kScrollSnapAlign, execution_context, mode);
      if (!memcmp(buffer, u8"滚动吸附类型", 18)) return ExposedProperty(CSSPropertyID::kScrollSnapType, execution_context, mode);
      if (!memcmp(buffer, u8"滚动块级边距", 18)) return ExposedProperty(CSSPropertyID::kScrollMarginBlock, execution_context, mode);
      if (!memcmp(buffer, u8"滚动左内边距", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingLeft, execution_context, mode);
      if (!memcmp(buffer, u8"滚动时间线轴", 18)) return ExposedProperty(CSSPropertyID::kScrollTimelineAxis, execution_context, mode);
      if (!memcmp(buffer, u8"滤镜颜色插值", 18)) return ExposedProperty(CSSPropertyID::kColorInterpolationFilters, execution_context, mode);
      if (!memcmp(buffer, u8"盒子装饰断行", 18)) return ExposedProperty(CSSPropertyID::kBoxDecorationBreak, execution_context, mode);
      if (!memcmp(buffer, u8"结束结束圆角", 18)) return ExposedProperty(CSSPropertyID::kBorderEndEndRadius, execution_context, mode);
      if (!memcmp(buffer, u8"结束起始圆角", 18)) return ExposedProperty(CSSPropertyID::kBorderEndStartRadius, execution_context, mode);
      if (!memcmp(buffer, u8"背景垂直位置", 18)) return ExposedProperty(CSSPropertyID::kBackgroundPositionY, execution_context, mode);
      if (!memcmp(buffer, u8"背景水平位置", 18)) return ExposedProperty(CSSPropertyID::kBackgroundPositionX, execution_context, mode);
      if (!memcmp(buffer, u8"背景混合模式", 18)) return ExposedProperty(CSSPropertyID::kBackgroundBlendMode, execution_context, mode);
      if (!memcmp(buffer, u8"视图时间线轴", 18)) return ExposedProperty(CSSPropertyID::kViewTimelineAxis, execution_context, mode);
      if (!memcmp(buffer, u8"视图过渡名称", 18)) return ExposedProperty(CSSPropertyID::kViewTransitionName, execution_context, mode);
      if (!memcmp(buffer, u8"起始结束圆角", 18)) return ExposedProperty(CSSPropertyID::kBorderStartEndRadius, execution_context, mode);
      if (!memcmp(buffer, u8"起始起始圆角", 18)) return ExposedProperty(CSSPropertyID::kBorderStartStartRadius, execution_context, mode);
      if (!memcmp(buffer, u8"边框图像切片", 18)) return ExposedProperty(CSSPropertyID::kBorderImageSlice, execution_context, mode);
      if (!memcmp(buffer, u8"边框图像外扩", 18)) return ExposedProperty(CSSPropertyID::kBorderImageOutset, execution_context, mode);
      if (!memcmp(buffer, u8"边框图像宽度", 18)) return ExposedProperty(CSSPropertyID::kBorderImageWidth, execution_context, mode);
      if (!memcmp(buffer, u8"边框图像重复", 18)) return ExposedProperty(CSSPropertyID::kBorderImageRepeat, execution_context, mode);
      if (!memcmp(buffer, u8"过度滚动行为", 18)) return ExposedProperty(CSSPropertyID::kOverscrollBehavior, execution_context, mode);
      if (!memcmp(buffer, u8"过渡时间函数", 18)) return ExposedProperty(CSSPropertyID::kTransitionTimingFunction, execution_context, mode);
    } else if (utf8_len == 19) {
      if (!memcmp(buffer, u8"Webkit变换原点X", 19)) return ExposedProperty(CSSPropertyID::kWebkitTransformOriginX, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit变换原点Y", 19)) return ExposedProperty(CSSPropertyID::kWebkitTransformOriginY, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit变换原点Z", 19)) return ExposedProperty(CSSPropertyID::kWebkitTransformOriginZ, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit透视原点X", 19)) return ExposedProperty(CSSPropertyID::kWebkitPerspectiveOriginX, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit透视原点Y", 19)) return ExposedProperty(CSSPropertyID::kWebkitPerspectiveOriginY, execution_context, mode);
    } else if (utf8_len == 21) {
      if (!memcmp(buffer, u8"Webkit列规则宽度", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRuleWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列规则样式", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRuleStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit列规则颜色", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRuleColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit前置内边距", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingBefore, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit后置内边距", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingAfter, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit结束内边距", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingEnd, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit背面可见性", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackfaceVisibility, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit起始内边距", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingStart, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩框图像", 21)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImage, execution_context, mode);
      if (!memcmp(buffer, u8"交叉轴自身对齐", 21)) return ExposedProperty(CSSPropertyID::kAlignSelf, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束内边距", 21)) return ExposedProperty(CSSPropertyID::kPaddingInlineEnd, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始内边距", 21)) return ExposedProperty(CSSPropertyID::kPaddingInlineStart, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后填充", 21)) return ExposedProperty(CSSPropertyID::kInternalVisitedFill, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后描边", 21)) return ExposedProperty(CSSPropertyID::kInternalVisitedStroke, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后颜色", 21)) return ExposedProperty(CSSPropertyID::kInternalVisitedColor, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束内边距", 21)) return ExposedProperty(CSSPropertyID::kPaddingBlockEnd, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始内边距", 21)) return ExposedProperty(CSSPropertyID::kPaddingBlockStart, execution_context, mode);
      if (!memcmp(buffer, u8"文本下划线位置", 21)) return ExposedProperty(CSSPropertyID::kTextUnderlinePosition, execution_context, mode);
      if (!memcmp(buffer, u8"文本下划线偏移", 21)) return ExposedProperty(CSSPropertyID::kTextUnderlineOffset, execution_context, mode);
      if (!memcmp(buffer, u8"源试用测试属性", 21)) return ExposedProperty(CSSPropertyID::kOriginTrialTestProperty, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内联内边距", 21)) return ExposedProperty(CSSPropertyID::kScrollPaddingInline, execution_context, mode);
      if (!memcmp(buffer, u8"滚动块级内边距", 21)) return ExposedProperty(CSSPropertyID::kScrollPaddingBlock, execution_context, mode);
      if (!memcmp(buffer, u8"滚动时间线名称", 21)) return ExposedProperty(CSSPropertyID::kScrollTimelineName, execution_context, mode);
      if (!memcmp(buffer, u8"视图时间线名称", 21)) return ExposedProperty(CSSPropertyID::kViewTimelineName, execution_context, mode);
      if (!memcmp(buffer, u8"视图时间线嵌入", 21)) return ExposedProperty(CSSPropertyID::kViewTimelineInset, execution_context, mode);
    } else if (utf8_len == 22) {
      if (!memcmp(buffer, u8"EPUB文本强调样式", 22)) return ExposedProperty(CSSPropertyID::kAliasEpubTextEmphasisStyle, execution_context, mode);
      if (!memcmp(buffer, u8"EPUB文本强调颜色", 22)) return ExposedProperty(CSSPropertyID::kAliasEpubTextEmphasisColor, execution_context, mode);
    } else if (utf8_len == 24) {
      if (!memcmp(buffer, u8"Webkit从右到左排序", 24)) return ExposedProperty(CSSPropertyID::kWebkitRtlOrdering, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit前置边框宽度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBeforeWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit前置边框样式", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBeforeStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit前置边框颜色", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBeforeColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画填充模式", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationFillMode, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画播放状态", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationPlayState, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画时间函数", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationTimingFunction, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit动画迭代次数", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationIterationCount, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit后置边框宽度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfterWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit后置边框样式", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfterStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit后置边框颜色", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfterColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit垂直边框间距", 24)) return ExposedProperty(CSSPropertyID::kWebkitBorderVerticalSpacing, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit字体特性设置", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitFontFeatureSettings, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit形状图像阈值", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitShapeImageThreshold, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本填充颜色", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextFillColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本大小调整", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本强调位置", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasisPosition, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本强调样式", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasisStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本强调颜色", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasisColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本描边宽度", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextStrokeWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit文本描边颜色", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextStrokeColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit最大逻辑宽度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaxLogicalWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit最大逻辑高度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaxLogicalHeight, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit最小逻辑宽度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMinLogicalWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit最小逻辑高度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMinLogicalHeight, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit水平边框间距", 24)) return ExposedProperty(CSSPropertyID::kWebkitBorderHorizontalSpacing, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit点击高亮颜色", 24)) return ExposedProperty(CSSPropertyID::kWebkitTapHighlightColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit生效文本装饰", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextDecorationsInEffect, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit盒子装饰断行", 24)) return ExposedProperty(CSSPropertyID::kWebkitBoxDecorationBreak, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit结束边框宽度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEndWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit结束边框样式", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEndStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit结束边框颜色", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEndColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit起始边框宽度", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStartWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit起始边框样式", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStartStyle, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit起始边框颜色", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStartColor, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit过渡时间函数", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionTimingFunction, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩垂直位置", 24)) return ExposedProperty(CSSPropertyID::kWebkitMaskPositionY, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩框图像源", 24)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageSource, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩水平位置", 24)) return ExposedProperty(CSSPropertyID::kWebkitMaskPositionX, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束边框宽度", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineEndWidth, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束边框样式", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineEndStyle, execution_context, mode);
      if (!memcmp(buffer, u8"内联结束边框颜色", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineEndColor, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始边框宽度", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineStartWidth, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始边框样式", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineStartStyle, execution_context, mode);
      if (!memcmp(buffer, u8"内联起始边框颜色", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineStartColor, execution_context, mode);
      if (!memcmp(buffer, u8"内联过度滚动行为", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorInline, execution_context, mode);
      if (!memcmp(buffer, u8"内部块级内容对齐", 24)) return ExposedProperty(CSSPropertyID::kInternalAlignContentBlock, execution_context, mode);
      if (!memcmp(buffer, u8"内部字体大小增量", 24)) return ExposedProperty(CSSPropertyID::kInternalFontSizeDelta, execution_context, mode);
      if (!memcmp(buffer, u8"内部强制背景颜色", 24)) return ExposedProperty(CSSPropertyID::kInternalForcedBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部强制轮廓颜色", 24)) return ExposedProperty(CSSPropertyID::kInternalForcedOutlineColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部强制边框颜色", 24)) return ExposedProperty(CSSPropertyID::kInternalForcedBorderColor, execution_context, mode);
      if (!memcmp(buffer, u8"固有包含内联尺寸", 24)) return ExposedProperty(CSSPropertyID::kContainIntrinsicInlineSize, execution_context, mode);
      if (!memcmp(buffer, u8"固有包含块级尺寸", 24)) return ExposedProperty(CSSPropertyID::kContainIntrinsicBlockSize, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束边框宽度", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockEndWidth, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束边框样式", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockEndStyle, execution_context, mode);
      if (!memcmp(buffer, u8"块级结束边框颜色", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockEndColor, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始边框宽度", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockStartWidth, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始边框样式", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockStartStyle, execution_context, mode);
      if (!memcmp(buffer, u8"块级起始边框颜色", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockStartColor, execution_context, mode);
      if (!memcmp(buffer, u8"块级过度滚动行为", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorBlock, execution_context, mode);
      if (!memcmp(buffer, u8"垂直过度滚动行为", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorY, execution_context, mode);
      if (!memcmp(buffer, u8"字体合成小型大写", 24)) return ExposedProperty(CSSPropertyID::kFontSynthesisSmallCaps, execution_context, mode);
      if (!memcmp(buffer, u8"文本装饰避让笔画", 24)) return ExposedProperty(CSSPropertyID::kTextDecorationSkipInk, execution_context, mode);
      if (!memcmp(buffer, u8"水平过度滚动行为", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorX, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内联结束边距", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginInlineEnd, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内联起始边距", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginInlineStart, execution_context, mode);
      if (!memcmp(buffer, u8"滚动块级结束边距", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginBlockEnd, execution_context, mode);
      if (!memcmp(buffer, u8"滚动块级起始边距", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginBlockStart, execution_context, mode);
    } else if (utf8_len == 27) {
      if (!memcmp(buffer, u8"Webkit遮罩框图像切片", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageSlice, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩框图像外扩", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageOutset, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩框图像宽度", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageWidth, execution_context, mode);
      if (!memcmp(buffer, u8"Webkit遮罩框图像重复", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageRepeat, execution_context, mode);
      if (!memcmp(buffer, u8"内部强制访问后颜色", 27)) return ExposedProperty(CSSPropertyID::kInternalForcedVisitedColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后光标颜色", 27)) return ExposedProperty(CSSPropertyID::kInternalVisitedCaretColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后背景颜色", 27)) return ExposedProperty(CSSPropertyID::kInternalVisitedBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后轮廓颜色", 27)) return ExposedProperty(CSSPropertyID::kInternalVisitedOutlineColor, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内联结束内边距", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingInlineEnd, execution_context, mode);
      if (!memcmp(buffer, u8"滚动内联起始内边距", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingInlineStart, execution_context, mode);
      if (!memcmp(buffer, u8"滚动块级结束内边距", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingBlockEnd, execution_context, mode);
      if (!memcmp(buffer, u8"滚动块级起始内边距", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingBlockStart, execution_context, mode);
    } else if (utf8_len == 30) {
      if (!memcmp(buffer, u8"内部访问后上边框颜色", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderTopColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后下边框颜色", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderBottomColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后列规则颜色", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedColumnRuleColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后右边框颜色", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderRightColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后左边框颜色", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderLeftColor, execution_context, mode);
    } else if (utf8_len == 33) {
      if (!memcmp(buffer, u8"内部访问后文本填充颜色", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextFillColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后文本强调颜色", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextEmphasisColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后文本描边颜色", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextStrokeColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后文本装饰颜色", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextDecorationColor, execution_context, mode);
    } else if (utf8_len == 39) {
      if (!memcmp(buffer, u8"内部访问后内联结束边框颜色", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderInlineEndColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后内联起始边框颜色", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderInlineStartColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后块级结束边框颜色", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderBlockEndColor, execution_context, mode);
      if (!memcmp(buffer, u8"内部访问后块级起始边框颜色", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderBlockStartColor, execution_context, mode);
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
  if (utf8_len >= 3 && static_cast<unsigned char>(buffer[0]) >= 0xE4) {
    if (utf8_len == 3) {
      if (!memcmp(buffer, u8"列", 3)) return CSSValueID::kColumn;
      if (!memcmp(buffer, u8"圆", 3)) return CSSValueID::kRound;
      if (!memcmp(buffer, u8"小", 3)) return CSSValueID::kSmall;
      if (!memcmp(buffer, u8"棕", 3)) return CSSValueID::kBrown;
      if (!memcmp(buffer, u8"橙", 3)) return CSSValueID::kOrange;
      if (!memcmp(buffer, u8"灰", 3)) return CSSValueID::kGray;
      if (!memcmp(buffer, u8"白", 3)) return CSSValueID::kWhite;
      if (!memcmp(buffer, u8"粉", 3)) return CSSValueID::kPink;
      if (!memcmp(buffer, u8"粗", 3)) return CSSValueID::kThick;
      if (!memcmp(buffer, u8"紫", 3)) return CSSValueID::kPurple;
      if (!memcmp(buffer, u8"红", 3)) return CSSValueID::kRed;
      if (!memcmp(buffer, u8"细", 3)) return CSSValueID::kThin;
      if (!memcmp(buffer, u8"绿", 3)) return CSSValueID::kGreen;
      if (!memcmp(buffer, u8"蓝", 3)) return CSSValueID::kBlue;
      if (!memcmp(buffer, u8"行", 3)) return CSSValueID::kRow;
      if (!memcmp(buffer, u8"青", 3)) return CSSValueID::kCyan;
      if (!memcmp(buffer, u8"黄", 3)) return CSSValueID::kYellow;
      if (!memcmp(buffer, u8"黑", 3)) return CSSValueID::kBlack;
      if (!memcmp(buffer, u8"无", 3)) return CSSValueID::kNone;
    } else if (utf8_len == 6) {
      if (!memcmp(buffer, u8"上标", 6)) return CSSValueID::kSuper;
      if (!memcmp(buffer, u8"下标", 6)) return CSSValueID::kSub;
      if (!memcmp(buffer, u8"两端", 6)) return CSSValueID::kJustify;
      if (!memcmp(buffer, u8"中央", 6)) return CSSValueID::kCentral;
      if (!memcmp(buffer, u8"中等", 6)) return CSSValueID::kMedium;
      if (!memcmp(buffer, u8"中间", 6)) return CSSValueID::kMiddle;
      if (!memcmp(buffer, u8"交替", 6)) return CSSValueID::kAlternate;
      if (!memcmp(buffer, u8"任意", 6)) return CSSValueID::kAnywhere;
      if (!memcmp(buffer, u8"优美", 6)) return CSSValueID::kPretty;
      if (!memcmp(buffer, u8"侧向", 6)) return CSSValueID::kSideways;
      if (!memcmp(buffer, u8"倾斜", 6)) return CSSValueID::kOblique;
      if (!memcmp(buffer, u8"全宽", 6)) return CSSValueID::kFullWidth;
      if (!memcmp(buffer, u8"全部", 6)) return CSSValueID::kAll;
      if (!memcmp(buffer, u8"关闭", 6)) return CSSValueID::kClose;
      if (!memcmp(buffer, u8"内容", 6)) return CSSValueID::kContents;
      if (!memcmp(buffer, u8"内嵌", 6)) return CSSValueID::kInset;
      if (!memcmp(buffer, u8"内联", 6)) return CSSValueID::kInline;
      if (!memcmp(buffer, u8"内部", 6)) return CSSValueID::kInside;
      if (!memcmp(buffer, u8"凹槽", 6)) return CSSValueID::kGroove;
      if (!memcmp(buffer, u8"分离", 6)) return CSSValueID::kSeparate;
      if (!memcmp(buffer, u8"切片", 6)) return CSSValueID::kSlice;
      if (!memcmp(buffer, u8"初始", 6)) return CSSValueID::kInitial;
      if (!memcmp(buffer, u8"别名", 6)) return CSSValueID::kAlias;
      if (!memcmp(buffer, u8"前进", 6)) return CSSValueID::kForwards;
      if (!memcmp(buffer, u8"包含", 6)) return CSSValueID::kContain;
      if (!memcmp(buffer, u8"包裹", 6)) return CSSValueID::kWrap;
      if (!memcmp(buffer, u8"十字", 6)) return CSSValueID::kCrosshair;
      if (!memcmp(buffer, u8"半径", 6)) return CSSValueID::kR;
      if (!memcmp(buffer, u8"模糊", 6)) return CSSValueID::kBlur;
      if (!memcmp(buffer, u8"双向", 6)) return CSSValueID::kBoth;
      if (!memcmp(buffer, u8"双线", 6)) return CSSValueID::kDouble;
      if (!memcmp(buffer, u8"反面", 6)) return CSSValueID::kVerso;
      if (!memcmp(buffer, u8"变亮", 6)) return CSSValueID::kLighten;
      if (!memcmp(buffer, u8"变暗", 6)) return CSSValueID::kDarken;
      if (!memcmp(buffer, u8"可见", 6)) return CSSValueID::kVisible;
      if (!memcmp(buffer, u8"右侧", 6)) return CSSValueID::kRight;
      if (!memcmp(buffer, u8"后退", 6)) return CSSValueID::kBackwards;
      if (!memcmp(buffer, u8"回退", 6)) return CSSValueID::kFallback;
      if (!memcmp(buffer, u8"固定", 6)) return CSSValueID::kFixed;
      if (!memcmp(buffer, u8"圆形", 6)) return CSSValueID::kCircle;
      if (!memcmp(buffer, u8"圆点", 6)) return CSSValueID::kDisc;
      if (!memcmp(buffer, u8"块级", 6)) return CSSValueID::kBlock;
      if (!memcmp(buffer, u8"基线", 6)) return CSSValueID::kBaseline;
      if (!memcmp(buffer, u8"填充", 6)) return CSSValueID::kFill;
      if (!memcmp(buffer, u8"复制", 6)) return CSSValueID::kCopy;
      if (!memcmp(buffer, u8"外扩", 6)) return CSSValueID::kOutset;
      if (!memcmp(buffer, u8"外部", 6)) return CSSValueID::kOutside;
      if (!memcmp(buffer, u8"大写", 6)) return CSSValueID::kUppercase;
      if (!memcmp(buffer, u8"奇偶", 6)) return CSSValueID::kEvenodd;
      if (!memcmp(buffer, u8"始终", 6)) return CSSValueID::kAlways;
      if (!memcmp(buffer, u8"安全", 6)) return CSSValueID::kSafe;
      if (!memcmp(buffer, u8"实线", 6)) return CSSValueID::kSolid;
      if (!memcmp(buffer, u8"宽度", 6)) return CSSValueID::kWidth;
      if (!memcmp(buffer, u8"小写", 6)) return CSSValueID::kLowercase;
      if (!memcmp(buffer, u8"尺寸", 6)) return CSSValueID::kSize;
      if (!memcmp(buffer, u8"局部", 6)) return CSSValueID::kLocal;
      if (!memcmp(buffer, u8"居中", 6)) return CSSValueID::kCenter;
      if (!memcmp(buffer, u8"嵌入", 6)) return CSSValueID::kEmbed;
      if (!memcmp(buffer, u8"左侧", 6)) return CSSValueID::kLeft;
      if (!memcmp(buffer, u8"差值", 6)) return CSSValueID::kDifference;
      if (!memcmp(buffer, u8"布局", 6)) return CSSValueID::kLayout;
      if (!memcmp(buffer, u8"帮助", 6)) return CSSValueID::kHelp;
      if (!memcmp(buffer, u8"平滑", 6)) return CSSValueID::kSmooth;
      if (!memcmp(buffer, u8"平移", 6)) return CSSValueID::kTranslate;
      if (!memcmp(buffer, u8"平端", 6)) return CSSValueID::kButt;
      if (!memcmp(buffer, u8"平衡", 6)) return CSSValueID::kBalance;
      if (!memcmp(buffer, u8"平面", 6)) return CSSValueID::kFlat;
      if (!memcmp(buffer, u8"序数", 6)) return CSSValueID::kOrdinal;
      if (!memcmp(buffer, u8"底部", 6)) return CSSValueID::kBottom;
      if (!memcmp(buffer, u8"弹性", 6)) return CSSValueID::kFlex;
      if (!memcmp(buffer, u8"强光", 6)) return CSSValueID::kHardLight;
      if (!memcmp(buffer, u8"手动", 6)) return CSSValueID::kManual;
      if (!memcmp(buffer, u8"打开", 6)) return CSSValueID::kOpen;
      if (!memcmp(buffer, u8"扩展", 6)) return CSSValueID::kExpanded;
      if (!memcmp(buffer, u8"抓取", 6)) return CSSValueID::kGrab;
      if (!memcmp(buffer, u8"折叠", 6)) return CSSValueID::kCollapse;
      if (!memcmp(buffer, u8"拉伸", 6)) return CSSValueID::kStretch;
      if (!memcmp(buffer, u8"指针", 6)) return CSSValueID::kPointer;
      if (!memcmp(buffer, u8"排除", 6)) return CSSValueID::kExclusion;
      if (!memcmp(buffer, u8"描边", 6)) return CSSValueID::kStroke;
      if (!memcmp(buffer, u8"放大", 6)) return CSSValueID::kZoomIn;
      if (!memcmp(buffer, u8"文本", 6)) return CSSValueID::kText;
      if (!memcmp(buffer, u8"斜体", 6)) return CSSValueID::kItalic;
      if (!memcmp(buffer, u8"斜接", 6)) return CSSValueID::kMiter;
      if (!memcmp(buffer, u8"斜面", 6)) return CSSValueID::kBevel;
      if (!memcmp(buffer, u8"断词", 6)) return CSSValueID::kBreakWord;
      if (!memcmp(buffer, u8"方形", 6)) return CSSValueID::kSquare;
      if (!memcmp(buffer, u8"旋转", 6)) return CSSValueID::kRotate;
      if (!memcmp(buffer, u8"无限", 6)) return CSSValueID::kInfinite;
      if (!memcmp(buffer, u8"明度", 6)) return CSSValueID::kLuminosity;
      if (!memcmp(buffer, u8"更大", 6)) return CSSValueID::kXLarge;
      if (!memcmp(buffer, u8"更小", 6)) return CSSValueID::kXSmall;
      if (!memcmp(buffer, u8"更粗", 6)) return CSSValueID::kBolder;
      if (!memcmp(buffer, u8"更细", 6)) return CSSValueID::kLighter;
      if (!memcmp(buffer, u8"柔光", 6)) return CSSValueID::kSoftLight;
      if (!memcmp(buffer, u8"样式", 6)) return CSSValueID::kStyle;
      if (!memcmp(buffer, u8"椭圆", 6)) return CSSValueID::kEllipse;
      if (!memcmp(buffer, u8"正常", 6)) return CSSValueID::kNormal;
      if (!memcmp(buffer, u8"正面", 6)) return CSSValueID::kRecto;
      if (!memcmp(buffer, u8"混合", 6)) return CSSValueID::kMixed;
      if (!memcmp(buffer, u8"清除", 6)) return CSSValueID::kClear;
      if (!memcmp(buffer, u8"滚动", 6)) return CSSValueID::kScroll;
      if (!memcmp(buffer, u8"滤色", 6)) return CSSValueID::kScreen;
      if (!memcmp(buffer, u8"点线", 6)) return CSSValueID::kDotted;
      if (!memcmp(buffer, u8"特大", 6)) return CSSValueID::kXxLarge;
      if (!memcmp(buffer, u8"特小", 6)) return CSSValueID::kXxSmall;
      if (!memcmp(buffer, u8"直立", 6)) return CSSValueID::kUpright;
      if (!memcmp(buffer, u8"相对", 6)) return CSSValueID::kRelative;
      if (!memcmp(buffer, u8"矩形", 6)) return CSSValueID::kRect;
      if (!memcmp(buffer, u8"矩阵", 6)) return CSSValueID::kMatrix;
      if (!memcmp(buffer, u8"禁止", 6)) return CSSValueID::kNotAllowed;
      if (!memcmp(buffer, u8"移动", 6)) return CSSValueID::kMove;
      if (!memcmp(buffer, u8"稳定", 6)) return CSSValueID::kStable;
      if (!memcmp(buffer, u8"等待", 6)) return CSSValueID::kWait;
      if (!memcmp(buffer, u8"粗体", 6)) return CSSValueID::kBold;
      if (!memcmp(buffer, u8"粘性", 6)) return CSSValueID::kSticky;
      if (!memcmp(buffer, u8"紧缩", 6)) return CSSValueID::kCondensed;
      if (!memcmp(buffer, u8"线性", 6)) return CSSValueID::kLinear;
      if (!memcmp(buffer, u8"终点", 6)) return CSSValueID::kEnd;
      if (!memcmp(buffer, u8"绘制", 6)) return CSSValueID::kPaint;
      if (!memcmp(buffer, u8"绝对", 6)) return CSSValueID::kAbsolute;
      if (!memcmp(buffer, u8"继承", 6)) return CSSValueID::kInherit;
      if (!memcmp(buffer, u8"缓入", 6)) return CSSValueID::kEaseIn;
      if (!memcmp(buffer, u8"缓出", 6)) return CSSValueID::kEaseOut;
      if (!memcmp(buffer, u8"缓动", 6)) return CSSValueID::kEase;
      if (!memcmp(buffer, u8"缩小", 6)) return CSSValueID::kZoomOut;
      if (!memcmp(buffer, u8"缩放", 6)) return CSSValueID::kScale;
      if (!memcmp(buffer, u8"网格", 6)) return CSSValueID::kGrid;
      if (!memcmp(buffer, u8"背景", 6)) return CSSValueID::kBackground;
      if (!memcmp(buffer, u8"脊线", 6)) return CSSValueID::kRidge;
      if (!memcmp(buffer, u8"自动", 6)) return CSSValueID::kAuto;
      if (!memcmp(buffer, u8"色相", 6)) return CSSValueID::kHue;
      if (!memcmp(buffer, u8"虚线", 6)) return CSSValueID::kDashed;
      if (!memcmp(buffer, u8"表格", 6)) return CSSValueID::kTable;
      if (!memcmp(buffer, u8"裁剪", 6)) return CSSValueID::kClip;
      if (!memcmp(buffer, u8"覆盖", 6)) return CSSValueID::kCover;
      if (!memcmp(buffer, u8"起点", 6)) return CSSValueID::kStart;
      if (!memcmp(buffer, u8"边框", 6)) return CSSValueID::kBorder;
      if (!memcmp(buffer, u8"还原", 6)) return CSSValueID::kUnset;
      if (!memcmp(buffer, u8"透明", 6)) return CSSValueID::kTransparent;
      if (!memcmp(buffer, u8"透视", 6)) return CSSValueID::kPerspective;
      if (!memcmp(buffer, u8"避免", 6)) return CSSValueID::kAvoid;
      if (!memcmp(buffer, u8"重复", 6)) return CSSValueID::kRepeat;
      if (!memcmp(buffer, u8"闪烁", 6)) return CSSValueID::kBlink;
      if (!memcmp(buffer, u8"间隔", 6)) return CSSValueID::kSpace;
      if (!memcmp(buffer, u8"隐藏", 6)) return CSSValueID::kHidden;
      if (!memcmp(buffer, u8"隔离", 6)) return CSSValueID::kIsolate;
      if (!memcmp(buffer, u8"静态", 6)) return CSSValueID::kStatic;
      if (!memcmp(buffer, u8"非零", 6)) return CSSValueID::kNonzero;
      if (!memcmp(buffer, u8"页面", 6)) return CSSValueID::kPage;
      if (!memcmp(buffer, u8"顶部", 6)) return CSSValueID::kTop;
      if (!memcmp(buffer, u8"颜色", 6)) return CSSValueID::kColor;
      if (!memcmp(buffer, u8"高度", 6)) return CSSValueID::kHeight;
      if (!memcmp(buffer, u8"默认", 6)) return CSSValueID::kDefault;
    } else if (utf8_len == 7) {
      if (!memcmp(buffer, u8"X坐标", 7)) return CSSValueID::kX;
      if (!memcmp(buffer, u8"Y坐标", 7)) return CSSValueID::kY;
    } else if (utf8_len == 8) {
      if (!memcmp(buffer, u8"保持3D", 8)) return CSSValueID::kPreserve3d;
    } else if (utf8_len == 9) {
      if (!memcmp(buffer, u8"上下文", 9)) return CSSValueID::kContextual;
      if (!memcmp(buffer, u8"上划线", 9)) return CSSValueID::kOverline;
      if (!memcmp(buffer, u8"下划线", 9)) return CSSValueID::kUnderline;
      if (!memcmp(buffer, u8"不安全", 9)) return CSSValueID::kUnsafe;
      if (!memcmp(buffer, u8"不换行", 9)) return CSSValueID::kNowrap;
      if (!memcmp(buffer, u8"不重复", 9)) return CSSValueID::kNoRepeat;
      if (!memcmp(buffer, u8"像素化", 9)) return CSSValueID::kPixelated;
      if (!memcmp(buffer, u8"内容盒", 9)) return CSSValueID::kContentBox;
      if (!memcmp(buffer, u8"内边距", 9)) return CSSValueID::kPadding;
      if (!memcmp(buffer, u8"列表项", 9)) return CSSValueID::kListItem;
      if (!memcmp(buffer, u8"列调整", 9)) return CSSValueID::kColResize;
      if (!memcmp(buffer, u8"删除线", 9)) return CSSValueID::kLineThrough;
      if (!memcmp(buffer, u8"十进制", 9)) return CSSValueID::kDecimal;
      if (!memcmp(buffer, u8"半扩展", 9)) return CSSValueID::kSemiExpanded;
      if (!memcmp(buffer, u8"半紧缩", 9)) return CSSValueID::kSemiCondensed;
      if (!memcmp(buffer, u8"单元格", 9)) return CSSValueID::kCell;
      if (!memcmp(buffer, u8"反向列", 9)) return CSSValueID::kColumnReverse;
      if (!memcmp(buffer, u8"反向行", 9)) return CSSValueID::kRowReverse;
      if (!memcmp(buffer, u8"填充盒", 9)) return CSSValueID::kFillBox;
      if (!memcmp(buffer, u8"弹性流", 9)) return CSSValueID::kFlexFlow;
      if (!memcmp(buffer, u8"抓取中", 9)) return CSSValueID::kGrabbing;
      if (!memcmp(buffer, u8"描边盒", 9)) return CSSValueID::kStrokeBox;
      if (!memcmp(buffer, u8"斜分数", 9)) return CSSValueID::kDiagonalFractions;
      if (!memcmp(buffer, u8"斜杠零", 9)) return CSSValueID::kSlashedZero;
      if (!memcmp(buffer, u8"更扩展", 9)) return CSSValueID::kExtraExpanded;
      if (!memcmp(buffer, u8"更紧缩", 9)) return CSSValueID::kExtraCondensed;
      if (!memcmp(buffer, u8"最近角", 9)) return CSSValueID::kClosestCorner;
      if (!memcmp(buffer, u8"最近边", 9)) return CSSValueID::kClosestSide;
      if (!memcmp(buffer, u8"最远角", 9)) return CSSValueID::kFarthestCorner;
      if (!memcmp(buffer, u8"最远边", 9)) return CSSValueID::kFarthestSide;
      if (!memcmp(buffer, u8"省略号", 9)) return CSSValueID::kEllipsis;
      if (!memcmp(buffer, u8"纯文本", 9)) return CSSValueID::kPlaintext;
      if (!memcmp(buffer, u8"行调整", 9)) return CSSValueID::kRowResize;
      if (!memcmp(buffer, u8"表格行", 9)) return CSSValueID::kTableRow;
      if (!memcmp(buffer, u8"覆盖层", 9)) return CSSValueID::kOverlay;
      if (!memcmp(buffer, u8"视图盒", 9)) return CSSValueID::kViewBox;
      if (!memcmp(buffer, u8"超扩展", 9)) return CSSValueID::kUltraExpanded;
      if (!memcmp(buffer, u8"超紧缩", 9)) return CSSValueID::kUltraCondensed;
      if (!memcmp(buffer, u8"边框盒", 9)) return CSSValueID::kBorderBox;
      if (!memcmp(buffer, u8"进行中", 9)) return CSSValueID::kProgress;
      if (!memcmp(buffer, u8"饱和度", 9)) return CSSValueID::kSaturation;
      if (!memcmp(buffer, u8"红绿蓝", 9)) return CSSValueID::kRgb;
    } else if (utf8_len == 12) {
      if (!memcmp(buffer, u8"Webkit弹性", 12)) return CSSValueID::kWebkitFlex;
      if (!memcmp(buffer, u8"红绿蓝透", 12)) return CSSValueID::kRgba;
      if (!memcmp(buffer, u8"两端对齐", 12)) return CSSValueID::kSpaceBetween;
      if (!memcmp(buffer, u8"从右到左", 12)) return CSSValueID::kRtl;
      if (!memcmp(buffer, u8"从左到右", 12)) return CSSValueID::kLtr;
      if (!memcmp(buffer, u8"任意断行", 12)) return CSSValueID::kBreakAll;
      if (!memcmp(buffer, u8"保持整体", 12)) return CSSValueID::kKeepAll;
      if (!memcmp(buffer, u8"保留换行", 12)) return CSSValueID::kPreLine;
      if (!memcmp(buffer, u8"保留空白", 12)) return CSSValueID::kPre;
      if (!memcmp(buffer, u8"全部滚动", 12)) return CSSValueID::kAllScroll;
      if (!memcmp(buffer, u8"内联块级", 12)) return CSSValueID::kInlineBlock;
      if (!memcmp(buffer, u8"内联尺寸", 12)) return CSSValueID::kInlineSize;
      if (!memcmp(buffer, u8"内联弹性", 12)) return CSSValueID::kInlineFlex;
      if (!memcmp(buffer, u8"内联网格", 12)) return CSSValueID::kInlineGrid;
      if (!memcmp(buffer, u8"内联表格", 12)) return CSSValueID::kInlineTable;
      if (!memcmp(buffer, u8"内边距盒", 12)) return CSSValueID::kPaddingBox;
      if (!memcmp(buffer, u8"历史连字", 12)) return CSSValueID::kHistoricalLigatures;
      if (!memcmp(buffer, u8"双向覆盖", 12)) return CSSValueID::kBidiOverride;
      if (!memcmp(buffer, u8"反向交替", 12)) return CSSValueID::kAlternateReverse;
      if (!memcmp(buffer, u8"反向换行", 12)) return CSSValueID::kWrapReverse;
      if (!memcmp(buffer, u8"可选连字", 12)) return CSSValueID::kDiscretionaryLigatures;
      if (!memcmp(buffer, u8"右上调整", 12)) return CSSValueID::kNeResize;
      if (!memcmp(buffer, u8"右下调整", 12)) return CSSValueID::kSeResize;
      if (!memcmp(buffer, u8"向下调整", 12)) return CSSValueID::kSResize;
      if (!memcmp(buffer, u8"向左调整", 12)) return CSSValueID::kWResize;
      if (!memcmp(buffer, u8"回退图层", 12)) return CSSValueID::kRevertLayer;
      if (!memcmp(buffer, u8"均匀对齐", 12)) return CSSValueID::kSpaceEvenly;
      if (!memcmp(buffer, u8"块级尺寸", 12)) return CSSValueID::kBlockSize;
      if (!memcmp(buffer, u8"垂直文本", 12)) return CSSValueID::kVerticalText;
      if (!memcmp(buffer, u8"垂直调整", 12)) return CSSValueID::kNResize;
      if (!memcmp(buffer, u8"垂直重复", 12)) return CSSValueID::kRepeatY;
      if (!memcmp(buffer, u8"堆叠分数", 12)) return CSSValueID::kStackedFractions;
      if (!memcmp(buffer, u8"字母基线", 12)) return CSSValueID::kAlphabetic;
      if (!memcmp(buffer, u8"定位区域", 12)) return CSSValueID::kPositionArea;
      if (!memcmp(buffer, u8"小型大写", 12)) return CSSValueID::kSmallCaps;
      if (!memcmp(buffer, u8"左上调整", 12)) return CSSValueID::kNwResize;
      if (!memcmp(buffer, u8"左下调整", 12)) return CSSValueID::kSwResize;
      if (!memcmp(buffer, u8"常见连字", 12)) return CSSValueID::kCommonLigatures;
      if (!memcmp(buffer, u8"弹性终点", 12)) return CSSValueID::kFlexEnd;
      if (!memcmp(buffer, u8"弹性起点", 12)) return CSSValueID::kFlexStart;
      if (!memcmp(buffer, u8"悬挂基线", 12)) return CSSValueID::kHanging;
      if (!memcmp(buffer, u8"数学基线", 12)) return CSSValueID::kMathematical;
      if (!memcmp(buffer, u8"文本底部", 12)) return CSSValueID::kTextBottom;
      if (!memcmp(buffer, u8"文本顶部", 12)) return CSSValueID::kTextTop;
      if (!memcmp(buffer, u8"断行空白", 12)) return CSSValueID::kBreakSpaces;
      if (!memcmp(buffer, u8"无上下文", 12)) return CSSValueID::kNoContextual;
      if (!memcmp(buffer, u8"旧式数字", 12)) return CSSValueID::kOldstyleNums;
      if (!memcmp(buffer, u8"最大内容", 12)) return CSSValueID::kMaxContent;
      if (!memcmp(buffer, u8"最小内容", 12)) return CSSValueID::kMinContent;
      if (!memcmp(buffer, u8"标题大写", 12)) return CSSValueID::kTitlingCaps;
      if (!memcmp(buffer, u8"正片叠底", 12)) return CSSValueID::kMultiply;
      if (!memcmp(buffer, u8"比例数字", 12)) return CSSValueID::kProportionalNums;
      if (!memcmp(buffer, u8"水平调整", 12)) return CSSValueID::kEResize;
      if (!memcmp(buffer, u8"水平重复", 12)) return CSSValueID::kRepeatX;
      if (!memcmp(buffer, u8"环绕对齐", 12)) return CSSValueID::kSpaceAround;
      if (!memcmp(buffer, u8"禁止放置", 12)) return CSSValueID::kNoDrop;
      if (!memcmp(buffer, u8"缓入缓出", 12)) return CSSValueID::kEaseInOut;
      if (!memcmp(buffer, u8"自身终点", 12)) return CSSValueID::kSelfEnd;
      if (!memcmp(buffer, u8"自身起点", 12)) return CSSValueID::kSelfStart;
      if (!memcmp(buffer, u8"表意基线", 12)) return CSSValueID::kIdeographic;
      if (!memcmp(buffer, u8"表格数字", 12)) return CSSValueID::kTabularNums;
      if (!memcmp(buffer, u8"表格标题", 12)) return CSSValueID::kTableCaption;
      if (!memcmp(buffer, u8"衬线数字", 12)) return CSSValueID::kLiningNums;
      if (!memcmp(buffer, u8"避免分列", 12)) return CSSValueID::kAvoidColumn;
      if (!memcmp(buffer, u8"避免分页", 12)) return CSSValueID::kAvoidPage;
      if (!memcmp(buffer, u8"阶跃终点", 12)) return CSSValueID::kStepEnd;
      if (!memcmp(buffer, u8"阶跃起点", 12)) return CSSValueID::kStepStart;
      if (!memcmp(buffer, u8"隔离覆盖", 12)) return CSSValueID::kIsolateOverride;
      if (!memcmp(buffer, u8"页面缩放", 12)) return CSSValueID::kZoom;
      if (!memcmp(buffer, u8"颜色减淡", 12)) return CSSValueID::kColorDodge;
      if (!memcmp(buffer, u8"颜色加深", 12)) return CSSValueID::kColorBurn;
    } else if (utf8_len == 15) {
      if (!memcmp(buffer, u8"上下文菜单", 15)) return CSSValueID::kContextMenu;
      if (!memcmp(buffer, u8"无常见连字", 15)) return CSSValueID::kNoCommonLigatures;
      if (!memcmp(buffer, u8"自适应内容", 15)) return CSSValueID::kFitContent;
      if (!memcmp(buffer, u8"表格单元格", 15)) return CSSValueID::kTableCell;
      if (!memcmp(buffer, u8"首字母大写", 15)) return CSSValueID::kCapitalize;
    } else if (utf8_len == 18) {
      if (!memcmp(buffer, u8"侧向从右到左", 18)) return CSSValueID::kSidewaysRl;
      if (!memcmp(buffer, u8"侧向从左到右", 18)) return CSSValueID::kSidewaysLr;
      if (!memcmp(buffer, u8"保留空白换行", 18)) return CSSValueID::kPreWrap;
      if (!memcmp(buffer, u8"全部小型大写", 18)) return CSSValueID::kAllSmallCaps;
      if (!memcmp(buffer, u8"垂直从右到左", 18)) return CSSValueID::kVerticalRl;
      if (!memcmp(buffer, u8"垂直从左到右", 18)) return CSSValueID::kVerticalLr;
      if (!memcmp(buffer, u8"垂直双向调整", 18)) return CSSValueID::kNsResize;
      if (!memcmp(buffer, u8"对角双向调整", 18)) return CSSValueID::kNeswResize;
      if (!memcmp(buffer, u8"水平双向调整", 18)) return CSSValueID::kEwResize;
      if (!memcmp(buffer, u8"水平自上而下", 18)) return CSSValueID::kHorizontalTb;
    } else if (utf8_len == 24) {
      if (!memcmp(buffer, u8"块级格式化上下文", 24)) return CSSValueID::kFlowRoot;
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

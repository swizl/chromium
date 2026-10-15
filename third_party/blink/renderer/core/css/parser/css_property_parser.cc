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
      if (!memcmp(buffer, "\xe4\xbd\x8d\xe7\xbd\xae", 6)) return ExposedProperty(CSSPropertyID::kPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x8f\xe7\xa7\xbb", 6)) return ExposedProperty(CSSPropertyID::kOffset, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x85\x89\xe6\xa0\x87", 6)) return ExposedProperty(CSSPropertyID::kCursor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x85\xa8\xe9\x83\xa8", 6)) return ExposedProperty(CSSPropertyID::kAll, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xae\xb9", 6)) return ExposedProperty(CSSPropertyID::kContent, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xb5\x8c", 6)) return ExposedProperty(CSSPropertyID::kInset, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\xa1\xac", 6)) return ExposedProperty(CSSPropertyID::kPad, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe5\xae\xbd", 6)) return ExposedProperty(CSSPropertyID::kColumnWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe6\x95\xb0", 6)) return ExposedProperty(CSSPropertyID::kColumnCount, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x89\x8d\xe7\xbc\x80", 6)) return ExposedProperty(CSSPropertyID::kPrefix, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa0\xe7\xb2\x97", 6)) return ExposedProperty(CSSPropertyID::kFontWeight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb", 6)) return ExposedProperty(CSSPropertyID::kAnimation, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8c\x85\xe5\x90\xab", 6)) return ExposedProperty(CSSPropertyID::kContain, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8d\x8a\xe5\xbe\x84", 6)) return ExposedProperty(CSSPropertyID::kR, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\x98\xe6\x8d\xa2", 6)) return ExposedProperty(CSSPropertyID::kTransform, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xaf\xe8\xa7\x81", 6)) return ExposedProperty(CSSPropertyID::kVisibility, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe4\xbe\xa7", 6)) return ExposedProperty(CSSPropertyID::kRight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x90\x8e\xe7\xbc\x80", 6)) return ExposedProperty(CSSPropertyID::kSuffix, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\x9e\xe9\x80\x80", 6)) return ExposedProperty(CSSPropertyID::kFallback, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9c\x86\xe8\xa7\x92", 6)) return ExposedProperty(CSSPropertyID::kBorderRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa1\xab\xe5\x85\x85", 6)) return ExposedProperty(CSSPropertyID::kFill, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa4\x96\xe8\xa7\x82", 6)) return ExposedProperty(CSSPropertyID::kAppearance, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa4\x9a\xe5\x88\x97", 6)) return ExposedProperty(CSSPropertyID::kColumns, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93", 6)) return ExposedProperty(CSSPropertyID::kFont, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\xa4\xe8\xa1\x8c", 6)) return ExposedProperty(CSSPropertyID::kWidows, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\xb9\xe5\x99\xa8", 6)) return ExposedProperty(CSSPropertyID::kContainer, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\xbd\xe5\xba\xa6", 6)) return ExposedProperty(CSSPropertyID::kWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xa1\xe8\xa1\x8c", 6)) return ExposedProperty(CSSPropertyID::kOrphans, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xb9\xe9\xbd\x90", 6)) return ExposedProperty(CSSPropertyID::kAlignItems, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xbc\xe8\x88\xaa", 6)) return ExposedProperty(CSSPropertyID::kNavigation, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb0\xba\xe5\xaf\xb8", 6)) return ExposedProperty(CSSPropertyID::kSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb1\x82\xe7\xba\xa7", 6)) return ExposedProperty(CSSPropertyID::kZIndex, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe4\xbe\xa7", 6)) return ExposedProperty(CSSPropertyID::kLeft, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb9\xb3\xe7\xa7\xbb", 6)) return ExposedProperty(CSSPropertyID::kTranslate, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xba\x95\xe9\x83\xa8", 6)) return ExposedProperty(CSSPropertyID::kBottom, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\x95\xe5\x8f\xb7", 6)) return ExposedProperty(CSSPropertyID::kQuotes, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7", 6)) return ExposedProperty(CSSPropertyID::kFlex, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8d\xa2\xe8\xa1\x8c", 6)) return ExposedProperty(CSSPropertyID::kLineBreak, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8e\x92\xe5\xba\x8f", 6)) return ExposedProperty(CSSPropertyID::kOrder, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9", 6)) return ExposedProperty(CSSPropertyID::kStroke, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x9c\xe4\xbd\x93", 6)) return ExposedProperty(CSSPropertyID::kFontStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\xad\xe8\xaf\x8d", 6)) return ExposedProperty(CSSPropertyID::kWordBreak, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\xb9\xe5\x90\x91", 6)) return ExposedProperty(CSSPropertyID::kDirection, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x97\x8b\xe8\xbd\xac", 6)) return ExposedProperty(CSSPropertyID::kRotate, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x98\xbe\xe7\xa4\xba", 6)) return ExposedProperty(CSSPropertyID::kDisplay, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x97\xe8\xaf\xbb", 6)) return ExposedProperty(CSSPropertyID::kSpeak, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9d\xa5\xe6\xba\x90", 6)) return ExposedProperty(CSSPropertyID::kSrc, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xa0\x87\xe8\xae\xb0", 6)) return ExposedProperty(CSSPropertyID::kMarker, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb5\xae\xe5\x8a\xa8", 6)) return ExposedProperty(CSSPropertyID::kFloat, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb8\x85\xe9\x99\xa4", 6)) return ExposedProperty(CSSPropertyID::kClear, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xba\xa2\xe5\x87\xba", 6)) return ExposedProperty(CSSPropertyID::kOverflow, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\xa4\xe9\x95\x9c", 6)) return ExposedProperty(CSSPropertyID::kFilter, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xac\xa6\xe5\x8f\xb7", 6)) return ExposedProperty(CSSPropertyID::kSymbols, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xb1\xbb\xe5\x9e\x8b", 6)) return ExposedProperty(CSSPropertyID::kTypes, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xb3\xbb\xe7\xbb\x9f", 6)) return ExposedProperty(CSSPropertyID::kSystem, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbb\x93\xe6\x9e\x9c", 6)) return ExposedProperty(CSSPropertyID::kResult, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbb\xa7\xe6\x89\xbf", 6)) return ExposedProperty(CSSPropertyID::kInherits, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbc\xa9\xe6\x94\xbe", 6)) return ExposedProperty(CSSPropertyID::kScale, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc", 6)) return ExposedProperty(CSSPropertyID::kGrid, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf", 6)) return ExposedProperty(CSSPropertyID::kBackground, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x8c\x83\xe5\x9b\xb4", 6)) return ExposedProperty(CSSPropertyID::kRange, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe9\xab\x98", 6)) return ExposedProperty(CSSPropertyID::kLineHeight, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa3\x81\xe5\x89\xaa", 6)) return ExposedProperty(CSSPropertyID::kClip, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xaf\xad\xe6\xb3\x95", 6)) return ExposedProperty(CSSPropertyID::kSyntax, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xb4\x9f\xe6\x95\xb0", 6)) return ExposedProperty(CSSPropertyID::kNegative, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbd\xae\xe5\xbb\x93", 6)) return ExposedProperty(CSSPropertyID::kOutline, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86", 6)) return ExposedProperty(CSSPropertyID::kBorder, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe8\xb7\x9d", 6)) return ExposedProperty(CSSPropertyID::kMargin, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe6\xb8\xa1", 6)) return ExposedProperty(CSSPropertyID::kTransition, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x80\x8f\xe6\x98\x8e", 6)) return ExposedProperty(CSSPropertyID::kOpacity, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x80\x8f\xe8\xa7\x86", 6)) return ExposedProperty(CSSPropertyID::kPerspective, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9", 6)) return ExposedProperty(CSSPropertyID::kMask, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x97\xb4\xe8\xb7\x9d", 6)) return ExposedProperty(CSSPropertyID::kGap, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x98\xb4\xe5\xbd\xb1", 6)) return ExposedProperty(CSSPropertyID::kBoxShadow, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x9a\x94\xe7\xa6\xbb", 6)) return ExposedProperty(CSSPropertyID::kIsolation, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe9\x9d\xa2", 6)) return ExposedProperty(CSSPropertyID::kPage, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb6\xe9\x83\xa8", 6)) return ExposedProperty(CSSPropertyID::kTop, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa2\x9c\xe8\x89\xb2", 6)) return ExposedProperty(CSSPropertyID::kColor, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xab\x98\xe5\xba\xa6", 6)) return ExposedProperty(CSSPropertyID::kHeight, execution_context, mode);
    } else if (utf8_len == 7) {
      if (!memcmp(buffer, "\x58\xe5\x9d\x90\xe6\xa0\x87", 7)) return ExposedProperty(CSSPropertyID::kX, execution_context, mode);
      if (!memcmp(buffer, "\x59\xe5\x9d\x90\xe6\xa0\x87", 7)) return ExposedProperty(CSSPropertyID::kY, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9c\x86\xe5\xbf\x83\x58", 7)) return ExposedProperty(CSSPropertyID::kCx, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9c\x86\xe5\xbf\x83\x59", 7)) return ExposedProperty(CSSPropertyID::kCy, execution_context, mode);
    } else if (utf8_len == 9) {
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe8\xbe\xb9\xe6\xa1\x86", 9)) return ExposedProperty(CSSPropertyID::kBorderTop, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe8\xbe\xb9\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kMarginTop, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe8\xbe\xb9\xe6\xa1\x86", 9)) return ExposedProperty(CSSPropertyID::kBorderBottom, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kMarginBottom, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xba\xa4\xe4\xba\x92\xe6\x80\xa7", 9)) return ExposedProperty(CSSPropertyID::kInteractivity, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kPadding, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe5\xa1\xab\xe5\x85\x85", 9)) return ExposedProperty(CSSPropertyID::kColumnFill, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe5\xbc\x80\xe5\xa7\x8b", 9)) return ExposedProperty(CSSPropertyID::kGridColumnStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe7\xbb\x93\xe6\x9d\x9f", 9)) return ExposedProperty(CSSPropertyID::kGridColumnEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99", 9)) return ExposedProperty(CSSPropertyID::kColumnRule, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xb7\xa8\xe8\xa1\x8c", 9)) return ExposedProperty(CSSPropertyID::kColumnSpan, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe9\x97\xb4\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kColumnGap, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x9d\xe5\xa7\x8b\xe5\x80\xbc", 9)) return ExposedProperty(CSSPropertyID::kInitialValue, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\x98\xe6\x8d\xa2\xe7\x9b\x92", 9)) return ExposedProperty(CSSPropertyID::kTransformBox, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe8\xbe\xb9\xe6\xa1\x86", 9)) return ExposedProperty(CSSPropertyID::kBorderRight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe8\xbe\xb9\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kMarginRight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe6\x97\x8f", 9)) return ExposedProperty(CSSPropertyID::kFontFamily, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe9\x97\xb4\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kLetterSpacing, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\xbd\xe9\xab\x98\xe6\xaf\x94", 9)) return ExposedProperty(CSSPropertyID::kAspectRatio, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb0\x86\xe5\x8f\x98\xe6\x9b\xb4", 9)) return ExposedProperty(CSSPropertyID::kWillChange, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe8\xbe\xb9\xe6\xa1\x86", 9)) return ExposedProperty(CSSPropertyID::kBorderLeft, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe8\xbe\xb9\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kMarginLeft, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe6\xb5\x81", 9)) return ExposedProperty(CSSPropertyID::kFlexFlow, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xba\xe8\xb0\x83\xe8\x89\xb2", 9)) return ExposedProperty(CSSPropertyID::kAccentColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\xa1\x86", 9)) return ExposedProperty(CSSPropertyID::kTextBox, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\xad\xe5\xad\x97\xe7\xac\xa6", 9)) return ExposedProperty(CSSPropertyID::kHyphenateCharacter, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xa8\xa1\xe6\x9d\xbf\xe5\x88\x97", 9)) return ExposedProperty(CSSPropertyID::kGridTemplateColumns, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xa8\xa1\xe6\x9d\xbf\xe8\xa1\x8c", 9)) return ExposedProperty(CSSPropertyID::kGridTemplateRows, execution_context, mode);
      if (!memcmp(buffer, "\xe7\x9b\x92\xe6\xa8\xa1\xe5\x9e\x8b", 9)) return ExposedProperty(CSSPropertyID::kBoxSizing, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe5\x88\x97", 9)) return ExposedProperty(CSSPropertyID::kGridColumn, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe8\xa1\x8c", 9)) return ExposedProperty(CSSPropertyID::kGridRow, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe8\x89\xb2", 9)) return ExposedProperty(CSSPropertyID::kBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x87\xaa\xe5\x8a\xa8\xe5\x88\x97", 9)) return ExposedProperty(CSSPropertyID::kGridAutoColumns, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x87\xaa\xe5\x8a\xa8\xe8\xa1\x8c", 9)) return ExposedProperty(CSSPropertyID::kGridAutoRows, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe5\xbc\x80\xe5\xa7\x8b", 9)) return ExposedProperty(CSSPropertyID::kGridRowStart, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe7\xbb\x93\xe6\x9d\x9f", 9)) return ExposedProperty(CSSPropertyID::kGridRowEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe9\x97\xb4\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kRowGap, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa6\x86\xe7\x9b\x96\xe5\xb1\x82", 9)) return ExposedProperty(CSSPropertyID::kOverlay, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xaf\x8d\xe9\x97\xb4\xe8\xb7\x9d", 9)) return ExposedProperty(CSSPropertyID::kWordSpacing, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x9e\xe5\xad\x97\xe7\xac\xa6", 9)) return ExposedProperty(CSSPropertyID::kHyphens, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x98\x85\xe8\xaf\xbb\xe6\xb5\x81", 9)) return ExposedProperty(CSSPropertyID::kReadingFlow, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa6\x96\xe5\xad\x97\xe6\xaf\x8d", 9)) return ExposedProperty(CSSPropertyID::kInitialLetter, execution_context, mode);
    } else if (utf8_len == 10) {
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\xad\xe8\xaf\x8d", 10)) return ExposedProperty(CSSPropertyID::kAliasEpubWordBreak, execution_context, mode);
    } else if (utf8_len == 12) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe5\xae\xbd", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe6\x95\xb0", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnCount, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimation, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\x98\xe6\x8d\xa2", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransform, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x9c\x86\xe8\xa7\x92", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderRadius, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xa4\x96\xe8\xa7\x82", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitAppearance, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xa4\x9a\xe5\x88\x97", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumns, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitAlignItems, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlex, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return ExposedProperty(CSSPropertyID::kWebkitLineBreak, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x8e\x92\xe5\xba\x8f", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitOrder, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\xbb\xa4\xe9\x95\x9c", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitFilter, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xbf\x87\xe6\xb8\xa1", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransition, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\x8f\xe8\xa7\x86", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitPerspective, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitMask, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x98\xb4\xe5\xbd\xb1", 12)) return ExposedProperty(CSSPropertyID::kAliasWebkitBoxShadow, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kPaddingTop, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe5\x8d\x87\xe8\xa6\x86\xe7\x9b\x96", 12)) return ExposedProperty(CSSPropertyID::kAscentOverride, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kPaddingBottom, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe9\x99\x8d\xe8\xa6\x86\xe7\x9b\x96", 12)) return ExposedProperty(CSSPropertyID::kDescentOverride, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\xad\xe9\x97\xb4\xe6\xa0\x87\xe8\xae\xb0", 12)) return ExposedProperty(CSSPropertyID::kMarkerMid, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\xbb\xe5\xaf\xbc\xe5\x9f\xba\xe7\xba\xbf", 12)) return ExposedProperty(CSSPropertyID::kDominantBaseline, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\xbb\xe8\xbd\xb4\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kJustifyContent, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb9\x8b\xe5\x89\x8d\xe6\x96\xad\xe9\xa1\xb5", 12)) return ExposedProperty(CSSPropertyID::kBreakBefore, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb9\x8b\xe5\x90\x8e\xe6\x96\xad\xe9\xa1\xb5", 12)) return ExposedProperty(CSSPropertyID::kBreakAfter, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb9\xa6\xe5\x86\x99\xe6\xa8\xa1\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kWritingMode, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x8f\xe7\xa7\xbb\xe4\xbd\x8d\xe7\xbd\xae", 12)) return ExposedProperty(CSSPropertyID::kOffsetPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x8f\xe7\xa7\xbb\xe6\x97\x8b\xe8\xbd\xac", 12)) return ExposedProperty(CSSPropertyID::kOffsetRotate, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x8f\xe7\xa7\xbb\xe8\xb7\x9d\xe7\xa6\xbb", 12)) return ExposedProperty(CSSPropertyID::kOffsetDistance, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x8f\xe7\xa7\xbb\xe8\xb7\xaf\xe5\xbe\x84", 12)) return ExposedProperty(CSSPropertyID::kOffsetPath, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x8f\xe7\xa7\xbb\xe9\x94\x9a\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kOffsetAnchor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x9c\xe6\xad\xa2\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kStopColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x85\x89\xe6\xa0\x87\xe5\x8a\xa8\xe7\x94\xbb", 12)) return ExposedProperty(CSSPropertyID::kCaretAnimation, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x85\x89\xe6\xa0\x87\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kCaretColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x85\x89\xe7\x85\xa7\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kLightingColor, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xae\xb9\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kAlignContent, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe5\xb0\xba\xe5\xaf\xb8", 12)) return ExposedProperty(CSSPropertyID::kInlineSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe5\xb5\x8c\xe5\x85\xa5", 12)) return ExposedProperty(CSSPropertyID::kInsetInline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe6\xba\xa2\xe5\x87\xba", 12)) return ExposedProperty(CSSPropertyID::kOverflowInline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xbe\xb9\xe6\xa1\x86", 12)) return ExposedProperty(CSSPropertyID::kBorderInline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kMarginInline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe6\x96\xad\xe9\xa1\xb5", 12)) return ExposedProperty(CSSPropertyID::kBreakInside, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa1\xa8\xe6\xa0\xb7\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kListStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe5\x90\x88\xe6\x88\x90", 12)) return ExposedProperty(CSSPropertyID::kAnimationComposition, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe5\x90\x8d\xe7\xa7\xb0", 12)) return ExposedProperty(CSSPropertyID::kAnimationName, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe5\xbb\xb6\xe8\xbf\x9f", 12)) return ExposedProperty(CSSPropertyID::kAnimationDelay, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe6\x96\xb9\xe5\x90\x91", 12)) return ExposedProperty(CSSPropertyID::kAnimationDirection, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe6\x97\xb6\xe9\x95\xbf", 12)) return ExposedProperty(CSSPropertyID::kAnimationDuration, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe8\x8c\x83\xe5\x9b\xb4", 12)) return ExposedProperty(CSSPropertyID::kAnimationRange, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe8\xa7\xa6\xe5\x8f\x91", 12)) return ExposedProperty(CSSPropertyID::kAnimationTrigger, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8d\x95\xe8\xaf\x8d\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return ExposedProperty(CSSPropertyID::kAliasWordWrap, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\x98\xe6\x8d\xa2\xe5\x8e\x9f\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kTransformOrigin, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\x98\xe6\x8d\xa2\xe6\xa0\xb7\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kTransformStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe4\xb8\x8a\xe5\x9c\x86\xe8\xa7\x92", 12)) return ExposedProperty(CSSPropertyID::kBorderTopRightRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe4\xb8\x8b\xe5\x9c\x86\xe8\xa7\x92", 12)) return ExposedProperty(CSSPropertyID::kBorderBottomRightRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kPaddingRight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xbe\xe5\x83\x8f\xe6\x96\xb9\xe5\x90\x91", 12)) return ExposedProperty(CSSPropertyID::kImageOrientation, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xbe\xe5\x83\x8f\xe6\xb8\xb2\xe6\x9f\x93", 12)) return ExposedProperty(CSSPropertyID::kImageRendering, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe5\xb0\xba\xe5\xaf\xb8", 12)) return ExposedProperty(CSSPropertyID::kBlockSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe5\xb5\x8c\xe5\x85\xa5", 12)) return ExposedProperty(CSSPropertyID::kInsetBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe6\xba\xa2\xe5\x87\xba", 12)) return ExposedProperty(CSSPropertyID::kOverflowBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xbe\xb9\xe6\xa1\x86", 12)) return ExposedProperty(CSSPropertyID::kBorderBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kMarginBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kVerticalAlign, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe6\xba\xa2\xe5\x87\xba", 12)) return ExposedProperty(CSSPropertyID::kOverflowY, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9f\xba\xe7\xba\xbf\xe5\x81\x8f\xe7\xa7\xbb", 12)) return ExposedProperty(CSSPropertyID::kBaselineShift, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9f\xba\xe7\xba\xbf\xe6\x9d\xa5\xe6\xba\x90", 12)) return ExposedProperty(CSSPropertyID::kBaselineSource, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa1\xab\xe5\x85\x85\xe8\xa7\x84\xe5\x88\x99", 12)) return ExposedProperty(CSSPropertyID::kFillRule, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa4\x96\xe9\x83\xa8\xe5\xbd\xa2\xe7\x8a\xb6", 12)) return ExposedProperty(CSSPropertyID::kShapeOutside, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa4\xa7\xe5\xb0\x8f\xe8\xb0\x83\xe6\x95\xb4", 12)) return ExposedProperty(CSSPropertyID::kSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x8f\x98\xe4\xbd\x93", 12)) return ExposedProperty(CSSPropertyID::kFontVariant, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x90\x88\xe6\x88\x90", 12)) return ExposedProperty(CSSPropertyID::kFontSynthesis, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f", 12)) return ExposedProperty(CSSPropertyID::kFontSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe6\x8b\x89\xe4\xbc\xb8", 12)) return ExposedProperty(CSSPropertyID::kFontStretch, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe6\x98\xbe\xe7\xa4\xba", 12)) return ExposedProperty(CSSPropertyID::kFontDisplay, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe6\xae\xb5\xe5\xa4\xa7\xe5\xb0\x8f", 12)) return ExposedProperty(CSSPropertyID::kFieldSizing, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe8\xb7\x9d\xe8\xb0\x83\xe6\x95\xb4", 12)) return ExposedProperty(CSSPropertyID::kFontKerning, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe5\x8c\xba\xe5\x9f\x9f", 12)) return ExposedProperty(CSSPropertyID::kPositionArea, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe5\xb0\x9d\xe8\xaf\x95", 12)) return ExposedProperty(CSSPropertyID::kPositionTry, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe9\x94\x9a\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kPositionAnchor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\xb9\xe5\x99\xa8\xe5\x90\x8d\xe7\xa7\xb0", 12)) return ExposedProperty(CSSPropertyID::kContainerName, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\xb9\xe5\x99\xa8\xe7\xb1\xbb\xe5\x9e\x8b", 12)) return ExposedProperty(CSSPropertyID::kContainerType, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xb9\xe8\xb1\xa1\xe4\xbd\x8d\xe7\xbd\xae", 12)) return ExposedProperty(CSSPropertyID::kObjectPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xb9\xe8\xb1\xa1\xe9\x80\x82\xe9\x85\x8d", 12)) return ExposedProperty(CSSPropertyID::kObjectFit, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xb9\xe9\xbd\x90\xe5\x9f\xba\xe7\xba\xbf", 12)) return ExposedProperty(CSSPropertyID::kAlignmentBaseline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe4\xb8\x8a\xe5\x9c\x86\xe8\xa7\x92", 12)) return ExposedProperty(CSSPropertyID::kBorderTopLeftRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe4\xb8\x8b\xe5\x9c\x86\xe8\xa7\x92", 12)) return ExposedProperty(CSSPropertyID::kBorderBottomLeftRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kPaddingLeft, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xba\x94\xe7\x94\xa8\xe5\x8c\xba\xe5\x9f\x9f", 12)) return ExposedProperty(CSSPropertyID::kAppRegion, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe5\x9f\xba\xe5\x87\x86", 12)) return ExposedProperty(CSSPropertyID::kFlexBasis, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe5\xa2\x9e\xe9\x95\xbf", 12)) return ExposedProperty(CSSPropertyID::kFlexGrow, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return ExposedProperty(CSSPropertyID::kFlexWrap, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe6\x94\xb6\xe7\xbc\xa9", 12)) return ExposedProperty(CSSPropertyID::kFlexShrink, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe6\x96\xb9\xe5\x90\x91", 12)) return ExposedProperty(CSSPropertyID::kFlexDirection, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbd\xa2\xe7\x8a\xb6\xe6\xb8\xb2\xe6\x9f\x93", 12)) return ExposedProperty(CSSPropertyID::kShapeRendering, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbd\xa2\xe7\x8a\xb6\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kShapeMargin, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8c\x87\xe9\x92\x88\xe4\xba\x8b\xe4\xbb\xb6", 12)) return ExposedProperty(CSSPropertyID::kPointerEvents, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe5\xae\xbd\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kStrokeWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe7\xba\xbf\xe7\xab\xaf", 12)) return ExposedProperty(CSSPropertyID::kStrokeLinecap, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe8\xbf\x9e\xe6\x8e\xa5", 12)) return ExposedProperty(CSSPropertyID::kStrokeLinejoin, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x92\xe5\x80\xbc\xe5\xa4\xa7\xe5\xb0\x8f", 12)) return ExposedProperty(CSSPropertyID::kInterpolateSize, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x94\xbe\xe7\xbd\xae\xe5\x86\x85\xe5\xae\xb9", 12)) return ExposedProperty(CSSPropertyID::kPlaceContent, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x94\xbe\xe7\xbd\xae\xe8\x87\xaa\xe8\xba\xab", 12)) return ExposedProperty(CSSPropertyID::kPlaceSelf, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x94\xbe\xe7\xbd\xae\xe9\xa1\xb9\xe7\x9b\xae", 12)) return ExposedProperty(CSSPropertyID::kPlaceItems, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x95\xb0\xe5\xad\xa6\xe6\xa0\xb7\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kMathStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x95\xb0\xe5\xad\xa6\xe6\xb7\xb1\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kMathDepth, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x95\xb0\xe5\xad\xa6\xe7\xa7\xbb\xe4\xbd\x8d", 12)) return ExposedProperty(CSSPropertyID::kMathShift, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kTextAlign, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83", 12)) return ExposedProperty(CSSPropertyID::kTextEmphasis, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return ExposedProperty(CSSPropertyID::kTextWrap, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\x96\xb9\xe5\x90\x91", 12)) return ExposedProperty(CSSPropertyID::kTextOrientation, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\xb8\xb2\xe6\x9f\x93", 12)) return ExposedProperty(CSSPropertyID::kTextRendering, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\xba\xa2\xe5\x87\xba", 12)) return ExposedProperty(CSSPropertyID::kTextOverflow, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe7\xbc\xa9\xe8\xbf\x9b", 12)) return ExposedProperty(CSSPropertyID::kTextIndent, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0", 12)) return ExposedProperty(CSSPropertyID::kTextDecoration, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xbd\xac\xe6\x8d\xa2", 12)) return ExposedProperty(CSSPropertyID::kTextTransform, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe9\x94\x9a\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kTextAnchor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe9\x97\xb4\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kTextSpacing, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe9\x98\xb4\xe5\xbd\xb1", 12)) return ExposedProperty(CSSPropertyID::kTextShadow, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xa4\xa7\xe5\xae\xbd\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kMaxWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xa4\xa7\xe9\xab\x98\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kMaxHeight, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xb0\x8f\xe5\xae\xbd\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kMinWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xb0\x8f\xe9\xab\x98\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kMinHeight, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x97\xe8\xaf\xbb\xe6\x96\xb9\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kSpeakAs, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\xab\xe7\xab\xaf\xe6\xa0\x87\xe8\xae\xb0", 12)) return ExposedProperty(CSSPropertyID::kMarkerEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\xab\xe8\xa1\x8c\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kTextAlignLast, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xa0\x87\xe9\xa2\x98\xe4\xbd\x8d\xe7\xbd\xae", 12)) return ExposedProperty(CSSPropertyID::kCaptionSide, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xa8\xa1\xe6\x9d\xbf\xe5\x8c\xba\xe5\x9f\x9f", 12)) return ExposedProperty(CSSPropertyID::kGridTemplateAreas, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xa8\xaa\xe5\x90\x91\xe5\x8d\x8a\xe5\xbe\x84", 12)) return ExposedProperty(CSSPropertyID::kRx, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe6\xba\xa2\xe5\x87\xba", 12)) return ExposedProperty(CSSPropertyID::kOverflowX, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb3\x9b\xe5\x85\x89\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kFloodColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb3\xa8\xe9\x9f\xb3\xe4\xbd\x8d\xe7\xbd\xae", 12)) return ExposedProperty(CSSPropertyID::kRubyPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb3\xa8\xe9\x9f\xb3\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kRubyAlign, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb7\xb7\xe5\x90\x88\xe6\xa8\xa1\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kMixBlendMode, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xba\xa2\xe5\x87\xba\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return ExposedProperty(CSSPropertyID::kOverflowWrap, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xba\xa2\xe5\x87\xba\xe9\x94\x9a\xe5\xae\x9a", 12)) return ExposedProperty(CSSPropertyID::kOverflowAnchor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x9d\xa1\xe6\xa7\xbd", 12)) return ExposedProperty(CSSPropertyID::kScrollbarGutter, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xa1\x8c\xe4\xb8\xba", 12)) return ExposedProperty(CSSPropertyID::kScrollBehavior, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xb5\xb7\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xbe\xb9\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kScrollMargin, execution_context, mode);
      if (!memcmp(buffer, "\xe7\x94\xa8\xe6\x88\xb7\xe9\x80\x89\xe6\x8b\xa9", 12)) return ExposedProperty(CSSPropertyID::kUserSelect, execution_context, mode);
      if (!memcmp(buffer, "\xe7\x9f\xa2\xe9\x87\x8f\xe6\x95\x88\xe6\x9e\x9c", 12)) return ExposedProperty(CSSPropertyID::kVectorEffect, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xa9\xba\xe5\x8d\x95\xe5\x85\x83\xe6\xa0\xbc", 12)) return ExposedProperty(CSSPropertyID::kEmptyCells, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xa9\xba\xe7\x99\xbd\xe5\xa4\x84\xe7\x90\x86", 12)) return ExposedProperty(CSSPropertyID::kWhiteSpace, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xa9\xba\xe7\x99\xbd\xe6\x8a\x98\xe5\x8f\xa0", 12)) return ExposedProperty(CSSPropertyID::kWhiteSpaceCollapse, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xb4\xaf\xe5\x8a\xa0\xe7\xac\xa6\xe5\x8f\xb7", 12)) return ExposedProperty(CSSPropertyID::kAdditiveSymbols, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xba\xb5\xe5\x90\x91\xe5\x8d\x8a\xe5\xbe\x84", 12)) return ExposedProperty(CSSPropertyID::kRy, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbb\x98\xe5\x88\xb6\xe9\xa1\xba\xe5\xba\x8f", 12)) return ExposedProperty(CSSPropertyID::kPaintOrder, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbc\x93\xe5\x86\xb2\xe6\xb8\xb2\xe6\x9f\x93", 12)) return ExposedProperty(CSSPropertyID::kBufferedRendering, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe5\x8c\xba\xe5\x9f\x9f", 12)) return ExposedProperty(CSSPropertyID::kGridArea, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe6\xa8\xa1\xe6\x9d\xbf", 12)) return ExposedProperty(CSSPropertyID::kGridTemplate, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe9\x97\xb4\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kAliasGridGap, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe4\xbd\x8d\xe7\xbd\xae", 12)) return ExposedProperty(CSSPropertyID::kBackgroundPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe5\x8e\x9f\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kBackgroundOrigin, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe5\x9b\xbe\xe5\x83\x8f", 12)) return ExposedProperty(CSSPropertyID::kBackgroundImage, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe5\xa4\xa7\xe5\xb0\x8f", 12)) return ExposedProperty(CSSPropertyID::kBackgroundSize, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe6\xbb\xa4\xe9\x95\x9c", 12)) return ExposedProperty(CSSPropertyID::kBackdropFilter, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe8\xa3\x81\xe5\x89\xaa", 12)) return ExposedProperty(CSSPropertyID::kBackgroundClip, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe9\x87\x8d\xe5\xa4\x8d", 12)) return ExposedProperty(CSSPropertyID::kBackgroundRepeat, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe9\x99\x84\xe7\x9d\x80", 12)) return ExposedProperty(CSSPropertyID::kBackgroundAttachment, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x87\xaa\xe5\x8a\xa8\xe6\x8e\x92\xe5\x88\x97", 12)) return ExposedProperty(CSSPropertyID::kGridAutoFlow, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe6\x95\xb0\xe9\x99\x90\xe5\x88\xb6", 12)) return ExposedProperty(CSSPropertyID::kLineClamp, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe8\xb7\x9d\xe8\xa6\x86\xe7\x9b\x96", 12)) return ExposedProperty(CSSPropertyID::kLineGapOverride, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\xa0\xbc\xe5\xb8\x83\xe5\xb1\x80", 12)) return ExposedProperty(CSSPropertyID::kTableLayout, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa3\x81\xe5\x89\xaa\xe8\xa7\x84\xe5\x88\x99", 12)) return ExposedProperty(CSSPropertyID::kClipRule, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa3\x81\xe5\x89\xaa\xe8\xb7\xaf\xe5\xbe\x84", 12)) return ExposedProperty(CSSPropertyID::kClipPath, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa6\x86\xe7\x9b\x96\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kOverrideColors, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x8a\xa8\xe4\xbd\x9c", 12)) return ExposedProperty(CSSPropertyID::kTouchAction, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xb0\x83\xe6\x95\xb4\xe5\xa4\xa7\xe5\xb0\x8f", 12)) return ExposedProperty(CSSPropertyID::kResize, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xb5\xb7\xe5\xa7\x8b\xe6\xa0\x87\xe8\xae\xb0", 12)) return ExposedProperty(CSSPropertyID::kMarkerStart, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xb7\xaf\xe5\xbe\x84\xe6\x95\xb0\xe6\x8d\xae", 12)) return ExposedProperty(CSSPropertyID::kD, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbd\xae\xe5\xbb\x93\xe5\x81\x8f\xe7\xa7\xbb", 12)) return ExposedProperty(CSSPropertyID::kOutlineOffset, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbd\xae\xe5\xbb\x93\xe5\xae\xbd\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kOutlineWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbd\xae\xe5\xbb\x93\xe6\xa0\xb7\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kOutlineStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbd\xae\xe5\xbb\x93\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kOutlineColor, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x90\x88\xe5\xb9\xb6", 12)) return ExposedProperty(CSSPropertyID::kBorderCollapse, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f", 12)) return ExposedProperty(CSSPropertyID::kBorderImage, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 12)) return ExposedProperty(CSSPropertyID::kBorderWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kBorderStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe9\x97\xb4\xe8\xb7\x9d", 12)) return ExposedProperty(CSSPropertyID::kBorderSpacing, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 12)) return ExposedProperty(CSSPropertyID::kBorderColor, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe6\xb8\xa1\xe5\xb1\x9e\xe6\x80\xa7", 12)) return ExposedProperty(CSSPropertyID::kTransitionProperty, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe6\xb8\xa1\xe5\xbb\xb6\xe8\xbf\x9f", 12)) return ExposedProperty(CSSPropertyID::kTransitionDelay, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe6\xb8\xa1\xe6\x97\xb6\xe9\x95\xbf", 12)) return ExposedProperty(CSSPropertyID::kTransitionDuration, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe6\xb8\xa1\xe8\xa1\x8c\xe4\xb8\xba", 12)) return ExposedProperty(CSSPropertyID::kTransitionBehavior, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x80\x8f\xe8\xa7\x86\xe5\x8e\x9f\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kPerspectiveOrigin, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe4\xbd\x8d\xe7\xbd\xae", 12)) return ExposedProperty(CSSPropertyID::kMaskPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe5\x8e\x9f\xe7\x82\xb9", 12)) return ExposedProperty(CSSPropertyID::kMaskOrigin, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe5\x90\x88\xe6\x88\x90", 12)) return ExposedProperty(CSSPropertyID::kMaskComposite, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe5\x9b\xbe\xe5\x83\x8f", 12)) return ExposedProperty(CSSPropertyID::kMaskImage, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe5\xa4\xa7\xe5\xb0\x8f", 12)) return ExposedProperty(CSSPropertyID::kMaskSize, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe6\xa8\xa1\xe5\xbc\x8f", 12)) return ExposedProperty(CSSPropertyID::kMaskMode, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe7\xb1\xbb\xe5\x9e\x8b", 12)) return ExposedProperty(CSSPropertyID::kMaskType, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe8\xa3\x81\xe5\x89\xaa", 12)) return ExposedProperty(CSSPropertyID::kMaskClip, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x81\xae\xe7\xbd\xa9\xe9\x87\x8d\xe5\xa4\x8d", 12)) return ExposedProperty(CSSPropertyID::kMaskRepeat, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x85\x8d\xe8\x89\xb2\xe6\x96\xb9\xe6\xa1\x88", 12)) return ExposedProperty(CSSPropertyID::kColorScheme, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x94\x9a\xe7\x82\xb9\xe5\x90\x8d\xe7\xa7\xb0", 12)) return ExposedProperty(CSSPropertyID::kAnchorName, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe5\x86\x85\xe6\x96\xad\xe9\xa1\xb5", 12)) return ExposedProperty(CSSPropertyID::kPageBreakInside, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe5\x89\x8d\xe6\x96\xad\xe9\xa1\xb5", 12)) return ExposedProperty(CSSPropertyID::kPageBreakBefore, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe5\x90\x8e\xe6\x96\xad\xe9\xa1\xb5", 12)) return ExposedProperty(CSSPropertyID::kPageBreakAfter, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe9\x9d\xa2\xe6\x96\xb9\xe5\x90\x91", 12)) return ExposedProperty(CSSPropertyID::kPageOrientation, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe9\x9d\xa2\xe7\xbc\xa9\xe6\x94\xbe", 12)) return ExposedProperty(CSSPropertyID::kZoom, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa1\xb9\xe7\x9b\xae\xe5\xaf\xb9\xe9\xbd\x90", 12)) return ExposedProperty(CSSPropertyID::kJustifyItems, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa2\x9c\xe8\x89\xb2\xe6\x8f\x92\xe5\x80\xbc", 12)) return ExposedProperty(CSSPropertyID::kColorInterpolation, execution_context, mode);
      if (!memcmp(buffer, "\xe9\xa2\x9c\xe8\x89\xb2\xe6\xb8\xb2\xe6\x9f\x93", 12)) return ExposedProperty(CSSPropertyID::kColorRendering, execution_context, mode);
    } else if (utf8_len == 13) {
      if (!memcmp(buffer, "\x55\x6e\x69\x63\x6f\x64\x65\xe5\x8f\x8c\xe5\x90\x91", 13)) return ExposedProperty(CSSPropertyID::kUnicodeBidi, execution_context, mode);
      if (!memcmp(buffer, "\x55\x6e\x69\x63\x6f\x64\x65\xe8\x8c\x83\xe5\x9b\xb4", 13)) return ExposedProperty(CSSPropertyID::kUnicodeRange, execution_context, mode);
    } else if (utf8_len == 15) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRule, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe8\xb7\xa8\xe8\xa1\x8c", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnSpan, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe9\x97\xb4\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnGap, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7\xe6\xb5\x81", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexFlow, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\xad\xe5\xad\x97\xe7\xac\xa6", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitHyphenateCharacter, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\xb9\xe6\x96\xb9\xe5\x90\x91", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxDirection, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\xb9\xe6\x9c\x9d\xe5\x90\x91", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxOrient, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe5\x88\x86\xe5\xb8\x83", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxPack, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe5\x8f\x8d\xe5\xb0\x84", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxReflect, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe5\xaf\xb9\xe9\xbd\x90", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxAlign, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe5\xbc\xb9\xe6\x80\xa7", 15)) return ExposedProperty(CSSPropertyID::kWebkitBoxFlex, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe6\xa8\xa1\xe5\x9e\x8b", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitBoxSizing, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kAliasWebkitOpacity, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kBorderTopWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 15)) return ExposedProperty(CSSPropertyID::kBorderTopStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 15)) return ExposedProperty(CSSPropertyID::kBorderTopColor, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kBorderBottomWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 15)) return ExposedProperty(CSSPropertyID::kBorderBottomStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 15)) return ExposedProperty(CSSPropertyID::kBorderBottomColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x81\x9c\xe6\xad\xa2\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kStopOpacity, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xae\xb9\xe5\x8f\xaf\xe8\xa7\x81\xe6\x80\xa7", 15)) return ExposedProperty(CSSPropertyID::kContentVisibility, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kPaddingInline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe7\xa9\xba\xe8\xa1\x8c\xe9\xab\x98", 15)) return ExposedProperty(CSSPropertyID::kInternalEmptyLineHeight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe5\xae\xbd\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe6\x96\xad\xe9\xa1\xb5", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleBreak, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe6\xa0\xb7\xe5\xbc\x8f", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe9\xa2\x9c\xe8\x89\xb2", 15)) return ExposedProperty(CSSPropertyID::kColumnRuleColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\xb6\xe8\xa1\xa8\xe7\xac\xa6\xe5\xa4\xa7\xe5\xb0\x8f", 15)) return ExposedProperty(CSSPropertyID::kTabSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf", 15)) return ExposedProperty(CSSPropertyID::kAnimationTimeline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kBorderRightWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 15)) return ExposedProperty(CSSPropertyID::kBorderRightStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 15)) return ExposedProperty(CSSPropertyID::kBorderRightColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kPaddingBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9f\xba\xe7\xa1\x80\xe8\xb0\x83\xe8\x89\xb2\xe6\x9d\xbf", 15)) return ExposedProperty(CSSPropertyID::kBasePalette, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xa1\xab\xe5\x85\x85\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kFillOpacity, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe8\xb0\x83\xe8\x89\xb2\xe6\x9d\xbf", 15)) return ExposedProperty(CSSPropertyID::kFontPalette, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe5\x8f\xaf\xe8\xa7\x81\xe6\x80\xa7", 15)) return ExposedProperty(CSSPropertyID::kPositionVisibility, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xaf\xb9\xe8\xb1\xa1\xe8\xa7\x86\xe5\x9b\xbe\xe6\xa1\x86", 15)) return ExposedProperty(CSSPropertyID::kObjectViewBox, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kBorderLeftWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 15)) return ExposedProperty(CSSPropertyID::kBorderLeftStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 15)) return ExposedProperty(CSSPropertyID::kBorderLeftColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kStrokeOpacity, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\xa1\x86\xe4\xbf\xae\xe5\x89\xaa", 15)) return ExposedProperty(CSSPropertyID::kTextBoxTrim, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\xa1\x86\xe8\xbe\xb9\xe7\xbc\x98", 15)) return ExposedProperty(CSSPropertyID::kTextBoxEdge, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0\xe7\xba\xbf", 15)) return ExposedProperty(CSSPropertyID::kTextDecorationLine, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb3\x9b\xe5\x85\x89\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kFloodOpacity, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe4\xb8\x8a\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginTop, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe4\xb8\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginBottom, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kScrollPadding, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x8f\xb3\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginRight, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\xb7\xa6\xe8\xbe\xb9\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kScrollMarginLeft, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf", 15)) return ExposedProperty(CSSPropertyID::kScrollTimeline, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x9d\xa1\xe5\xae\xbd\xe5\xba\xa6", 15)) return ExposedProperty(CSSPropertyID::kScrollbarWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x9d\xa1\xe9\xa2\x9c\xe8\x89\xb2", 15)) return ExposedProperty(CSSPropertyID::kScrollbarColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\xa0\x87\xe8\xae\xb0\xe7\xbb\x84", 15)) return ExposedProperty(CSSPropertyID::kScrollMarkerGroup, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe5\x88\x97\xe9\x97\xb4\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kAliasGridColumnGap, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc\xe8\xa1\x8c\xe9\x97\xb4\xe8\xb7\x9d", 15)) return ExposedProperty(CSSPropertyID::kAliasGridRowGap, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe9\x9d\xa2\xe5\x8f\xaf\xe8\xa7\x81\xe6\x80\xa7", 15)) return ExposedProperty(CSSPropertyID::kBackfaceVisibility, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf", 15)) return ExposedProperty(CSSPropertyID::kViewTimeline, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe8\xbf\x87\xe6\xb8\xa1\xe7\xb1\xbb", 15)) return ExposedProperty(CSSPropertyID::kViewTransitionClass, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe8\xbf\x87\xe6\xb8\xa1\xe7\xbb\x84", 15)) return ExposedProperty(CSSPropertyID::kViewTransitionGroup, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xae\xa1\xe6\x95\xb0\xe5\x99\xa8\xe8\xae\xbe\xe7\xbd\xae", 15)) return ExposedProperty(CSSPropertyID::kCounterSet, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xae\xa1\xe6\x95\xb0\xe5\x99\xa8\xe9\x80\x92\xe5\xa2\x9e", 15)) return ExposedProperty(CSSPropertyID::kCounterIncrement, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xae\xa1\xe6\x95\xb0\xe5\x99\xa8\xe9\x87\x8d\xe7\xbd\xae", 15)) return ExposedProperty(CSSPropertyID::kCounterReset, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe6\xba\x90", 15)) return ExposedProperty(CSSPropertyID::kBorderImageSource, execution_context, mode);
      if (!memcmp(buffer, "\xe9\x94\x9a\xe7\x82\xb9\xe4\xbd\x9c\xe7\x94\xa8\xe5\x9f\x9f", 15)) return ExposedProperty(CSSPropertyID::kAnchorScope, execution_context, mode);
    } else if (utf8_len == 16) {
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe4\xb9\xa6\xe5\x86\x99\xe6\xa8\xa1\xe5\xbc\x8f", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubWritingMode, execution_context, mode);
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\x87\xe6\x9c\xac\xe5\x90\x88\xe5\xb9\xb6", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextCombine, execution_context, mode);
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextEmphasis, execution_context, mode);
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\x87\xe6\x9c\xac\xe6\x96\xb9\xe5\x90\x91", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextOrientation, execution_context, mode);
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\x87\xe6\x9c\xac\xe8\xbd\xac\xe6\x8d\xa2", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubTextTransform, execution_context, mode);
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\xa0\x87\xe9\xa2\x98\xe4\xbd\x8d\xe7\xbd\xae", 16)) return ExposedProperty(CSSPropertyID::kAliasEpubCaptionSide, execution_context, mode);
    } else if (utf8_len == 18) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe4\xb8\xbb\xe8\xbd\xb4\xe5\xaf\xb9\xe9\xbd\x90", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitJustifyContent, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe4\xb9\xa6\xe5\x86\x99\xe6\xa8\xa1\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kWebkitWritingMode, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x86\x85\xe5\xae\xb9\xe5\xaf\xb9\xe9\xbd\x90", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAlignContent, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe5\x86\x85\xe6\x96\xad\xe9\xa1\xb5", 18)) return ExposedProperty(CSSPropertyID::kWebkitColumnBreakInside, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe5\x89\x8d\xe6\x96\xad\xe9\xa1\xb5", 18)) return ExposedProperty(CSSPropertyID::kWebkitColumnBreakBefore, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe5\x90\x8e\xe6\x96\xad\xe9\xa1\xb5", 18)) return ExposedProperty(CSSPropertyID::kWebkitColumnBreakAfter, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x89\x8d\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBefore, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x89\x8d\xe7\xbd\xae\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginBefore, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe5\x90\x8d\xe7\xa7\xb0", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationName, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe5\xbb\xb6\xe8\xbf\x9f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationDelay, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe6\x96\xb9\xe5\x90\x91", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationDirection, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe6\x97\xb6\xe9\x95\xbf", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationDuration, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8c\xba\xe5\x9f\x9f\xe8\xae\xbe\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kWebkitLocale, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\x98\xe6\x8d\xa2\xe5\x8e\x9f\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransformOrigin, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\x98\xe6\x8d\xa2\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransformStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\xb3\xe4\xb8\x8a\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderTopRightRadius, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\xb3\xe4\xb8\x8b\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBottomRightRadius, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x90\x8e\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfter, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x90\x8e\xe7\xbd\xae\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginAfter, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xa4\x96\xe9\x83\xa8\xe5\xbd\xa2\xe7\x8a\xb6", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitShapeOutside, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xad\x97\xe4\xbd\x93\xe5\xb9\xb3\xe6\xbb\x91", 18)) return ExposedProperty(CSSPropertyID::kWebkitFontSmoothing, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xb7\xa6\xe4\xb8\x8a\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderTopLeftRadius, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xb7\xa6\xe4\xb8\x8b\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBottomLeftRadius, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xba\x94\xe7\x94\xa8\xe5\x8c\xba\xe5\x9f\x9f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAppRegion, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7\xe5\x9f\xba\xe5\x87\x86", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexBasis, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7\xe5\xa2\x9e\xe9\x95\xbf", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexGrow, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7\xe6\x8d\xa2\xe8\xa1\x8c", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexWrap, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7\xe6\x94\xb6\xe7\xbc\xa9", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexShrink, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7\xe6\x96\xb9\xe5\x90\x91", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitFlexDirection, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbd\xa2\xe7\x8a\xb6\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitShapeMargin, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\x90\x88\xe5\xb9\xb6", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextCombine, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xae\x89\xe5\x85\xa8", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextSecurity, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasis, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe6\x8f\x8f\xe8\xbe\xb9", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextStroke, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe6\x96\xb9\xe5\x90\x91", 18)) return ExposedProperty(CSSPropertyID::kWebkitTextOrientation, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\xb3\xa8\xe9\x9f\xb3\xe4\xbd\x8d\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kWebkitRubyPosition, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x94\xa8\xe6\x88\xb7\xe4\xbf\xae\xe6\x94\xb9", 18)) return ExposedProperty(CSSPropertyID::kWebkitUserModify, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x94\xa8\xe6\x88\xb7\xe6\x8b\x96\xe6\x8b\xbd", 18)) return ExposedProperty(CSSPropertyID::kWebkitUserDrag, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x94\xa8\xe6\x88\xb7\xe9\x80\x89\xe6\x8b\xa9", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitUserSelect, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe6\x8e\x92\xe5\xba\x8f\xe7\xbb\x84", 18)) return ExposedProperty(CSSPropertyID::kWebkitBoxOrdinalGroup, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEnd, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginEnd, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\x83\x8c\xe6\x99\xaf\xe5\x8e\x9f\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackgroundOrigin, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\x83\x8c\xe6\x99\xaf\xe5\xa4\xa7\xe5\xb0\x8f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackgroundSize, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\x83\x8c\xe6\x99\xaf\xe8\xa3\x81\xe5\x89\xaa", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackgroundClip, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\x87\xaa\xe8\xba\xab\xe5\xaf\xb9\xe9\xbd\x90", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitAlignSelf, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xa1\x8c\xe6\x95\xb0\xe9\x99\x90\xe5\x88\xb6", 18)) return ExposedProperty(CSSPropertyID::kWebkitLineClamp, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xa3\x81\xe5\x89\xaa\xe8\xb7\xaf\xe5\xbe\x84", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitClipPath, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStart, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMarginStart, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f", 18)) return ExposedProperty(CSSPropertyID::kWebkitBorderImage, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xbf\x87\xe6\xb8\xa1\xe5\xb1\x9e\xe6\x80\xa7", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionProperty, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xbf\x87\xe6\xb8\xa1\xe5\xbb\xb6\xe8\xbf\x9f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionDelay, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xbf\x87\xe6\xb8\xa1\xe6\x97\xb6\xe9\x95\xbf", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionDuration, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\x8f\xe8\xa7\x86\xe5\x8e\x9f\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitPerspectiveOrigin, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\xbb\xe8\xbe\x91\xe5\xae\xbd\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitLogicalWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\xbb\xe8\xbe\x91\xe9\xab\x98\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitLogicalHeight, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe4\xbd\x8d\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskPosition, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe5\x8e\x9f\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskOrigin, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe5\x90\x88\xe6\x88\x90", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskComposite, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe5\x9b\xbe\xe5\x83\x8f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskImage, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe5\xa4\xa7\xe5\xb0\x8f", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskSize, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe8\xa3\x81\xe5\x89\xaa", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskClip, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe9\x87\x8d\xe5\xa4\x8d", 18)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaskRepeat, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xb8\xbb\xe8\xbd\xb4\xe8\x87\xaa\xe8\xba\xab\xe5\xaf\xb9\xe9\xbd\x90", 18)) return ExposedProperty(CSSPropertyID::kJustifySelf, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xb5\xb7\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe5\xb5\x8c\xe5\x85\xa5", 18)) return ExposedProperty(CSSPropertyID::kInsetInlineEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kMarginInlineEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe5\xb5\x8c\xe5\x85\xa5", 18)) return ExposedProperty(CSSPropertyID::kInsetInlineStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kMarginInlineStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 18)) return ExposedProperty(CSSPropertyID::kBorderInlineColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\xbc\xba\xe5\x88\xb6\xe9\xa2\x9c\xe8\x89\xb2", 18)) return ExposedProperty(CSSPropertyID::kInternalForcedColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa1\xa8\xe6\xa0\xb7\xe5\xbc\x8f\xe4\xbd\x8d\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kListStylePosition, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa1\xa8\xe6\xa0\xb7\xe5\xbc\x8f\xe5\x9b\xbe\xe5\x83\x8f", 18)) return ExposedProperty(CSSPropertyID::kListStyleImage, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa1\xa8\xe6\xa0\xb7\xe5\xbc\x8f\xe7\xb1\xbb\xe5\x9e\x8b", 18)) return ExposedProperty(CSSPropertyID::kListStyleType, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe6\x80\x81\xe8\x8c\x83\xe5\x9b\xb4\xe9\x99\x90\xe5\x88\xb6", 18)) return ExposedProperty(CSSPropertyID::kDynamicRangeLimit, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe5\xa1\xab\xe5\x85\x85\xe6\xa8\xa1\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kAnimationFillMode, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe6\x92\xad\xe6\x94\xbe\xe7\x8a\xb6\xe6\x80\x81", 18)) return ExposedProperty(CSSPropertyID::kAnimationPlayState, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe6\x97\xb6\xe9\x97\xb4\xe5\x87\xbd\xe6\x95\xb0", 18)) return ExposedProperty(CSSPropertyID::kAnimationTimingFunction, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe8\x8c\x83\xe5\x9b\xb4\xe7\xbb\x88\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kAnimationRangeEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe8\x8c\x83\xe5\x9b\xb4\xe8\xb5\xb7\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kAnimationRangeStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x8a\xa8\xe7\x94\xbb\xe8\xbf\xad\xe4\xbb\xa3\xe6\xac\xa1\xe6\x95\xb0", 18)) return ExposedProperty(CSSPropertyID::kAnimationIterationCount, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xba\xe6\x9c\x89\xe5\x8c\x85\xe5\x90\xab\xe5\xae\xbd\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kContainIntrinsicWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xba\xe6\x9c\x89\xe5\x8c\x85\xe5\x90\xab\xe5\xb0\xba\xe5\xaf\xb8", 18)) return ExposedProperty(CSSPropertyID::kContainIntrinsicSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xba\xe6\x9c\x89\xe5\x8c\x85\xe5\x90\xab\xe9\xab\x98\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kContainIntrinsicHeight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xb5\xb7\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe5\xb5\x8c\xe5\x85\xa5", 18)) return ExposedProperty(CSSPropertyID::kInsetBlockEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kMarginBlockEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe5\xb5\x8c\xe5\x85\xa5", 18)) return ExposedProperty(CSSPropertyID::kInsetBlockStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kMarginBlockStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 18)) return ExposedProperty(CSSPropertyID::kBorderBlockColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xb5\xb7\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe4\xb8\x9c\xe4\xba\x9a\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantEastAsian, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe4\xbd\x8d\xe7\xbd\xae\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x85\x89\xe5\xad\xa6\xe5\xa4\xa7\xe5\xb0\x8f", 18)) return ExposedProperty(CSSPropertyID::kFontOpticalSizing, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x8f\x98\xe5\x8c\x96\xe8\xae\xbe\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kFontVariationSettings, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x90\x88\xe6\x88\x90\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kFontSynthesisStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x90\x88\xe6\x88\x90\xe7\xb2\x97\xe7\xbb\x86", 18)) return ExposedProperty(CSSPropertyID::kFontSynthesisWeight, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\x86\x99\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantCaps, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f\xe8\xb0\x83\xe6\x95\xb4", 18)) return ExposedProperty(CSSPropertyID::kFontSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe6\x95\xb0\xe5\xad\x97\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantNumeric, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe6\x9b\xbf\xe4\xbb\xa3\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantAlternates, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe7\x89\xb9\xe6\x80\xa7\xe8\xae\xbe\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kFontFeatureSettings, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe8\xa1\xa8\xe6\x83\x85\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantEmoji, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe8\xbf\x9e\xe5\xad\x97\xe5\x8f\x98\xe4\xbd\x93", 18)) return ExposedProperty(CSSPropertyID::kFontVariantLigatures, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe5\xb0\x9d\xe8\xaf\x95\xe5\x9b\x9e\xe9\x80\x80", 18)) return ExposedProperty(CSSPropertyID::kPositionTryFallbacks, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe5\xb0\x9d\xe8\xaf\x95\xe9\xa1\xba\xe5\xba\x8f", 18)) return ExposedProperty(CSSPropertyID::kPositionTryOrder, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbc\xba\xe5\x88\xb6\xe9\xa2\x9c\xe8\x89\xb2\xe8\xb0\x83\xe6\x95\xb4", 18)) return ExposedProperty(CSSPropertyID::kForcedColorAdjust, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xbd\xa2\xe7\x8a\xb6\xe5\x9b\xbe\xe5\x83\x8f\xe9\x98\x88\xe5\x80\xbc", 18)) return ExposedProperty(CSSPropertyID::kShapeImageThreshold, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe6\x96\x9c\xe6\x8e\xa5\xe9\x99\x90\xe5\x88\xb6", 18)) return ExposedProperty(CSSPropertyID::kStrokeMiterlimit, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe8\x99\x9a\xe7\xba\xbf\xe5\x81\x8f\xe7\xa7\xbb", 18)) return ExposedProperty(CSSPropertyID::kStrokeDashoffset, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe8\x99\x9a\xe7\xba\xbf\xe9\x98\xb5\xe5\x88\x97", 18)) return ExposedProperty(CSSPropertyID::kStrokeDasharray, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xa4\xa7\xe5\xb0\x8f\xe8\xb0\x83\xe6\x95\xb4", 18)) return ExposedProperty(CSSPropertyID::kTextSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe4\xbd\x8d\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kTextEmphasisPosition, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kTextEmphasisStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe9\xa2\x9c\xe8\x89\xb2", 18)) return ExposedProperty(CSSPropertyID::kTextEmphasisColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\x8d\xa2\xe8\xa1\x8c\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kTextWrapStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe6\x8d\xa2\xe8\xa1\x8c\xe6\xa8\xa1\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kTextWrapMode, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe7\xab\x96\xe6\x8e\x92\xe5\x90\x88\xe5\xb9\xb6", 18)) return ExposedProperty(CSSPropertyID::kTextCombineUpright, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\x87\xaa\xe5\x8a\xa8\xe9\x97\xb4\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kTextAutospace, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0\xe6\xa0\xb7\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kTextDecorationStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0\xe7\xb2\x97\xe7\xbb\x86", 18)) return ExposedProperty(CSSPropertyID::kTextDecorationThickness, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0\xe9\xa2\x9c\xe8\x89\xb2", 18)) return ExposedProperty(CSSPropertyID::kTextDecorationColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe9\x97\xb4\xe8\xb7\x9d\xe4\xbf\xae\xe5\x89\xaa", 18)) return ExposedProperty(CSSPropertyID::kTextSpacingTrim, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\xad\xe5\xad\x97\xe7\xac\xa6\xe6\x95\xb0\xe9\x99\x90\xe5\x88\xb6", 18)) return ExposedProperty(CSSPropertyID::kHyphenateLimitChars, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf\xe4\xbd\x9c\xe7\x94\xa8\xe5\x9f\x9f", 18)) return ExposedProperty(CSSPropertyID::kTimelineScope, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xa4\xa7\xe5\x86\x85\xe8\x81\x94\xe5\xb0\xba\xe5\xaf\xb8", 18)) return ExposedProperty(CSSPropertyID::kMaxInlineSize, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xa4\xa7\xe5\x9d\x97\xe7\xba\xa7\xe5\xb0\xba\xe5\xaf\xb8", 18)) return ExposedProperty(CSSPropertyID::kMaxBlockSize, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xb0\x8f\xe5\x86\x85\xe8\x81\x94\xe5\xb0\xba\xe5\xaf\xb8", 18)) return ExposedProperty(CSSPropertyID::kMinInlineSize, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xb0\x8f\xe5\x9d\x97\xe7\xba\xa7\xe5\xb0\xba\xe5\xaf\xb8", 18)) return ExposedProperty(CSSPropertyID::kMinBlockSize, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xb5\xb7\xe7\x82\xb9", 18)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xba\xa2\xe5\x87\xba\xe8\xa3\x81\xe5\x89\xaa\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kOverflowClipMargin, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe4\xb8\x8a\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingTop, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe4\xb8\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingBottom, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\x81\x94\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kScrollMarginInline, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x88\x9d\xe5\xa7\x8b\xe7\x9b\xae\xe6\xa0\x87", 18)) return ExposedProperty(CSSPropertyID::kScrollInitialTarget, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x8f\xb3\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingRight, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x90\xb8\xe9\x99\x84\xe5\x81\x9c\xe6\xad\xa2", 18)) return ExposedProperty(CSSPropertyID::kScrollSnapStop, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x90\xb8\xe9\x99\x84\xe5\xaf\xb9\xe9\xbd\x90", 18)) return ExposedProperty(CSSPropertyID::kScrollSnapAlign, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x90\xb8\xe9\x99\x84\xe7\xb1\xbb\xe5\x9e\x8b", 18)) return ExposedProperty(CSSPropertyID::kScrollSnapType, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x9d\x97\xe7\xba\xa7\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kScrollMarginBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\xb7\xa6\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 18)) return ExposedProperty(CSSPropertyID::kScrollPaddingLeft, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf\xe8\xbd\xb4", 18)) return ExposedProperty(CSSPropertyID::kScrollTimelineAxis, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\xa4\xe9\x95\x9c\xe9\xa2\x9c\xe8\x89\xb2\xe6\x8f\x92\xe5\x80\xbc", 18)) return ExposedProperty(CSSPropertyID::kColorInterpolationFilters, execution_context, mode);
      if (!memcmp(buffer, "\xe7\x9b\x92\xe5\xad\x90\xe8\xa3\x85\xe9\xa5\xb0\xe6\x96\xad\xe8\xa1\x8c", 18)) return ExposedProperty(CSSPropertyID::kBoxDecorationBreak, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbb\x93\xe6\x9d\x9f\xe7\xbb\x93\xe6\x9d\x9f\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kBorderEndEndRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe7\xbb\x93\xe6\x9d\x9f\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kBorderEndStartRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe5\x9e\x82\xe7\x9b\xb4\xe4\xbd\x8d\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kBackgroundPositionY, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe6\xb0\xb4\xe5\xb9\xb3\xe4\xbd\x8d\xe7\xbd\xae", 18)) return ExposedProperty(CSSPropertyID::kBackgroundPositionX, execution_context, mode);
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf\xe6\xb7\xb7\xe5\x90\x88\xe6\xa8\xa1\xe5\xbc\x8f", 18)) return ExposedProperty(CSSPropertyID::kBackgroundBlendMode, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf\xe8\xbd\xb4", 18)) return ExposedProperty(CSSPropertyID::kViewTimelineAxis, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe8\xbf\x87\xe6\xb8\xa1\xe5\x90\x8d\xe7\xa7\xb0", 18)) return ExposedProperty(CSSPropertyID::kViewTransitionName, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xb5\xb7\xe5\xa7\x8b\xe7\xbb\x93\xe6\x9d\x9f\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kBorderStartEndRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x9c\x86\xe8\xa7\x92", 18)) return ExposedProperty(CSSPropertyID::kBorderStartStartRadius, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe5\x88\x87\xe7\x89\x87", 18)) return ExposedProperty(CSSPropertyID::kBorderImageSlice, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe5\xa4\x96\xe6\x89\xa9", 18)) return ExposedProperty(CSSPropertyID::kBorderImageOutset, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe5\xae\xbd\xe5\xba\xa6", 18)) return ExposedProperty(CSSPropertyID::kBorderImageWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe9\x87\x8d\xe5\xa4\x8d", 18)) return ExposedProperty(CSSPropertyID::kBorderImageRepeat, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe5\xba\xa6\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xa1\x8c\xe4\xb8\xba", 18)) return ExposedProperty(CSSPropertyID::kOverscrollBehavior, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xbf\x87\xe6\xb8\xa1\xe6\x97\xb6\xe9\x97\xb4\xe5\x87\xbd\xe6\x95\xb0", 18)) return ExposedProperty(CSSPropertyID::kTransitionTimingFunction, execution_context, mode);
    } else if (utf8_len == 19) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\x98\xe6\x8d\xa2\xe5\x8e\x9f\xe7\x82\xb9\x58", 19)) return ExposedProperty(CSSPropertyID::kWebkitTransformOriginX, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\x98\xe6\x8d\xa2\xe5\x8e\x9f\xe7\x82\xb9\x59", 19)) return ExposedProperty(CSSPropertyID::kWebkitTransformOriginY, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8f\x98\xe6\x8d\xa2\xe5\x8e\x9f\xe7\x82\xb9\x5a", 19)) return ExposedProperty(CSSPropertyID::kWebkitTransformOriginZ, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\x8f\xe8\xa7\x86\xe5\x8e\x9f\xe7\x82\xb9\x58", 19)) return ExposedProperty(CSSPropertyID::kWebkitPerspectiveOriginX, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x80\x8f\xe8\xa7\x86\xe5\x8e\x9f\xe7\x82\xb9\x59", 19)) return ExposedProperty(CSSPropertyID::kWebkitPerspectiveOriginY, execution_context, mode);
    } else if (utf8_len == 21) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe5\xae\xbd\xe5\xba\xa6", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRuleWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe6\xa0\xb7\xe5\xbc\x8f", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRuleStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe9\xa2\x9c\xe8\x89\xb2", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitColumnRuleColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x89\x8d\xe7\xbd\xae\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingBefore, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x90\x8e\xe7\xbd\xae\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingAfter, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\xbb\x93\xe6\x9d\x9f\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingEnd, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\x83\x8c\xe9\x9d\xa2\xe5\x8f\xaf\xe8\xa7\x81\xe6\x80\xa7", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitBackfaceVisibility, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kAliasWebkitPaddingStart, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f", 21)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImage, execution_context, mode);
      if (!memcmp(buffer, "\xe4\xba\xa4\xe5\x8f\x89\xe8\xbd\xb4\xe8\x87\xaa\xe8\xba\xab\xe5\xaf\xb9\xe9\xbd\x90", 21)) return ExposedProperty(CSSPropertyID::kAlignSelf, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kPaddingInlineEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kPaddingInlineStart, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\xa1\xab\xe5\x85\x85", 21)) return ExposedProperty(CSSPropertyID::kInternalVisitedFill, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe6\x8f\x8f\xe8\xbe\xb9", 21)) return ExposedProperty(CSSPropertyID::kInternalVisitedStroke, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe9\xa2\x9c\xe8\x89\xb2", 21)) return ExposedProperty(CSSPropertyID::kInternalVisitedColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kPaddingBlockEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kPaddingBlockStart, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe4\xb8\x8b\xe5\x88\x92\xe7\xba\xbf\xe4\xbd\x8d\xe7\xbd\xae", 21)) return ExposedProperty(CSSPropertyID::kTextUnderlinePosition, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe4\xb8\x8b\xe5\x88\x92\xe7\xba\xbf\xe5\x81\x8f\xe7\xa7\xbb", 21)) return ExposedProperty(CSSPropertyID::kTextUnderlineOffset, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xba\x90\xe8\xaf\x95\xe7\x94\xa8\xe6\xb5\x8b\xe8\xaf\x95\xe5\xb1\x9e\xe6\x80\xa7", 21)) return ExposedProperty(CSSPropertyID::kOriginTrialTestProperty, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\x81\x94\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kScrollPaddingInline, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x9d\x97\xe7\xba\xa7\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 21)) return ExposedProperty(CSSPropertyID::kScrollPaddingBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf\xe5\x90\x8d\xe7\xa7\xb0", 21)) return ExposedProperty(CSSPropertyID::kScrollTimelineName, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf\xe5\x90\x8d\xe7\xa7\xb0", 21)) return ExposedProperty(CSSPropertyID::kViewTimelineName, execution_context, mode);
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe6\x97\xb6\xe9\x97\xb4\xe7\xba\xbf\xe5\xb5\x8c\xe5\x85\xa5", 21)) return ExposedProperty(CSSPropertyID::kViewTimelineInset, execution_context, mode);
    } else if (utf8_len == 22) {
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe6\xa0\xb7\xe5\xbc\x8f", 22)) return ExposedProperty(CSSPropertyID::kAliasEpubTextEmphasisStyle, execution_context, mode);
      if (!memcmp(buffer, "\x45\x50\x55\x42\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe9\xa2\x9c\xe8\x89\xb2", 22)) return ExposedProperty(CSSPropertyID::kAliasEpubTextEmphasisColor, execution_context, mode);
    } else if (utf8_len == 24) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe4\xbb\x8e\xe5\x8f\xb3\xe5\x88\xb0\xe5\xb7\xa6\xe6\x8e\x92\xe5\xba\x8f", 24)) return ExposedProperty(CSSPropertyID::kWebkitRtlOrdering, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x89\x8d\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBeforeWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x89\x8d\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBeforeStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x89\x8d\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderBeforeColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe5\xa1\xab\xe5\x85\x85\xe6\xa8\xa1\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationFillMode, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe6\x92\xad\xe6\x94\xbe\xe7\x8a\xb6\xe6\x80\x81", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationPlayState, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe6\x97\xb6\xe9\x97\xb4\xe5\x87\xbd\xe6\x95\xb0", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationTimingFunction, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x8a\xa8\xe7\x94\xbb\xe8\xbf\xad\xe4\xbb\xa3\xe6\xac\xa1\xe6\x95\xb0", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitAnimationIterationCount, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x90\x8e\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfterWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x90\x8e\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfterStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x90\x8e\xe7\xbd\xae\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderAfterColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\x9e\x82\xe7\x9b\xb4\xe8\xbe\xb9\xe6\xa1\x86\xe9\x97\xb4\xe8\xb7\x9d", 24)) return ExposedProperty(CSSPropertyID::kWebkitBorderVerticalSpacing, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xad\x97\xe4\xbd\x93\xe7\x89\xb9\xe6\x80\xa7\xe8\xae\xbe\xe7\xbd\xae", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitFontFeatureSettings, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbd\xa2\xe7\x8a\xb6\xe5\x9b\xbe\xe5\x83\x8f\xe9\x98\x88\xe5\x80\xbc", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitShapeImageThreshold, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xa1\xab\xe5\x85\x85\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextFillColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xa4\xa7\xe5\xb0\x8f\xe8\xb0\x83\xe6\x95\xb4", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextSizeAdjust, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe4\xbd\x8d\xe7\xbd\xae", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasisPosition, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasisStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTextEmphasisColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe6\x8f\x8f\xe8\xbe\xb9\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextStrokeWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x96\x87\xe6\x9c\xac\xe6\x8f\x8f\xe8\xbe\xb9\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextStrokeColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x9c\x80\xe5\xa4\xa7\xe9\x80\xbb\xe8\xbe\x91\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaxLogicalWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x9c\x80\xe5\xa4\xa7\xe9\x80\xbb\xe8\xbe\x91\xe9\xab\x98\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMaxLogicalHeight, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x9c\x80\xe5\xb0\x8f\xe9\x80\xbb\xe8\xbe\x91\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMinLogicalWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\x9c\x80\xe5\xb0\x8f\xe9\x80\xbb\xe8\xbe\x91\xe9\xab\x98\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitMinLogicalHeight, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe6\xb0\xb4\xe5\xb9\xb3\xe8\xbe\xb9\xe6\xa1\x86\xe9\x97\xb4\xe8\xb7\x9d", 24)) return ExposedProperty(CSSPropertyID::kWebkitBorderHorizontalSpacing, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x82\xb9\xe5\x87\xbb\xe9\xab\x98\xe4\xba\xae\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kWebkitTapHighlightColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x94\x9f\xe6\x95\x88\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0", 24)) return ExposedProperty(CSSPropertyID::kWebkitTextDecorationsInEffect, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\x9b\x92\xe5\xad\x90\xe8\xa3\x85\xe9\xa5\xb0\xe6\x96\xad\xe8\xa1\x8c", 24)) return ExposedProperty(CSSPropertyID::kWebkitBoxDecorationBreak, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEndWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEndStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderEndColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStartWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStartStyle, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitBorderStartColor, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe8\xbf\x87\xe6\xb8\xa1\xe6\x97\xb6\xe9\x97\xb4\xe5\x87\xbd\xe6\x95\xb0", 24)) return ExposedProperty(CSSPropertyID::kAliasWebkitTransitionTimingFunction, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe5\x9e\x82\xe7\x9b\xb4\xe4\xbd\x8d\xe7\xbd\xae", 24)) return ExposedProperty(CSSPropertyID::kWebkitMaskPositionY, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe6\xba\x90", 24)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageSource, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xb0\xb4\xe5\xb9\xb3\xe4\xbd\x8d\xe7\xbd\xae", 24)) return ExposedProperty(CSSPropertyID::kWebkitMaskPositionX, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineEndWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineEndStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineEndColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineStartWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineStartStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kBorderInlineStartColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xbf\x87\xe5\xba\xa6\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xa1\x8c\xe4\xb8\xba", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorInline, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\x9d\x97\xe7\xba\xa7\xe5\x86\x85\xe5\xae\xb9\xe5\xaf\xb9\xe9\xbd\x90", 24)) return ExposedProperty(CSSPropertyID::kInternalAlignContentBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\xad\x97\xe4\xbd\x93\xe5\xa4\xa7\xe5\xb0\x8f\xe5\xa2\x9e\xe9\x87\x8f", 24)) return ExposedProperty(CSSPropertyID::kInternalFontSizeDelta, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\xbc\xba\xe5\x88\xb6\xe8\x83\x8c\xe6\x99\xaf\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kInternalForcedBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\xbc\xba\xe5\x88\xb6\xe8\xbd\xae\xe5\xbb\x93\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kInternalForcedOutlineColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\xbc\xba\xe5\x88\xb6\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kInternalForcedBorderColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xba\xe6\x9c\x89\xe5\x8c\x85\xe5\x90\xab\xe5\x86\x85\xe8\x81\x94\xe5\xb0\xba\xe5\xaf\xb8", 24)) return ExposedProperty(CSSPropertyID::kContainIntrinsicInlineSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9b\xba\xe6\x9c\x89\xe5\x8c\x85\xe5\x90\xab\xe5\x9d\x97\xe7\xba\xa7\xe5\xb0\xba\xe5\xaf\xb8", 24)) return ExposedProperty(CSSPropertyID::kContainIntrinsicBlockSize, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockEndWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockEndStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockEndColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockStartWidth, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe6\xa0\xb7\xe5\xbc\x8f", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockStartStyle, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 24)) return ExposedProperty(CSSPropertyID::kBorderBlockStartColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe8\xbf\x87\xe5\xba\xa6\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xa1\x8c\xe4\xb8\xba", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorBlock, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe8\xbf\x87\xe5\xba\xa6\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xa1\x8c\xe4\xb8\xba", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorY, execution_context, mode);
      if (!memcmp(buffer, "\xe5\xad\x97\xe4\xbd\x93\xe5\x90\x88\xe6\x88\x90\xe5\xb0\x8f\xe5\x9e\x8b\xe5\xa4\xa7\xe5\x86\x99", 24)) return ExposedProperty(CSSPropertyID::kFontSynthesisSmallCaps, execution_context, mode);
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0\xe9\x81\xbf\xe8\xae\xa9\xe7\xac\x94\xe7\x94\xbb", 24)) return ExposedProperty(CSSPropertyID::kTextDecorationSkipInk, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe8\xbf\x87\xe5\xba\xa6\xe6\xbb\x9a\xe5\x8a\xa8\xe8\xa1\x8c\xe4\xb8\xba", 24)) return ExposedProperty(CSSPropertyID::kOverscrollBehaviorX, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe8\xb7\x9d", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginInlineEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginInlineStart, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe8\xb7\x9d", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginBlockEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe8\xb7\x9d", 24)) return ExposedProperty(CSSPropertyID::kScrollMarginBlockStart, execution_context, mode);
    } else if (utf8_len == 27) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe5\x88\x87\xe7\x89\x87", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageSlice, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe5\xa4\x96\xe6\x89\xa9", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageOutset, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe5\xae\xbd\xe5\xba\xa6", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageWidth, execution_context, mode);
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe9\x81\xae\xe7\xbd\xa9\xe6\xa1\x86\xe5\x9b\xbe\xe5\x83\x8f\xe9\x87\x8d\xe5\xa4\x8d", 27)) return ExposedProperty(CSSPropertyID::kWebkitMaskBoxImageRepeat, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe5\xbc\xba\xe5\x88\xb6\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe9\xa2\x9c\xe8\x89\xb2", 27)) return ExposedProperty(CSSPropertyID::kInternalForcedVisitedColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x85\x89\xe6\xa0\x87\xe9\xa2\x9c\xe8\x89\xb2", 27)) return ExposedProperty(CSSPropertyID::kInternalVisitedCaretColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe8\x83\x8c\xe6\x99\xaf\xe9\xa2\x9c\xe8\x89\xb2", 27)) return ExposedProperty(CSSPropertyID::kInternalVisitedBackgroundColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe8\xbd\xae\xe5\xbb\x93\xe9\xa2\x9c\xe8\x89\xb2", 27)) return ExposedProperty(CSSPropertyID::kInternalVisitedOutlineColor, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingInlineEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingInlineStart, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingBlockEnd, execution_context, mode);
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 27)) return ExposedProperty(CSSPropertyID::kScrollPaddingBlockStart, execution_context, mode);
    } else if (utf8_len == 30) {
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe4\xb8\x8a\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderTopColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe4\xb8\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderBottomColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x88\x97\xe8\xa7\x84\xe5\x88\x99\xe9\xa2\x9c\xe8\x89\xb2", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedColumnRuleColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x8f\xb3\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderRightColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\xb7\xa6\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 30)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderLeftColor, execution_context, mode);
    } else if (utf8_len == 33) {
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe6\x96\x87\xe6\x9c\xac\xe5\xa1\xab\xe5\x85\x85\xe9\xa2\x9c\xe8\x89\xb2", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextFillColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe6\x96\x87\xe6\x9c\xac\xe5\xbc\xba\xe8\xb0\x83\xe9\xa2\x9c\xe8\x89\xb2", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextEmphasisColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe6\x96\x87\xe6\x9c\xac\xe6\x8f\x8f\xe8\xbe\xb9\xe9\xa2\x9c\xe8\x89\xb2", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextStrokeColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe6\x96\x87\xe6\x9c\xac\xe8\xa3\x85\xe9\xa5\xb0\xe9\xa2\x9c\xe8\x89\xb2", 33)) return ExposedProperty(CSSPropertyID::kInternalVisitedTextDecorationColor, execution_context, mode);
    } else if (utf8_len == 39) {
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x86\x85\xe8\x81\x94\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderInlineEndColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x86\x85\xe8\x81\x94\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderInlineStartColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x9d\x97\xe7\xba\xa7\xe7\xbb\x93\xe6\x9d\x9f\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderBlockEndColor, execution_context, mode);
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8\xe8\xae\xbf\xe9\x97\xae\xe5\x90\x8e\xe5\x9d\x97\xe7\xba\xa7\xe8\xb5\xb7\xe5\xa7\x8b\xe8\xbe\xb9\xe6\xa1\x86\xe9\xa2\x9c\xe8\x89\xb2", 39)) return ExposedProperty(CSSPropertyID::kInternalVisitedBorderBlockStartColor, execution_context, mode);
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
      if (!memcmp(buffer, "\xe5\x88\x97", 3)) return CSSValueID::kColumn;
      if (!memcmp(buffer, "\xe5\x9c\x86", 3)) return CSSValueID::kRound;
      if (!memcmp(buffer, "\xe5\xb0\x8f", 3)) return CSSValueID::kSmall;
      if (!memcmp(buffer, "\xe6\xa3\x95", 3)) return CSSValueID::kBrown;
      if (!memcmp(buffer, "\xe6\xa9\x99", 3)) return CSSValueID::kOrange;
      if (!memcmp(buffer, "\xe7\x81\xb0", 3)) return CSSValueID::kGray;
      if (!memcmp(buffer, "\xe7\x99\xbd", 3)) return CSSValueID::kWhite;
      if (!memcmp(buffer, "\xe7\xb2\x89", 3)) return CSSValueID::kPink;
      if (!memcmp(buffer, "\xe7\xb2\x97", 3)) return CSSValueID::kThick;
      if (!memcmp(buffer, "\xe7\xb4\xab", 3)) return CSSValueID::kPurple;
      if (!memcmp(buffer, "\xe7\xba\xa2", 3)) return CSSValueID::kRed;
      if (!memcmp(buffer, "\xe7\xbb\x86", 3)) return CSSValueID::kThin;
      if (!memcmp(buffer, "\xe7\xbb\xbf", 3)) return CSSValueID::kGreen;
      if (!memcmp(buffer, "\xe8\x93\x9d", 3)) return CSSValueID::kBlue;
      if (!memcmp(buffer, "\xe8\xa1\x8c", 3)) return CSSValueID::kRow;
      if (!memcmp(buffer, "\xe9\x9d\x92", 3)) return CSSValueID::kCyan;
      if (!memcmp(buffer, "\xe9\xbb\x84", 3)) return CSSValueID::kYellow;
      if (!memcmp(buffer, "\xe9\xbb\x91", 3)) return CSSValueID::kBlack;
    } else if (utf8_len == 6) {
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe6\xa0\x87", 6)) return CSSValueID::kSuper;
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe6\xa0\x87", 6)) return CSSValueID::kSub;
      if (!memcmp(buffer, "\xe4\xb8\xa4\xe7\xab\xaf", 6)) return CSSValueID::kJustify;
      if (!memcmp(buffer, "\xe4\xb8\xad\xe5\xa4\xae", 6)) return CSSValueID::kCentral;
      if (!memcmp(buffer, "\xe4\xb8\xad\xe7\xad\x89", 6)) return CSSValueID::kMedium;
      if (!memcmp(buffer, "\xe4\xb8\xad\xe9\x97\xb4", 6)) return CSSValueID::kMiddle;
      if (!memcmp(buffer, "\xe4\xba\xa4\xe6\x9b\xbf", 6)) return CSSValueID::kAlternate;
      if (!memcmp(buffer, "\xe4\xbb\xbb\xe6\x84\x8f", 6)) return CSSValueID::kAnywhere;
      if (!memcmp(buffer, "\xe4\xbc\x98\xe7\xbe\x8e", 6)) return CSSValueID::kPretty;
      if (!memcmp(buffer, "\xe4\xbe\xa7\xe5\x90\x91", 6)) return CSSValueID::kSideways;
      if (!memcmp(buffer, "\xe5\x80\xbe\xe6\x96\x9c", 6)) return CSSValueID::kOblique;
      if (!memcmp(buffer, "\xe5\x85\xa8\xe5\xae\xbd", 6)) return CSSValueID::kFullWidth;
      if (!memcmp(buffer, "\xe5\x85\xa8\xe9\x83\xa8", 6)) return CSSValueID::kAll;
      if (!memcmp(buffer, "\xe5\x85\xb3\xe9\x97\xad", 6)) return CSSValueID::kClose;
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xae\xb9", 6)) return CSSValueID::kContents;
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xb5\x8c", 6)) return CSSValueID::kInset;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94", 6)) return CSSValueID::kInline;
      if (!memcmp(buffer, "\xe5\x86\x85\xe9\x83\xa8", 6)) return CSSValueID::kInside;
      if (!memcmp(buffer, "\xe5\x87\xb9\xe6\xa7\xbd", 6)) return CSSValueID::kGroove;
      if (!memcmp(buffer, "\xe5\x88\x86\xe7\xa6\xbb", 6)) return CSSValueID::kSeparate;
      if (!memcmp(buffer, "\xe5\x88\x87\xe7\x89\x87", 6)) return CSSValueID::kSlice;
      if (!memcmp(buffer, "\xe5\x88\x9d\xe5\xa7\x8b", 6)) return CSSValueID::kInitial;
      if (!memcmp(buffer, "\xe5\x88\xab\xe5\x90\x8d", 6)) return CSSValueID::kAlias;
      if (!memcmp(buffer, "\xe5\x89\x8d\xe8\xbf\x9b", 6)) return CSSValueID::kForwards;
      if (!memcmp(buffer, "\xe5\x8c\x85\xe5\x90\xab", 6)) return CSSValueID::kContain;
      if (!memcmp(buffer, "\xe5\x8c\x85\xe8\xa3\xb9", 6)) return CSSValueID::kWrap;
      if (!memcmp(buffer, "\xe5\x8d\x81\xe5\xad\x97", 6)) return CSSValueID::kCrosshair;
      if (!memcmp(buffer, "\xe5\x8d\x8a\xe5\xbe\x84", 6)) return CSSValueID::kR;
      if (!memcmp(buffer, "\xe5\x8f\x8c\xe5\x90\x91", 6)) return CSSValueID::kBoth;
      if (!memcmp(buffer, "\xe5\x8f\x8c\xe7\xba\xbf", 6)) return CSSValueID::kDouble;
      if (!memcmp(buffer, "\xe5\x8f\x8d\xe9\x9d\xa2", 6)) return CSSValueID::kVerso;
      if (!memcmp(buffer, "\xe5\x8f\x98\xe4\xba\xae", 6)) return CSSValueID::kLighten;
      if (!memcmp(buffer, "\xe5\x8f\x98\xe6\x9a\x97", 6)) return CSSValueID::kDarken;
      if (!memcmp(buffer, "\xe5\x8f\xaf\xe8\xa7\x81", 6)) return CSSValueID::kVisible;
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe4\xbe\xa7", 6)) return CSSValueID::kRight;
      if (!memcmp(buffer, "\xe5\x90\x8e\xe9\x80\x80", 6)) return CSSValueID::kBackwards;
      if (!memcmp(buffer, "\xe5\x9b\x9e\xe9\x80\x80", 6)) return CSSValueID::kFallback;
      if (!memcmp(buffer, "\xe5\x9b\xba\xe5\xae\x9a", 6)) return CSSValueID::kFixed;
      if (!memcmp(buffer, "\xe5\x9c\x86\xe5\xbd\xa2", 6)) return CSSValueID::kCircle;
      if (!memcmp(buffer, "\xe5\x9c\x86\xe7\x82\xb9", 6)) return CSSValueID::kDisc;
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7", 6)) return CSSValueID::kBlock;
      if (!memcmp(buffer, "\xe5\x9f\xba\xe7\xba\xbf", 6)) return CSSValueID::kBaseline;
      if (!memcmp(buffer, "\xe5\xa1\xab\xe5\x85\x85", 6)) return CSSValueID::kFill;
      if (!memcmp(buffer, "\xe5\xa4\x8d\xe5\x88\xb6", 6)) return CSSValueID::kCopy;
      if (!memcmp(buffer, "\xe5\xa4\x96\xe6\x89\xa9", 6)) return CSSValueID::kOutset;
      if (!memcmp(buffer, "\xe5\xa4\x96\xe9\x83\xa8", 6)) return CSSValueID::kOutside;
      if (!memcmp(buffer, "\xe5\xa4\xa7\xe5\x86\x99", 6)) return CSSValueID::kUppercase;
      if (!memcmp(buffer, "\xe5\xa5\x87\xe5\x81\xb6", 6)) return CSSValueID::kEvenodd;
      if (!memcmp(buffer, "\xe5\xa7\x8b\xe7\xbb\x88", 6)) return CSSValueID::kAlways;
      if (!memcmp(buffer, "\xe5\xae\x89\xe5\x85\xa8", 6)) return CSSValueID::kSafe;
      if (!memcmp(buffer, "\xe5\xae\x9e\xe7\xba\xbf", 6)) return CSSValueID::kSolid;
      if (!memcmp(buffer, "\xe5\xae\xbd\xe5\xba\xa6", 6)) return CSSValueID::kWidth;
      if (!memcmp(buffer, "\xe5\xb0\x8f\xe5\x86\x99", 6)) return CSSValueID::kLowercase;
      if (!memcmp(buffer, "\xe5\xb0\xba\xe5\xaf\xb8", 6)) return CSSValueID::kSize;
      if (!memcmp(buffer, "\xe5\xb1\x80\xe9\x83\xa8", 6)) return CSSValueID::kLocal;
      if (!memcmp(buffer, "\xe5\xb1\x85\xe4\xb8\xad", 6)) return CSSValueID::kCenter;
      if (!memcmp(buffer, "\xe5\xb5\x8c\xe5\x85\xa5", 6)) return CSSValueID::kEmbed;
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe4\xbe\xa7", 6)) return CSSValueID::kLeft;
      if (!memcmp(buffer, "\xe5\xb7\xae\xe5\x80\xbc", 6)) return CSSValueID::kDifference;
      if (!memcmp(buffer, "\xe5\xb8\x83\xe5\xb1\x80", 6)) return CSSValueID::kLayout;
      if (!memcmp(buffer, "\xe5\xb8\xae\xe5\x8a\xa9", 6)) return CSSValueID::kHelp;
      if (!memcmp(buffer, "\xe5\xb9\xb3\xe6\xbb\x91", 6)) return CSSValueID::kSmooth;
      if (!memcmp(buffer, "\xe5\xb9\xb3\xe7\xa7\xbb", 6)) return CSSValueID::kTranslate;
      if (!memcmp(buffer, "\xe5\xb9\xb3\xe7\xab\xaf", 6)) return CSSValueID::kButt;
      if (!memcmp(buffer, "\xe5\xb9\xb3\xe8\xa1\xa1", 6)) return CSSValueID::kBalance;
      if (!memcmp(buffer, "\xe5\xb9\xb3\xe9\x9d\xa2", 6)) return CSSValueID::kFlat;
      if (!memcmp(buffer, "\xe5\xba\x8f\xe6\x95\xb0", 6)) return CSSValueID::kOrdinal;
      if (!memcmp(buffer, "\xe5\xba\x95\xe9\x83\xa8", 6)) return CSSValueID::kBottom;
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7", 6)) return CSSValueID::kFlex;
      if (!memcmp(buffer, "\xe5\xbc\xba\xe5\x85\x89", 6)) return CSSValueID::kHardLight;
      if (!memcmp(buffer, "\xe6\x89\x8b\xe5\x8a\xa8", 6)) return CSSValueID::kManual;
      if (!memcmp(buffer, "\xe6\x89\x93\xe5\xbc\x80", 6)) return CSSValueID::kOpen;
      if (!memcmp(buffer, "\xe6\x89\xa9\xe5\xb1\x95", 6)) return CSSValueID::kExpanded;
      if (!memcmp(buffer, "\xe6\x8a\x93\xe5\x8f\x96", 6)) return CSSValueID::kGrab;
      if (!memcmp(buffer, "\xe6\x8a\x98\xe5\x8f\xa0", 6)) return CSSValueID::kCollapse;
      if (!memcmp(buffer, "\xe6\x8b\x89\xe4\xbc\xb8", 6)) return CSSValueID::kStretch;
      if (!memcmp(buffer, "\xe6\x8c\x87\xe9\x92\x88", 6)) return CSSValueID::kPointer;
      if (!memcmp(buffer, "\xe6\x8e\x92\xe9\x99\xa4", 6)) return CSSValueID::kExclusion;
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9", 6)) return CSSValueID::kStroke;
      if (!memcmp(buffer, "\xe6\x94\xbe\xe5\xa4\xa7", 6)) return CSSValueID::kZoomIn;
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac", 6)) return CSSValueID::kText;
      if (!memcmp(buffer, "\xe6\x96\x9c\xe4\xbd\x93", 6)) return CSSValueID::kItalic;
      if (!memcmp(buffer, "\xe6\x96\x9c\xe6\x8e\xa5", 6)) return CSSValueID::kMiter;
      if (!memcmp(buffer, "\xe6\x96\x9c\xe9\x9d\xa2", 6)) return CSSValueID::kBevel;
      if (!memcmp(buffer, "\xe6\x96\xad\xe8\xaf\x8d", 6)) return CSSValueID::kBreakWord;
      if (!memcmp(buffer, "\xe6\x96\xb9\xe5\xbd\xa2", 6)) return CSSValueID::kSquare;
      if (!memcmp(buffer, "\xe6\x97\x8b\xe8\xbd\xac", 6)) return CSSValueID::kRotate;
      if (!memcmp(buffer, "\xe6\x97\xa0\xe5\x85\xb3", 6)) return CSSValueID::kNone;
      if (!memcmp(buffer, "\xe6\x97\xa0\xe9\x99\x90", 6)) return CSSValueID::kInfinite;
      if (!memcmp(buffer, "\xe6\x98\x8e\xe5\xba\xa6", 6)) return CSSValueID::kLuminosity;
      if (!memcmp(buffer, "\xe6\x98\xbe\xe7\xa4\xba", 6)) return CSSValueID::kVisible;
      if (!memcmp(buffer, "\xe6\x9b\xb4\xe5\xa4\xa7", 6)) return CSSValueID::kXLarge;
      if (!memcmp(buffer, "\xe6\x9b\xb4\xe5\xb0\x8f", 6)) return CSSValueID::kXSmall;
      if (!memcmp(buffer, "\xe6\x9b\xb4\xe7\xb2\x97", 6)) return CSSValueID::kBolder;
      if (!memcmp(buffer, "\xe6\x9b\xb4\xe7\xbb\x86", 6)) return CSSValueID::kLighter;
      if (!memcmp(buffer, "\xe6\x9f\x94\xe5\x85\x89", 6)) return CSSValueID::kSoftLight;
      if (!memcmp(buffer, "\xe6\xa0\xb7\xe5\xbc\x8f", 6)) return CSSValueID::kStyle;
      if (!memcmp(buffer, "\xe6\xa4\xad\xe5\x9c\x86", 6)) return CSSValueID::kEllipse;
      if (!memcmp(buffer, "\xe6\xad\xa3\xe5\xb8\xb8", 6)) return CSSValueID::kNormal;
      if (!memcmp(buffer, "\xe6\xad\xa3\xe9\x9d\xa2", 6)) return CSSValueID::kRecto;
      if (!memcmp(buffer, "\xe6\xb7\xb7\xe5\x90\x88", 6)) return CSSValueID::kMixed;
      if (!memcmp(buffer, "\xe6\xb8\x85\xe9\x99\xa4", 6)) return CSSValueID::kClear;
      if (!memcmp(buffer, "\xe6\xbb\x9a\xe5\x8a\xa8", 6)) return CSSValueID::kScroll;
      if (!memcmp(buffer, "\xe6\xbb\xa4\xe8\x89\xb2", 6)) return CSSValueID::kScreen;
      if (!memcmp(buffer, "\xe7\x82\xb9\xe7\xba\xbf", 6)) return CSSValueID::kDotted;
      if (!memcmp(buffer, "\xe7\x89\xb9\xe5\xa4\xa7", 6)) return CSSValueID::kXxLarge;
      if (!memcmp(buffer, "\xe7\x89\xb9\xe5\xb0\x8f", 6)) return CSSValueID::kXxSmall;
      if (!memcmp(buffer, "\xe7\x9b\xb4\xe7\xab\x8b", 6)) return CSSValueID::kUpright;
      if (!memcmp(buffer, "\xe7\x9b\xb8\xe5\xaf\xb9", 6)) return CSSValueID::kRelative;
      if (!memcmp(buffer, "\xe7\x9f\xa9\xe5\xbd\xa2", 6)) return CSSValueID::kRect;
      if (!memcmp(buffer, "\xe7\x9f\xa9\xe9\x98\xb5", 6)) return CSSValueID::kMatrix;
      if (!memcmp(buffer, "\xe7\xa6\x81\xe6\xad\xa2", 6)) return CSSValueID::kNotAllowed;
      if (!memcmp(buffer, "\xe7\xa7\xbb\xe5\x8a\xa8", 6)) return CSSValueID::kMove;
      if (!memcmp(buffer, "\xe7\xa8\xb3\xe5\xae\x9a", 6)) return CSSValueID::kStable;
      if (!memcmp(buffer, "\xe7\xad\x89\xe5\xbe\x85", 6)) return CSSValueID::kWait;
      if (!memcmp(buffer, "\xe7\xb2\x97\xe4\xbd\x93", 6)) return CSSValueID::kBold;
      if (!memcmp(buffer, "\xe7\xb2\x98\xe6\x80\xa7", 6)) return CSSValueID::kSticky;
      if (!memcmp(buffer, "\xe7\xb4\xa7\xe7\xbc\xa9", 6)) return CSSValueID::kCondensed;
      if (!memcmp(buffer, "\xe7\xba\xbf\xe6\x80\xa7", 6)) return CSSValueID::kLinear;
      if (!memcmp(buffer, "\xe7\xbb\x88\xe7\x82\xb9", 6)) return CSSValueID::kEnd;
      if (!memcmp(buffer, "\xe7\xbb\x98\xe5\x88\xb6", 6)) return CSSValueID::kPaint;
      if (!memcmp(buffer, "\xe7\xbb\x9d\xe5\xaf\xb9", 6)) return CSSValueID::kAbsolute;
      if (!memcmp(buffer, "\xe7\xbb\xa7\xe6\x89\xbf", 6)) return CSSValueID::kInherit;
      if (!memcmp(buffer, "\xe7\xbc\x93\xe5\x85\xa5", 6)) return CSSValueID::kEaseIn;
      if (!memcmp(buffer, "\xe7\xbc\x93\xe5\x87\xba", 6)) return CSSValueID::kEaseOut;
      if (!memcmp(buffer, "\xe7\xbc\x93\xe5\x8a\xa8", 6)) return CSSValueID::kEase;
      if (!memcmp(buffer, "\xe7\xbc\xa9\xe5\xb0\x8f", 6)) return CSSValueID::kZoomOut;
      if (!memcmp(buffer, "\xe7\xbc\xa9\xe6\x94\xbe", 6)) return CSSValueID::kScale;
      if (!memcmp(buffer, "\xe7\xbd\x91\xe6\xa0\xbc", 6)) return CSSValueID::kGrid;
      if (!memcmp(buffer, "\xe8\x83\x8c\xe6\x99\xaf", 6)) return CSSValueID::kBackground;
      if (!memcmp(buffer, "\xe8\x84\x8a\xe7\xba\xbf", 6)) return CSSValueID::kRidge;
      if (!memcmp(buffer, "\xe8\x87\xaa\xe5\x8a\xa8", 6)) return CSSValueID::kAuto;
      if (!memcmp(buffer, "\xe8\x89\xb2\xe7\x9b\xb8", 6)) return CSSValueID::kHue;
      if (!memcmp(buffer, "\xe8\x99\x9a\xe7\xba\xbf", 6)) return CSSValueID::kDashed;
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\xa0\xbc", 6)) return CSSValueID::kTable;
      if (!memcmp(buffer, "\xe8\xa3\x81\xe5\x89\xaa", 6)) return CSSValueID::kClip;
      if (!memcmp(buffer, "\xe8\xa6\x86\xe7\x9b\x96", 6)) return CSSValueID::kCover;
      if (!memcmp(buffer, "\xe8\xb5\xb7\xe7\x82\xb9", 6)) return CSSValueID::kStart;
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86", 6)) return CSSValueID::kBorder;
      if (!memcmp(buffer, "\xe8\xbf\x98\xe5\x8e\x9f", 6)) return CSSValueID::kUnset;
      if (!memcmp(buffer, "\xe9\x80\x8f\xe6\x98\x8e", 6)) return CSSValueID::kTransparent;
      if (!memcmp(buffer, "\xe9\x80\x8f\xe8\xa7\x86", 6)) return CSSValueID::kPerspective;
      if (!memcmp(buffer, "\xe9\x81\xbf\xe5\x85\x8d", 6)) return CSSValueID::kAvoid;
      if (!memcmp(buffer, "\xe9\x87\x8d\xe5\xa4\x8d", 6)) return CSSValueID::kRepeat;
      if (!memcmp(buffer, "\xe9\x97\xaa\xe7\x83\x81", 6)) return CSSValueID::kBlink;
      if (!memcmp(buffer, "\xe9\x97\xb4\xe9\x9a\x94", 6)) return CSSValueID::kSpace;
      if (!memcmp(buffer, "\xe9\x9a\x90\xe8\x97\x8f", 6)) return CSSValueID::kHidden;
      if (!memcmp(buffer, "\xe9\x9a\x94\xe7\xa6\xbb", 6)) return CSSValueID::kIsolate;
      if (!memcmp(buffer, "\xe9\x9d\x99\xe6\x80\x81", 6)) return CSSValueID::kStatic;
      if (!memcmp(buffer, "\xe9\x9d\x9e\xe9\x9b\xb6", 6)) return CSSValueID::kNonzero;
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe9\x9d\xa2", 6)) return CSSValueID::kPage;
      if (!memcmp(buffer, "\xe9\xa1\xb6\xe9\x83\xa8", 6)) return CSSValueID::kTop;
      if (!memcmp(buffer, "\xe9\xa2\x9c\xe8\x89\xb2", 6)) return CSSValueID::kColor;
      if (!memcmp(buffer, "\xe9\xab\x98\xe5\xba\xa6", 6)) return CSSValueID::kHeight;
      if (!memcmp(buffer, "\xe9\xbb\x98\xe8\xae\xa4", 6)) return CSSValueID::kDefault;
    } else if (utf8_len == 7) {
      if (!memcmp(buffer, "\x58\xe5\x9d\x90\xe6\xa0\x87", 7)) return CSSValueID::kX;
      if (!memcmp(buffer, "\x59\xe5\x9d\x90\xe6\xa0\x87", 7)) return CSSValueID::kY;
    } else if (utf8_len == 8) {
      if (!memcmp(buffer, "\xe4\xbf\x9d\xe6\x8c\x81\x33\x44", 8)) return CSSValueID::kPreserve3d;
    } else if (utf8_len == 9) {
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe4\xb8\x8b\xe6\x96\x87", 9)) return CSSValueID::kContextual;
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe5\x88\x92\xe7\xba\xbf", 9)) return CSSValueID::kOverline;
      if (!memcmp(buffer, "\xe4\xb8\x8b\xe5\x88\x92\xe7\xba\xbf", 9)) return CSSValueID::kUnderline;
      if (!memcmp(buffer, "\xe4\xb8\x8d\xe5\xae\x89\xe5\x85\xa8", 9)) return CSSValueID::kUnsafe;
      if (!memcmp(buffer, "\xe4\xb8\x8d\xe6\x8d\xa2\xe8\xa1\x8c", 9)) return CSSValueID::kNowrap;
      if (!memcmp(buffer, "\xe4\xb8\x8d\xe9\x87\x8d\xe5\xa4\x8d", 9)) return CSSValueID::kNoRepeat;
      if (!memcmp(buffer, "\xe5\x83\x8f\xe7\xb4\xa0\xe5\x8c\x96", 9)) return CSSValueID::kPixelated;
      if (!memcmp(buffer, "\xe5\x86\x85\xe5\xae\xb9\xe7\x9b\x92", 9)) return CSSValueID::kContentBox;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d", 9)) return CSSValueID::kPadding;
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xa1\xa8\xe9\xa1\xb9", 9)) return CSSValueID::kListItem;
      if (!memcmp(buffer, "\xe5\x88\x97\xe8\xb0\x83\xe6\x95\xb4", 9)) return CSSValueID::kColResize;
      if (!memcmp(buffer, "\xe5\x88\xa0\xe9\x99\xa4\xe7\xba\xbf", 9)) return CSSValueID::kLineThrough;
      if (!memcmp(buffer, "\xe5\x8d\x81\xe8\xbf\x9b\xe5\x88\xb6", 9)) return CSSValueID::kDecimal;
      if (!memcmp(buffer, "\xe5\x8d\x8a\xe6\x89\xa9\xe5\xb1\x95", 9)) return CSSValueID::kSemiExpanded;
      if (!memcmp(buffer, "\xe5\x8d\x8a\xe7\xb4\xa7\xe7\xbc\xa9", 9)) return CSSValueID::kSemiCondensed;
      if (!memcmp(buffer, "\xe5\x8d\x95\xe5\x85\x83\xe6\xa0\xbc", 9)) return CSSValueID::kCell;
      if (!memcmp(buffer, "\xe5\x8f\x8d\xe5\x90\x91\xe5\x88\x97", 9)) return CSSValueID::kColumnReverse;
      if (!memcmp(buffer, "\xe5\x8f\x8d\xe5\x90\x91\xe8\xa1\x8c", 9)) return CSSValueID::kRowReverse;
      if (!memcmp(buffer, "\xe5\xa1\xab\xe5\x85\x85\xe7\x9b\x92", 9)) return CSSValueID::kFillBox;
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe6\xb5\x81", 9)) return CSSValueID::kFlexFlow;
      if (!memcmp(buffer, "\xe6\x8a\x93\xe5\x8f\x96\xe4\xb8\xad", 9)) return CSSValueID::kGrabbing;
      if (!memcmp(buffer, "\xe6\x8f\x8f\xe8\xbe\xb9\xe7\x9b\x92", 9)) return CSSValueID::kStrokeBox;
      if (!memcmp(buffer, "\xe6\x96\x9c\xe5\x88\x86\xe6\x95\xb0", 9)) return CSSValueID::kDiagonalFractions;
      if (!memcmp(buffer, "\xe6\x96\x9c\xe6\x9d\xa0\xe9\x9b\xb6", 9)) return CSSValueID::kSlashedZero;
      if (!memcmp(buffer, "\xe6\x9b\xb4\xe6\x89\xa9\xe5\xb1\x95", 9)) return CSSValueID::kExtraExpanded;
      if (!memcmp(buffer, "\xe6\x9b\xb4\xe7\xb4\xa7\xe7\xbc\xa9", 9)) return CSSValueID::kExtraCondensed;
      if (!memcmp(buffer, "\xe6\x9c\x80\xe8\xbf\x91\xe8\xa7\x92", 9)) return CSSValueID::kClosestCorner;
      if (!memcmp(buffer, "\xe6\x9c\x80\xe8\xbf\x91\xe8\xbe\xb9", 9)) return CSSValueID::kClosestSide;
      if (!memcmp(buffer, "\xe6\x9c\x80\xe8\xbf\x9c\xe8\xa7\x92", 9)) return CSSValueID::kFarthestCorner;
      if (!memcmp(buffer, "\xe6\x9c\x80\xe8\xbf\x9c\xe8\xbe\xb9", 9)) return CSSValueID::kFarthestSide;
      if (!memcmp(buffer, "\xe7\x9c\x81\xe7\x95\xa5\xe5\x8f\xb7", 9)) return CSSValueID::kEllipsis;
      if (!memcmp(buffer, "\xe7\xba\xaf\xe6\x96\x87\xe6\x9c\xac", 9)) return CSSValueID::kPlaintext;
      if (!memcmp(buffer, "\xe8\xa1\x8c\xe8\xb0\x83\xe6\x95\xb4", 9)) return CSSValueID::kRowResize;
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\xa0\xbc\xe8\xa1\x8c", 9)) return CSSValueID::kTableRow;
      if (!memcmp(buffer, "\xe8\xa6\x86\xe7\x9b\x96\xe5\xb1\x82", 9)) return CSSValueID::kOverlay;
      if (!memcmp(buffer, "\xe8\xa7\x86\xe5\x9b\xbe\xe7\x9b\x92", 9)) return CSSValueID::kViewBox;
      if (!memcmp(buffer, "\xe8\xb6\x85\xe6\x89\xa9\xe5\xb1\x95", 9)) return CSSValueID::kUltraExpanded;
      if (!memcmp(buffer, "\xe8\xb6\x85\xe7\xb4\xa7\xe7\xbc\xa9", 9)) return CSSValueID::kUltraCondensed;
      if (!memcmp(buffer, "\xe8\xbe\xb9\xe6\xa1\x86\xe7\x9b\x92", 9)) return CSSValueID::kBorderBox;
      if (!memcmp(buffer, "\xe8\xbf\x9b\xe8\xa1\x8c\xe4\xb8\xad", 9)) return CSSValueID::kProgress;
      if (!memcmp(buffer, "\xe9\xa5\xb1\xe5\x92\x8c\xe5\xba\xa6", 9)) return CSSValueID::kSaturation;
    } else if (utf8_len == 12) {
      if (!memcmp(buffer, "\x57\x65\x62\x6b\x69\x74\xe5\xbc\xb9\xe6\x80\xa7", 12)) return CSSValueID::kWebkitFlex;
      if (!memcmp(buffer, "\xe4\xb8\xa4\xe7\xab\xaf\xe5\xaf\xb9\xe9\xbd\x90", 12)) return CSSValueID::kSpaceBetween;
      if (!memcmp(buffer, "\xe4\xbb\x8e\xe5\x8f\xb3\xe5\x88\xb0\xe5\xb7\xa6", 12)) return CSSValueID::kRtl;
      if (!memcmp(buffer, "\xe4\xbb\x8e\xe5\xb7\xa6\xe5\x88\xb0\xe5\x8f\xb3", 12)) return CSSValueID::kLtr;
      if (!memcmp(buffer, "\xe4\xbb\xbb\xe6\x84\x8f\xe6\x96\xad\xe8\xa1\x8c", 12)) return CSSValueID::kBreakAll;
      if (!memcmp(buffer, "\xe4\xbf\x9d\xe6\x8c\x81\xe6\x95\xb4\xe4\xbd\x93", 12)) return CSSValueID::kKeepAll;
      if (!memcmp(buffer, "\xe4\xbf\x9d\xe7\x95\x99\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return CSSValueID::kPreLine;
      if (!memcmp(buffer, "\xe4\xbf\x9d\xe7\x95\x99\xe7\xa9\xba\xe7\x99\xbd", 12)) return CSSValueID::kPre;
      if (!memcmp(buffer, "\xe5\x85\xa8\xe9\x83\xa8\xe6\xbb\x9a\xe5\x8a\xa8", 12)) return CSSValueID::kAllScroll;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe5\x9d\x97\xe7\xba\xa7", 12)) return CSSValueID::kInlineBlock;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe5\xb0\xba\xe5\xaf\xb8", 12)) return CSSValueID::kInlineSize;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe5\xbc\xb9\xe6\x80\xa7", 12)) return CSSValueID::kInlineFlex;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe7\xbd\x91\xe6\xa0\xbc", 12)) return CSSValueID::kInlineGrid;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\x81\x94\xe8\xa1\xa8\xe6\xa0\xbc", 12)) return CSSValueID::kInlineTable;
      if (!memcmp(buffer, "\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d\xe7\x9b\x92", 12)) return CSSValueID::kPaddingBox;
      if (!memcmp(buffer, "\xe5\x8e\x86\xe5\x8f\xb2\xe8\xbf\x9e\xe5\xad\x97", 12)) return CSSValueID::kHistoricalLigatures;
      if (!memcmp(buffer, "\xe5\x8f\x8c\xe5\x90\x91\xe8\xa6\x86\xe7\x9b\x96", 12)) return CSSValueID::kBidiOverride;
      if (!memcmp(buffer, "\xe5\x8f\x8d\xe5\x90\x91\xe4\xba\xa4\xe6\x9b\xbf", 12)) return CSSValueID::kAlternateReverse;
      if (!memcmp(buffer, "\xe5\x8f\x8d\xe5\x90\x91\xe6\x8d\xa2\xe8\xa1\x8c", 12)) return CSSValueID::kWrapReverse;
      if (!memcmp(buffer, "\xe5\x8f\xaf\xe9\x80\x89\xe8\xbf\x9e\xe5\xad\x97", 12)) return CSSValueID::kDiscretionaryLigatures;
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe4\xb8\x8a\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kNeResize;
      if (!memcmp(buffer, "\xe5\x8f\xb3\xe4\xb8\x8b\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kSeResize;
      if (!memcmp(buffer, "\xe5\x90\x91\xe4\xb8\x8b\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kSResize;
      if (!memcmp(buffer, "\xe5\x90\x91\xe5\xb7\xa6\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kWResize;
      if (!memcmp(buffer, "\xe5\x9b\x9e\xe9\x80\x80\xe5\x9b\xbe\xe5\xb1\x82", 12)) return CSSValueID::kRevertLayer;
      if (!memcmp(buffer, "\xe5\x9d\x87\xe5\x8c\x80\xe5\xaf\xb9\xe9\xbd\x90", 12)) return CSSValueID::kSpaceEvenly;
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe5\xb0\xba\xe5\xaf\xb8", 12)) return CSSValueID::kBlockSize;
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe6\x96\x87\xe6\x9c\xac", 12)) return CSSValueID::kVerticalText;
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kNResize;
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe9\x87\x8d\xe5\xa4\x8d", 12)) return CSSValueID::kRepeatY;
      if (!memcmp(buffer, "\xe5\xa0\x86\xe5\x8f\xa0\xe5\x88\x86\xe6\x95\xb0", 12)) return CSSValueID::kStackedFractions;
      if (!memcmp(buffer, "\xe5\xad\x97\xe6\xaf\x8d\xe5\x9f\xba\xe7\xba\xbf", 12)) return CSSValueID::kAlphabetic;
      if (!memcmp(buffer, "\xe5\xae\x9a\xe4\xbd\x8d\xe5\x8c\xba\xe5\x9f\x9f", 12)) return CSSValueID::kPositionArea;
      if (!memcmp(buffer, "\xe5\xb0\x8f\xe5\x9e\x8b\xe5\xa4\xa7\xe5\x86\x99", 12)) return CSSValueID::kSmallCaps;
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe4\xb8\x8a\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kNwResize;
      if (!memcmp(buffer, "\xe5\xb7\xa6\xe4\xb8\x8b\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kSwResize;
      if (!memcmp(buffer, "\xe5\xb8\xb8\xe8\xa7\x81\xe8\xbf\x9e\xe5\xad\x97", 12)) return CSSValueID::kCommonLigatures;
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe7\xbb\x88\xe7\x82\xb9", 12)) return CSSValueID::kFlexEnd;
      if (!memcmp(buffer, "\xe5\xbc\xb9\xe6\x80\xa7\xe8\xb5\xb7\xe7\x82\xb9", 12)) return CSSValueID::kFlexStart;
      if (!memcmp(buffer, "\xe6\x82\xac\xe6\x8c\x82\xe5\x9f\xba\xe7\xba\xbf", 12)) return CSSValueID::kHanging;
      if (!memcmp(buffer, "\xe6\x95\xb0\xe5\xad\xa6\xe5\x9f\xba\xe7\xba\xbf", 12)) return CSSValueID::kMathematical;
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe5\xba\x95\xe9\x83\xa8", 12)) return CSSValueID::kTextBottom;
      if (!memcmp(buffer, "\xe6\x96\x87\xe6\x9c\xac\xe9\xa1\xb6\xe9\x83\xa8", 12)) return CSSValueID::kTextTop;
      if (!memcmp(buffer, "\xe6\x96\xad\xe8\xa1\x8c\xe7\xa9\xba\xe7\x99\xbd", 12)) return CSSValueID::kBreakSpaces;
      if (!memcmp(buffer, "\xe6\x97\xa0\xe4\xb8\x8a\xe4\xb8\x8b\xe6\x96\x87", 12)) return CSSValueID::kNoContextual;
      if (!memcmp(buffer, "\xe6\x97\xa7\xe5\xbc\x8f\xe6\x95\xb0\xe5\xad\x97", 12)) return CSSValueID::kOldstyleNums;
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xa4\xa7\xe5\x86\x85\xe5\xae\xb9", 12)) return CSSValueID::kMaxContent;
      if (!memcmp(buffer, "\xe6\x9c\x80\xe5\xb0\x8f\xe5\x86\x85\xe5\xae\xb9", 12)) return CSSValueID::kMinContent;
      if (!memcmp(buffer, "\xe6\xa0\x87\xe9\xa2\x98\xe5\xa4\xa7\xe5\x86\x99", 12)) return CSSValueID::kTitlingCaps;
      if (!memcmp(buffer, "\xe6\xad\xa3\xe7\x89\x87\xe5\x8f\xa0\xe5\xba\x95", 12)) return CSSValueID::kMultiply;
      if (!memcmp(buffer, "\xe6\xaf\x94\xe4\xbe\x8b\xe6\x95\xb0\xe5\xad\x97", 12)) return CSSValueID::kProportionalNums;
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe8\xb0\x83\xe6\x95\xb4", 12)) return CSSValueID::kEResize;
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe9\x87\x8d\xe5\xa4\x8d", 12)) return CSSValueID::kRepeatX;
      if (!memcmp(buffer, "\xe7\x8e\xaf\xe7\xbb\x95\xe5\xaf\xb9\xe9\xbd\x90", 12)) return CSSValueID::kSpaceAround;
      if (!memcmp(buffer, "\xe7\xa6\x81\xe6\xad\xa2\xe6\x94\xbe\xe7\xbd\xae", 12)) return CSSValueID::kNoDrop;
      if (!memcmp(buffer, "\xe7\xbc\x93\xe5\x85\xa5\xe7\xbc\x93\xe5\x87\xba", 12)) return CSSValueID::kEaseInOut;
      if (!memcmp(buffer, "\xe8\x87\xaa\xe8\xba\xab\xe7\xbb\x88\xe7\x82\xb9", 12)) return CSSValueID::kSelfEnd;
      if (!memcmp(buffer, "\xe8\x87\xaa\xe8\xba\xab\xe8\xb5\xb7\xe7\x82\xb9", 12)) return CSSValueID::kSelfStart;
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\x84\x8f\xe5\x9f\xba\xe7\xba\xbf", 12)) return CSSValueID::kIdeographic;
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\xa0\xbc\xe6\x95\xb0\xe5\xad\x97", 12)) return CSSValueID::kTabularNums;
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\xa0\xbc\xe6\xa0\x87\xe9\xa2\x98", 12)) return CSSValueID::kTableCaption;
      if (!memcmp(buffer, "\xe8\xa1\xac\xe7\xba\xbf\xe6\x95\xb0\xe5\xad\x97", 12)) return CSSValueID::kLiningNums;
      if (!memcmp(buffer, "\xe9\x81\xbf\xe5\x85\x8d\xe5\x88\x86\xe5\x88\x97", 12)) return CSSValueID::kAvoidColumn;
      if (!memcmp(buffer, "\xe9\x81\xbf\xe5\x85\x8d\xe5\x88\x86\xe9\xa1\xb5", 12)) return CSSValueID::kAvoidPage;
      if (!memcmp(buffer, "\xe9\x98\xb6\xe8\xb7\x83\xe7\xbb\x88\xe7\x82\xb9", 12)) return CSSValueID::kStepEnd;
      if (!memcmp(buffer, "\xe9\x98\xb6\xe8\xb7\x83\xe8\xb5\xb7\xe7\x82\xb9", 12)) return CSSValueID::kStepStart;
      if (!memcmp(buffer, "\xe9\x9a\x94\xe7\xa6\xbb\xe8\xa6\x86\xe7\x9b\x96", 12)) return CSSValueID::kIsolateOverride;
      if (!memcmp(buffer, "\xe9\xa1\xb5\xe9\x9d\xa2\xe7\xbc\xa9\xe6\x94\xbe", 12)) return CSSValueID::kZoom;
      if (!memcmp(buffer, "\xe9\xa2\x9c\xe8\x89\xb2\xe5\x87\x8f\xe6\xb7\xa1", 12)) return CSSValueID::kColorDodge;
      if (!memcmp(buffer, "\xe9\xa2\x9c\xe8\x89\xb2\xe5\x8a\xa0\xe6\xb7\xb1", 12)) return CSSValueID::kColorBurn;
    } else if (utf8_len == 15) {
      if (!memcmp(buffer, "\xe4\xb8\x8a\xe4\xb8\x8b\xe6\x96\x87\xe8\x8f\x9c\xe5\x8d\x95", 15)) return CSSValueID::kContextMenu;
      if (!memcmp(buffer, "\xe6\x97\xa0\xe5\xb8\xb8\xe8\xa7\x81\xe8\xbf\x9e\xe5\xad\x97", 15)) return CSSValueID::kNoCommonLigatures;
      if (!memcmp(buffer, "\xe8\x87\xaa\xe9\x80\x82\xe5\xba\x94\xe5\x86\x85\xe5\xae\xb9", 15)) return CSSValueID::kFitContent;
      if (!memcmp(buffer, "\xe8\xa1\xa8\xe6\xa0\xbc\xe5\x8d\x95\xe5\x85\x83\xe6\xa0\xbc", 15)) return CSSValueID::kTableCell;
      if (!memcmp(buffer, "\xe9\xa6\x96\xe5\xad\x97\xe6\xaf\x8d\xe5\xa4\xa7\xe5\x86\x99", 15)) return CSSValueID::kCapitalize;
    } else if (utf8_len == 18) {
      if (!memcmp(buffer, "\xe4\xbe\xa7\xe5\x90\x91\xe4\xbb\x8e\xe5\x8f\xb3\xe5\x88\xb0\xe5\xb7\xa6", 18)) return CSSValueID::kSidewaysRl;
      if (!memcmp(buffer, "\xe4\xbe\xa7\xe5\x90\x91\xe4\xbb\x8e\xe5\xb7\xa6\xe5\x88\xb0\xe5\x8f\xb3", 18)) return CSSValueID::kSidewaysLr;
      if (!memcmp(buffer, "\xe4\xbf\x9d\xe7\x95\x99\xe7\xa9\xba\xe7\x99\xbd\xe6\x8d\xa2\xe8\xa1\x8c", 18)) return CSSValueID::kPreWrap;
      if (!memcmp(buffer, "\xe5\x85\xa8\xe9\x83\xa8\xe5\xb0\x8f\xe5\x9e\x8b\xe5\xa4\xa7\xe5\x86\x99", 18)) return CSSValueID::kAllSmallCaps;
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe4\xbb\x8e\xe5\x8f\xb3\xe5\x88\xb0\xe5\xb7\xa6", 18)) return CSSValueID::kVerticalRl;
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe4\xbb\x8e\xe5\xb7\xa6\xe5\x88\xb0\xe5\x8f\xb3", 18)) return CSSValueID::kVerticalLr;
      if (!memcmp(buffer, "\xe5\x9e\x82\xe7\x9b\xb4\xe5\x8f\x8c\xe5\x90\x91\xe8\xb0\x83\xe6\x95\xb4", 18)) return CSSValueID::kNsResize;
      if (!memcmp(buffer, "\xe5\xaf\xb9\xe8\xa7\x92\xe5\x8f\x8c\xe5\x90\x91\xe8\xb0\x83\xe6\x95\xb4", 18)) return CSSValueID::kNeswResize;
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe5\x8f\x8c\xe5\x90\x91\xe8\xb0\x83\xe6\x95\xb4", 18)) return CSSValueID::kEwResize;
      if (!memcmp(buffer, "\xe6\xb0\xb4\xe5\xb9\xb3\xe8\x87\xaa\xe4\xb8\x8a\xe8\x80\x8c\xe4\xb8\x8b", 18)) return CSSValueID::kHorizontalTb;
    } else if (utf8_len == 24) {
      if (!memcmp(buffer, "\xe5\x9d\x97\xe7\xba\xa7\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe4\xb8\x8a\xe4\xb8\x8b\xe6\x96\x87", 24)) return CSSValueID::kFlowRoot;
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

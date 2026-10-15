/*
 * Copyright (C) 2013 Google, Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PARSER_HTML_TOKEN_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PARSER_HTML_TOKEN_H_

#include <memory>
#include <utility>

#include "base/check_op.h"
#include "base/dcheck_is_on.h"
#include "base/memory/raw_ptr.h"
#include "third_party/blink/renderer/core/dom/attribute.h"
#include "third_party/blink/renderer/core/html/parser/html_parser_idioms.h"
#include "third_party/blink/renderer/core/html/parser/literal_buffer.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"

namespace blink {

struct DoctypeData {
  USING_FAST_MALLOC(DoctypeData);

 public:
  DoctypeData()
      : has_public_identifier_(false),
        has_system_identifier_(false),
        force_quirks_(false) {}
  DoctypeData(const DoctypeData&) = delete;
  DoctypeData& operator=(const DoctypeData&) = delete;

  bool has_public_identifier_;
  bool has_system_identifier_;
  Vector<UChar> public_identifier_;
  Vector<UChar> system_identifier_;
  bool force_quirks_;
};

enum class DOMPartTokenType {
  kChildNodePartStart,
  kChildNodePartEnd,
};

struct DOMPartData {
  USING_FAST_MALLOC(DOMPartData);

 public:
  explicit DOMPartData(DOMPartTokenType type) : type_(type) {
  }
  DOMPartData(const DOMPartData&) = delete;
  DOMPartData& operator=(const DOMPartData&) = delete;

  Vector<String> metadata_;
  DOMPartTokenType type_;
};

struct DOMPartsNeeded {
 public:
  bool needs_node_part{false};
  Vector<AtomicString> needs_attribute_parts{};
  explicit operator bool() const {
    return needs_node_part || !needs_attribute_parts.empty();
  }
};

static inline Attribute* FindAttributeInVector(base::span<Attribute> attributes,
                                               const QualifiedName& name) {
  for (Attribute& attr : attributes) {
    if (attr.GetName().Matches(name))
      return &attr;
  }
  return nullptr;
}

class HTMLToken {
  USING_FAST_MALLOC(HTMLToken);

 public:
  enum TokenType {
    kUninitialized,
    DOCTYPE,
    kStartTag,
    kEndTag,
    kComment,
    kCharacter,
    kEndOfFile,
    kProcessingInstruction,
  };

  class Attribute {
   public:
    AtomicString GetName() const { return name_.AsAtomicString(); }
    AtomicString GetValue() const { return value_.AsAtomicString(); }

    const UCharLiteralBuffer<32>& NameBuffer() const { return name_; }

    String NameAttemptStaticStringCreation() const {
      return AttemptStaticStringCreation(name_);
    }

    bool NameIsEmpty() const { return name_.IsEmpty(); }
    void AppendToName(UChar c) { name_.AddChar(c); }

    String Value() const { return value_.AsString(); }

    void AppendToValue(UChar c) { value_.AddChar(c); }
    void ClearValue() { value_.clear(); }

   private:
    // TODO(chromium:1204030): Do a more rigorous study and select a
    // better-informed inline capacity.
    UCharLiteralBuffer<32> name_;
    UCharLiteralBuffer<32> value_;
  };

  typedef Vector<Attribute, kAttributePrealloc> AttributeList;

  // By using an inline capacity of 256, we avoid spilling over into an malloced
  // buffer approximately 99% of the time based on a non-scientific browse
  // around a number of popular web sites on 23 May 2013.
  // TODO(chromium:1204030): Do a more rigorous study and select a
  // better-informed inline capacity.
  using DataVector = UCharLiteralBuffer<256>;

  HTMLToken() = default;

  HTMLToken(const HTMLToken&) = delete;
  HTMLToken& operator=(const HTMLToken&) = delete;

  std::unique_ptr<HTMLToken> Take() {
    std::unique_ptr<HTMLToken> copy = std::make_unique<HTMLToken>();
    copy->data_ = std::move(data_);
    copy->attributes_ = std::move(attributes_);
    copy->doctype_data_ = std::move(doctype_data_);
    copy->type_ = type_;
    copy->self_closing_ = self_closing_;
    copy->has_entity_ = has_entity_;
    // Reset to uninitialized.
    Clear();
    return copy;
  }

  ALWAYS_INLINE void Clear() {
    if (type_ == kUninitialized)
      return;

    type_ = kUninitialized;
    has_entity_ = false;
    data_.clear();
    processing_instruction_target_.reset();
    if (current_attribute_) {
      current_attribute_ = nullptr;
      attributes_.clear();
    }
  }

  bool IsUninitialized() { return type_ == kUninitialized; }
  TokenType GetType() const { return type_; }

  void MakeEndOfFile() {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kEndOfFile;
  }

  const DataVector& Data() const {
    DCHECK(type_ == kCharacter || type_ == kComment || type_ == kStartTag ||
           type_ == kEndTag || type_ == kProcessingInstruction);
    return data_;
  }

  // Converts a Chinese tag name (UTF-16 code units) to the equivalent English
  // tag name in place. English tag names are left unchanged. This is only
  // meaningful for tokens whose name is used as an element tag name, so it is
  // guarded to kStartTag / kEndTag / DOCTYPE.
  void ConvertName() {
    DCHECK(type_ == kStartTag || type_ == kEndTag || type_ == DOCTYPE);
    switch (data_.size()) {
      case 1:
        if (data_.data()[0] == 0x5217) {
          data_.clear();
          data_.AddChar('c'); data_.AddChar('o'); data_.AddChar('l');
          break;
        }
        if (data_.data()[0] == 0x5757) {
          data_.clear();
          data_.AddChar('d'); data_.AddChar('i'); data_.AddChar('v');
          break;
        }
        if (data_.data()[0] == 0x6bb5) {
          data_.clear();
          data_.AddChar('p');
          break;
        }
        if (data_.data()[0] == 0x884c) {
          data_.clear();
          data_.AddChar('t'); data_.AddChar('r');
          break;
        }
        if (data_.data()[0] == 0x5706) {
          data_.clear();
          data_.AddChar('c'); data_.AddChar('i'); data_.AddChar('r'); data_.AddChar('c'); data_.AddChar('l'); data_.AddChar('e');
          break;
        }
        break;
      case 2:
        if (data_.data()[0] == 0x4e0a) {
          if (data_.data()[1] == 0x6807) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('u'); data_.AddChar('p');
            break;
          }
        }
        if (data_.data()[0] == 0x4e0b) {
          if (data_.data()[1] == 0x6807) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('u'); data_.AddChar('b');
            break;
          }
        }
        if (data_.data()[0] == 0x4e3b) {
          if (data_.data()[1] == 0x4f53) {
            data_.clear();
            data_.AddChar('b'); data_.AddChar('o'); data_.AddChar('d'); data_.AddChar('y');
            break;
          }
          if (data_.data()[1] == 0x8981) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('i'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x4ee3) {
          if (data_.data()[1] == 0x7801) {
            data_.clear();
            data_.AddChar('c'); data_.AddChar('o'); data_.AddChar('d'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x5185) {
          if (data_.data()[1] == 0x6846) {
            data_.clear();
            data_.AddChar('i'); data_.AddChar('f'); data_.AddChar('r'); data_.AddChar('a'); data_.AddChar('m'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x5217) {
          if (data_.data()[1] == 0x7ec4) {
            data_.clear();
            data_.AddChar('c'); data_.AddChar('o'); data_.AddChar('l'); data_.AddChar('g'); data_.AddChar('r'); data_.AddChar('o'); data_.AddChar('u'); data_.AddChar('p');
            break;
          }
        }
        if (data_.data()[0] == 0x5220) {
          if (data_.data()[1] == 0x9664) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('e'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x533a) {
          if (data_.data()[1] == 0x5757) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('e'); data_.AddChar('c'); data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('o'); data_.AddChar('n');
            break;
          }
          if (data_.data()[1] == 0x57df) {
            data_.clear();
            data_.AddChar('a'); data_.AddChar('r'); data_.AddChar('e'); data_.AddChar('a');
            break;
          }
        }
        if (data_.data()[0] == 0x539f) {
          if (data_.data()[1] == 0x6837) {
            data_.clear();
            data_.AddChar('p'); data_.AddChar('r'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x53c2) {
          if (data_.data()[1] == 0x6570) {
            data_.clear();
            data_.AddChar('p'); data_.AddChar('a'); data_.AddChar('r'); data_.AddChar('a'); data_.AddChar('m');
            break;
          }
        }
        if (data_.data()[0] == 0x53d8) {
          if (data_.data()[1] == 0x91cf) {
            data_.clear();
            data_.AddChar('v'); data_.AddChar('a'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x56fe) {
          if (data_.data()[1] == 0x50cf) {
            data_.clear();
            data_.AddChar('i'); data_.AddChar('m'); data_.AddChar('g');
            break;
          }
          if (data_.data()[1] == 0x5f62) {
            data_.clear();
            data_.AddChar('f'); data_.AddChar('i'); data_.AddChar('g'); data_.AddChar('u'); data_.AddChar('r'); data_.AddChar('e');
            break;
          }
          if (data_.data()[1] == 0x6807) {
            data_.clear();
            data_.AddChar('i');
            break;
          }
          if (data_.data()[1] == 0x7247) {
            data_.clear();
            data_.AddChar('p'); data_.AddChar('i'); data_.AddChar('c'); data_.AddChar('t'); data_.AddChar('u'); data_.AddChar('r'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x5730) {
          if (data_.data()[1] == 0x56fe) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('p');
            break;
          }
          if (data_.data()[1] == 0x5740) {
            data_.clear();
            data_.AddChar('a'); data_.AddChar('d'); data_.AddChar('d'); data_.AddChar('r'); data_.AddChar('e'); data_.AddChar('s'); data_.AddChar('s');
            break;
          }
        }
        if (data_.data()[0] == 0x57fa) {
          if (data_.data()[1] == 0x5740) {
            data_.clear();
            data_.AddChar('b'); data_.AddChar('a'); data_.AddChar('s'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x5934) {
          if (data_.data()[1] == 0x90e8) {
            data_.clear();
            data_.AddChar('h'); data_.AddChar('e'); data_.AddChar('a'); data_.AddChar('d'); data_.AddChar('e'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x5b9a) {
          if (data_.data()[1] == 0x4e49) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('f'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x5bf9) {
          if (data_.data()[1] == 0x8c61) {
            data_.clear();
            data_.AddChar('o'); data_.AddChar('b'); data_.AddChar('j'); data_.AddChar('e'); data_.AddChar('c'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x5bfc) {
          if (data_.data()[1] == 0x822a) {
            data_.clear();
            data_.AddChar('n'); data_.AddChar('a'); data_.AddChar('v');
            break;
          }
        }
        if (data_.data()[0] == 0x5c0f) {
          if (data_.data()[1] == 0x5b57) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('l'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x5c3e) {
          if (data_.data()[1] == 0x90e8) {
            data_.clear();
            data_.AddChar('f'); data_.AddChar('o'); data_.AddChar('o'); data_.AddChar('t'); data_.AddChar('e'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x5d4c) {
          if (data_.data()[1] == 0x5165) {
            data_.clear();
            data_.AddChar('e'); data_.AddChar('m'); data_.AddChar('b'); data_.AddChar('e'); data_.AddChar('d');
            break;
          }
        }
        if (data_.data()[0] == 0x5f15) {
          if (data_.data()[1] == 0x7528) {
            data_.clear();
            data_.AddChar('c'); data_.AddChar('i'); data_.AddChar('t'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x5f3a) {
          if (data_.data()[1] == 0x8c03) {
            data_.clear();
            data_.AddChar('e'); data_.AddChar('m');
            break;
          }
        }
        if (data_.data()[0] == 0x62ac) {
          if (data_.data()[1] == 0x5934) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('t'); data_.AddChar('l'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x62fc) {
          if (data_.data()[1] == 0x97f3) {
            data_.clear();
            data_.AddChar('r'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x6309) {
          if (data_.data()[1] == 0x94ae) {
            data_.clear();
            data_.AddChar('b'); data_.AddChar('u'); data_.AddChar('t'); data_.AddChar('t'); data_.AddChar('o'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x6362) {
          if (data_.data()[1] == 0x884c) {
            data_.clear();
            data_.AddChar('b'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x63cf) {
          if (data_.data()[1] == 0x8ff0) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x63d2) {
          if (data_.data()[1] == 0x5165) {
            data_.clear();
            data_.AddChar('i'); data_.AddChar('n'); data_.AddChar('s');
            break;
          }
          if (data_.data()[1] == 0x69fd) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('l'); data_.AddChar('o'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x6458) {
          if (data_.data()[1] == 0x8981) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('u'); data_.AddChar('m'); data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('r'); data_.AddChar('y');
            break;
          }
        }
        if (data_.data()[0] == 0x6570) {
          if (data_.data()[1] == 0x5b66) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('t'); data_.AddChar('h');
            break;
          }
          if (data_.data()[1] == 0x636e) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('a'); data_.AddChar('t'); data_.AddChar('a');
            break;
          }
        }
        if (data_.data()[0] == 0x659c) {
          if (data_.data()[1] == 0x4f53) {
            data_.clear();
            data_.AddChar('i');
            break;
          }
        }
        if (data_.data()[0] == 0x65e0) {
          if (data_.data()[1] == 0x5e8f) {
            data_.clear();
            data_.AddChar('u'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x65f6) {
          if (data_.data()[1] == 0x95f4) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('m'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x6709) {
          if (data_.data()[1] == 0x5e8f) {
            data_.clear();
            data_.AddChar('o'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x672f) {
          if (data_.data()[1] == 0x8bed) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x6761) {
          if (data_.data()[1] == 0x76ee) {
            data_.clear();
            data_.AddChar('l'); data_.AddChar('i');
            break;
          }
        }
        if (data_.data()[0] == 0x6765) {
          if (data_.data()[1] == 0x6e90) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('o'); data_.AddChar('u'); data_.AddChar('r'); data_.AddChar('c'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x6807) {
          if (data_.data()[1] == 0x7b7e) {
            data_.clear();
            data_.AddChar('l'); data_.AddChar('a'); data_.AddChar('b'); data_.AddChar('e'); data_.AddChar('l');
            break;
          }
          if (data_.data()[1] == 0x9898) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('t'); data_.AddChar('l'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x6837) {
          if (data_.data()[1] == 0x5f0f) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('t'); data_.AddChar('y'); data_.AddChar('l'); data_.AddChar('e');
            break;
          }
          if (data_.data()[1] == 0x672c) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('a'); data_.AddChar('m'); data_.AddChar('p');
            break;
          }
        }
        if (data_.data()[0] == 0x6a21) {
          if (data_.data()[1] == 0x677f) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('e'); data_.AddChar('m'); data_.AddChar('p'); data_.AddChar('l'); data_.AddChar('a'); data_.AddChar('t'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x6ce8) {
          if (data_.data()[1] == 0x97f3) {
            data_.clear();
            data_.AddChar('r'); data_.AddChar('u'); data_.AddChar('b'); data_.AddChar('y');
            break;
          }
        }
        if (data_.data()[0] == 0x753b) {
          if (data_.data()[1] == 0x5e03) {
            data_.clear();
            data_.AddChar('c'); data_.AddChar('a'); data_.AddChar('n'); data_.AddChar('v'); data_.AddChar('a'); data_.AddChar('s');
            break;
          }
        }
        if (data_.data()[0] == 0x7a81) {
          if (data_.data()[1] == 0x51fa) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('r'); data_.AddChar('k');
            break;
          }
        }
        if (data_.data()[0] == 0x7c97) {
          if (data_.data()[1] == 0x4f53) {
            data_.clear();
            data_.AddChar('b');
            break;
          }
        }
        if (data_.data()[0] == 0x7f29) {
          if (data_.data()[1] == 0x5199) {
            data_.clear();
            data_.AddChar('a'); data_.AddChar('b'); data_.AddChar('b'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x7f51) {
          if (data_.data()[1] == 0x9875) {
            data_.clear();
            data_.AddChar('h'); data_.AddChar('t'); data_.AddChar('m'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x811a) {
          if (data_.data()[1] == 0x672c) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('c'); data_.AddChar('r'); data_.AddChar('i'); data_.AddChar('p'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x8303) {
          if (data_.data()[1] == 0x56f4) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('p'); data_.AddChar('a'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x83dc) {
          if (data_.data()[1] == 0x5355) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('e'); data_.AddChar('n'); data_.AddChar('u');
            break;
          }
        }
        if (data_.data()[0] == 0x8868) {
          if (data_.data()[1] == 0x4f53) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('b'); data_.AddChar('o'); data_.AddChar('d'); data_.AddChar('y');
            break;
          }
          if (data_.data()[1] == 0x5355) {
            data_.clear();
            data_.AddChar('f'); data_.AddChar('o'); data_.AddChar('r'); data_.AddChar('m');
            break;
          }
          if (data_.data()[1] == 0x5934) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('h'); data_.AddChar('e'); data_.AddChar('a'); data_.AddChar('d');
            break;
          }
          if (data_.data()[1] == 0x5c3e) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('f'); data_.AddChar('o'); data_.AddChar('o'); data_.AddChar('t');
            break;
          }
          if (data_.data()[1] == 0x683c) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('a'); data_.AddChar('b'); data_.AddChar('l'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x89c6) {
          if (data_.data()[1] == 0x9891) {
            data_.clear();
            data_.AddChar('v'); data_.AddChar('i'); data_.AddChar('d'); data_.AddChar('e'); data_.AddChar('o');
            break;
          }
        }
        if (data_.data()[0] == 0x8be6) {
          if (data_.data()[1] == 0x60c5) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('e'); data_.AddChar('t'); data_.AddChar('a'); data_.AddChar('i'); data_.AddChar('l'); data_.AddChar('s');
            break;
          }
          if (data_.data()[1] == 0x89e3) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('d');
            break;
          }
        }
        if (data_.data()[0] == 0x8f68) {
          if (data_.data()[1] == 0x9053) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('r'); data_.AddChar('a'); data_.AddChar('c'); data_.AddChar('k');
            break;
          }
        }
        if (data_.data()[0] == 0x8f93) {
          if (data_.data()[1] == 0x5165) {
            data_.clear();
            data_.AddChar('i'); data_.AddChar('n'); data_.AddChar('p'); data_.AddChar('u'); data_.AddChar('t');
            break;
          }
          if (data_.data()[1] == 0x51fa) {
            data_.clear();
            data_.AddChar('o'); data_.AddChar('u'); data_.AddChar('t'); data_.AddChar('p'); data_.AddChar('u'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x9009) {
          if (data_.data()[1] == 0x9879) {
            data_.clear();
            data_.AddChar('o'); data_.AddChar('p'); data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('o'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x91cd) {
          if (data_.data()[1] == 0x8981) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('t'); data_.AddChar('r'); data_.AddChar('o'); data_.AddChar('n'); data_.AddChar('g');
            break;
          }
        }
        if (data_.data()[0] == 0x952e) {
          if (data_.data()[1] == 0x76d8) {
            data_.clear();
            data_.AddChar('k'); data_.AddChar('b'); data_.AddChar('d');
            break;
          }
        }
        if (data_.data()[0] == 0x97f3) {
          if (data_.data()[1] == 0x9891) {
            data_.clear();
            data_.AddChar('a'); data_.AddChar('u'); data_.AddChar('d'); data_.AddChar('i'); data_.AddChar('o');
            break;
          }
        }
        if (data_.data()[0] == 0x9875) {
          if (data_.data()[1] == 0x5934) {
            data_.clear();
            data_.AddChar('h'); data_.AddChar('e'); data_.AddChar('a'); data_.AddChar('d');
            break;
          }
          if (data_.data()[1] == 0x5c3e) {
            data_.clear();
            data_.AddChar('f'); data_.AddChar('o'); data_.AddChar('o'); data_.AddChar('t'); data_.AddChar('e'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x6570) {
          if (data_.data()[1] == 0x5b66) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('a'); data_.AddChar('t'); data_.AddChar('h');
            break;
          }
          if (data_.data()[1] == 0x5b57) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x884c) {
          if (data_.data()[1] == 0x7ec4) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('r'); data_.AddChar('o'); data_.AddChar('w');
            break;
          }
        }
        if (data_.data()[0] == 0x77e9) {
          if (data_.data()[1] == 0x5f62) {
            data_.clear();
            data_.AddChar('r'); data_.AddChar('e'); data_.AddChar('c'); data_.AddChar('t');
            break;
          }
        }
        break;
      case 3:
        if (data_.data()[0] == 0x4e0b) {
          if (data_.data()[1] == 0x5212 && data_.data()[2] == 0x7ebf) {
            data_.clear();
            data_.AddChar('u');
            break;
          }
        }
        if (data_.data()[0] == 0x4ee3) {
          if (data_.data()[1] == 0x811a && data_.data()[2] == 0x672c) {
            data_.clear();
            data_.AddChar('n'); data_.AddChar('o'); data_.AddChar('s'); data_.AddChar('c'); data_.AddChar('r'); data_.AddChar('i'); data_.AddChar('p'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x4f20) {
          if (data_.data()[1] == 0x9001 && data_.data()[2] == 0x95e8) {
            data_.clear();
            data_.AddChar('p'); data_.AddChar('o'); data_.AddChar('r'); data_.AddChar('t'); data_.AddChar('a'); data_.AddChar('l');
            break;
          }
        }
        if (data_.data()[0] == 0x5143) {
          if (data_.data()[1] == 0x6570 && data_.data()[2] == 0x636e) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('e'); data_.AddChar('t'); data_.AddChar('a');
            break;
          }
        }
        if (data_.data()[0] == 0x6807) {
          if (data_.data()[1] == 0x8bc6 && data_.data()[2] == 0x7b26) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('i');
            break;
          }
        }
        if (data_.data()[0] == 0x8fd0) {
          if (data_.data()[1] == 0x7b97 && data_.data()[2] == 0x7b26) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('o');
            break;
          }
        }
        if (data_.data()[0] == 0x77e2) {
          if (data_.data()[1] == 0x91cf && data_.data()[2] == 0x56fe) {
            data_.clear();
            data_.AddChar('s'); data_.AddChar('v'); data_.AddChar('g');
            break;
          }
        }
        if (data_.data()[0] == 0x5220) {
          if (data_.data()[1] == 0x9664 && data_.data()[2] == 0x7ebf) {
            data_.clear();
            data_.AddChar('s');
            break;
          }
        }
        if (data_.data()[0] == 0x5355) {
          if (data_.data()[1] == 0x5143 && data_.data()[2] == 0x683c) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('d');
            break;
          }
        }
        if (data_.data()[0] == 0x5bf9) {
          if (data_.data()[1] == 0x8bdd && data_.data()[2] == 0x6846) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('i'); data_.AddChar('a'); data_.AddChar('l'); data_.AddChar('o'); data_.AddChar('g');
            break;
          }
        }
        if (data_.data()[0] == 0x63a7) {
          if (data_.data()[1] == 0x4ef6 && data_.data()[2] == 0x7ec4) {
            data_.clear();
            data_.AddChar('f'); data_.AddChar('i'); data_.AddChar('e'); data_.AddChar('l'); data_.AddChar('d'); data_.AddChar('s'); data_.AddChar('e'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x6587) {
          if (data_.data()[1] == 0x672c && data_.data()[2] == 0x57df) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('e'); data_.AddChar('x'); data_.AddChar('t'); data_.AddChar('a'); data_.AddChar('r'); data_.AddChar('e'); data_.AddChar('a');
            break;
          }
        }
        if (data_.data()[0] == 0x6807) {
          if (data_.data()[1] == 0x9898) {
            if (data_.data()[2] == 0x31) {
              data_.clear();
              data_.AddChar('h'); data_.AddChar('1');
              break;
            }
            if (data_.data()[2] == 0x32) {
              data_.clear();
              data_.AddChar('h'); data_.AddChar('2');
              break;
            }
            if (data_.data()[2] == 0x33) {
              data_.clear();
              data_.AddChar('h'); data_.AddChar('3');
              break;
            }
            if (data_.data()[2] == 0x34) {
              data_.clear();
              data_.AddChar('h'); data_.AddChar('4');
              break;
            }
            if (data_.data()[2] == 0x35) {
              data_.clear();
              data_.AddChar('h'); data_.AddChar('5');
              break;
            }
            if (data_.data()[2] == 0x36) {
              data_.clear();
              data_.AddChar('h'); data_.AddChar('6');
              break;
            }
          }
        }
        if (data_.data()[0] == 0x6c34) {
          if (data_.data()[1] == 0x5e73 && data_.data()[2] == 0x7ebf) {
            data_.clear();
            data_.AddChar('h'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x77e2) {
          if (data_.data()[1] == 0x91cf && data_.data()[2] == 0x56fe) {
            data_.clear();
            data_.AddChar('S'); data_.AddChar('V'); data_.AddChar('G');
            break;
          }
        }
        if (data_.data()[0] == 0x77ed) {
          if (data_.data()[1] == 0x5f15 && data_.data()[2] == 0x6587) {
            data_.clear();
            data_.AddChar('q');
            break;
          }
        }
        if (data_.data()[0] == 0x7ec4) {
          if (data_.data()[1] == 0x6807 && data_.data()[2] == 0x9898) {
            data_.clear();
            data_.AddChar('l'); data_.AddChar('e'); data_.AddChar('g'); data_.AddChar('e'); data_.AddChar('n'); data_.AddChar('d');
            break;
          }
        }
        if (data_.data()[0] == 0x8ba1) {
          if (data_.data()[1] == 0x91cf && data_.data()[2] == 0x8868) {
            data_.clear();
            data_.AddChar('m'); data_.AddChar('e'); data_.AddChar('t'); data_.AddChar('e'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x8d85) {
          if (data_.data()[1] == 0x94fe && data_.data()[2] == 0x63a5) {
            data_.clear();
            data_.AddChar('a');
            break;
          }
        }
        if (data_.data()[0] == 0x8f6f) {
          if (data_.data()[1] == 0x6362 && data_.data()[2] == 0x884c) {
            data_.clear();
            data_.AddChar('w'); data_.AddChar('b'); data_.AddChar('r');
            break;
          }
        }
        if (data_.data()[0] == 0x8fdb) {
          if (data_.data()[1] == 0x5ea6 && data_.data()[2] == 0x6761) {
            data_.clear();
            data_.AddChar('p'); data_.AddChar('r'); data_.AddChar('o'); data_.AddChar('g'); data_.AddChar('r'); data_.AddChar('e'); data_.AddChar('s'); data_.AddChar('s');
            break;
          }
        }
        if (data_.data()[0] == 0x9009) {
          if (data_.data()[1] == 0x62e9) {
            if (data_.data()[2] == 0x6846) {
              data_.clear();
              data_.AddChar('s'); data_.AddChar('e'); data_.AddChar('l'); data_.AddChar('e'); data_.AddChar('c'); data_.AddChar('t');
              break;
            }
          }
          if (data_.data()[1] == 0x9879) {
            if (data_.data()[2] == 0x7ec4) {
              data_.clear();
              data_.AddChar('o'); data_.AddChar('p'); data_.AddChar('t'); data_.AddChar('g'); data_.AddChar('r'); data_.AddChar('o'); data_.AddChar('u'); data_.AddChar('p');
              break;
            }
          }
        }
        if (data_.data()[0] == 0x957f) {
          if (data_.data()[1] == 0x5f15 && data_.data()[2] == 0x6587) {
            data_.clear();
            data_.AddChar('b'); data_.AddChar('l'); data_.AddChar('o'); data_.AddChar('c'); data_.AddChar('k'); data_.AddChar('q'); data_.AddChar('u'); data_.AddChar('o'); data_.AddChar('t'); data_.AddChar('e');
            break;
          }
        }
        break;
      case 4:
        if (data_.data()[0] == 0x5185) {
          if (data_.data()[1] == 0x5bb9 && data_.data()[2] == 0x533a && data_.data()[3] == 0x57df) {
            data_.clear();
            data_.AddChar('a'); data_.AddChar('r'); data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('c'); data_.AddChar('l'); data_.AddChar('e');
            break;
          }
        }
        if (data_.data()[0] == 0x53cc) {
          if (data_.data()[1] == 0x5411) {
            if (data_.data()[2] == 0x91cd) {
              if (data_.data()[3] == 0x8f7d) {
                data_.clear();
                data_.AddChar('b'); data_.AddChar('d'); data_.AddChar('o');
                break;
              }
            }
            if (data_.data()[2] == 0x9694) {
              if (data_.data()[3] == 0x79bb) {
                data_.clear();
                data_.AddChar('b'); data_.AddChar('d'); data_.AddChar('i');
                break;
              }
            }
          }
        }
        if (data_.data()[0] == 0x56fe) {
          if (data_.data()[1] == 0x5f62 && data_.data()[2] == 0x6807 && data_.data()[3] == 0x9898) {
            data_.clear();
            data_.AddChar('f'); data_.AddChar('i'); data_.AddChar('g'); data_.AddChar('c'); data_.AddChar('a'); data_.AddChar('p'); data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('o'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x5934) {
          if (data_.data()[1] == 0x5355 && data_.data()[2] == 0x5143 && data_.data()[3] == 0x683c) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('h');
            break;
          }
          if (data_.data()[1] == 0x5355 && data_.data()[2] == 0x005f && data_.data()[3] == 0x683c) {
            data_.clear();
            data_.AddChar('t'); data_.AddChar('h');
            break;
          }
        }
        if (data_.data()[0] == 0x8868 && data_.data()[1] == 0x5934 && data_.data()[2] == 0x005f && data_.data()[3] == 0x683c) {
          data_.clear();
          data_.AddChar('t'); data_.AddChar('h');
          break;
        }
        if (data_.data()[0] == 0x6570) {
          if (data_.data()[1] == 0x636e && data_.data()[2] == 0x5217 && data_.data()[3] == 0x8868) {
            data_.clear();
            data_.AddChar('d'); data_.AddChar('a'); data_.AddChar('t'); data_.AddChar('a'); data_.AddChar('l'); data_.AddChar('i'); data_.AddChar('s'); data_.AddChar('t');
            break;
          }
        }
        if (data_.data()[0] == 0x6ce8) {
          if (data_.data()[1] == 0x97f3) {
            if (data_.data()[2] == 0x5907) {
              if (data_.data()[3] == 0x9009) {
                data_.clear();
                data_.AddChar('r'); data_.AddChar('p');
                break;
              }
            }
            if (data_.data()[2] == 0x6587) {
              if (data_.data()[3] == 0x672c) {
                data_.clear();
                data_.AddChar('r'); data_.AddChar('b');
                break;
              }
            }
            if (data_.data()[2] == 0x8bed) {
              if (data_.data()[3] == 0x4e49) {
                data_.clear();
                data_.AddChar('r'); data_.AddChar('t'); data_.AddChar('c');
                break;
              }
            }
          }
        }
        if (data_.data()[0] == 0x8868) {
          if (data_.data()[1] == 0x683c && data_.data()[2] == 0x6807 && data_.data()[3] == 0x9898) {
            data_.clear();
            data_.AddChar('c'); data_.AddChar('a'); data_.AddChar('p'); data_.AddChar('t'); data_.AddChar('i'); data_.AddChar('o'); data_.AddChar('n');
            break;
          }
        }
        if (data_.data()[0] == 0x9644) {
          if (data_.data()[1] == 0x52a0 && data_.data()[2] == 0x533a && data_.data()[3] == 0x57df) {
            data_.clear();
            data_.AddChar('a'); data_.AddChar('s'); data_.AddChar('i'); data_.AddChar('d'); data_.AddChar('e');
            break;
          }
        }
        break;
      default:
        break;
    }
  }

  // Returns the converted (English) name, converting Chinese names on first
  // call. Callers that consume the token name as an element tag name (e.g.
  // the tree builder) should use this instead of GetName().
  const DataVector& EnsureConvertedName() {
    DCHECK(type_ == kStartTag || type_ == kEndTag || type_ == DOCTYPE);
    ConvertName();
    return data_;
  }

  const DataVector& GetName() const {
    DCHECK(type_ == kStartTag || type_ == kEndTag || type_ == DOCTYPE);
    return data_;
  }

  const DataVector& GetProcessingInstructionTarget() const {
    DCHECK(type_ == kProcessingInstruction);
    return *processing_instruction_target_;
  }
  ALWAYS_INLINE void AppendToName(UChar character) {
    DCHECK(type_ == kStartTag || type_ == kEndTag || type_ == DOCTYPE ||
           type_ == kProcessingInstruction);
    DCHECK(character);
    data_.AddChar(character);
  }

  /* DOCTYPE Tokens */

  bool ForceQuirks() const {
    DCHECK_EQ(type_, DOCTYPE);
    return doctype_data_->force_quirks_;
  }

  void SetForceQuirks() {
    DCHECK_EQ(type_, DOCTYPE);
    doctype_data_->force_quirks_ = true;
  }

  void BeginDOCTYPE() {
    DCHECK_EQ(type_, kUninitialized);
    type_ = DOCTYPE;
    doctype_data_ = std::make_unique<DoctypeData>();
  }

  void BeginDOCTYPE(UChar character) {
    DCHECK(character);
    BeginDOCTYPE();
    data_.AddChar(character);
  }

  // FIXME: Distinguish between a missing public identifer and an empty one.
  const Vector<UChar>& PublicIdentifier() const {
    DCHECK_EQ(type_, DOCTYPE);
    return doctype_data_->public_identifier_;
  }

  // FIXME: Distinguish between a missing system identifer and an empty one.
  const Vector<UChar>& SystemIdentifier() const {
    DCHECK_EQ(type_, DOCTYPE);
    return doctype_data_->system_identifier_;
  }

  void SetPublicIdentifierToEmptyString() {
    DCHECK_EQ(type_, DOCTYPE);
    doctype_data_->has_public_identifier_ = true;
    doctype_data_->public_identifier_.clear();
  }

  void SetSystemIdentifierToEmptyString() {
    DCHECK_EQ(type_, DOCTYPE);
    doctype_data_->has_system_identifier_ = true;
    doctype_data_->system_identifier_.clear();
  }

  void AppendToPublicIdentifier(UChar character) {
    DCHECK(character);
    DCHECK_EQ(type_, DOCTYPE);
    DCHECK(doctype_data_->has_public_identifier_);
    doctype_data_->public_identifier_.push_back(character);
  }

  void AppendToSystemIdentifier(UChar character) {
    DCHECK(character);
    DCHECK_EQ(type_, DOCTYPE);
    DCHECK(doctype_data_->has_system_identifier_);
    doctype_data_->system_identifier_.push_back(character);
  }

  std::unique_ptr<DoctypeData> ReleaseDoctypeData() {
    return std::move(doctype_data_);
  }

  /* Start/End Tag Tokens */

  bool SelfClosing() const {
    DCHECK(type_ == kStartTag || type_ == kEndTag);
    return self_closing_;
  }

  void SetSelfClosing() {
    DCHECK(type_ == kStartTag || type_ == kEndTag);
    self_closing_ = true;
  }

  ALWAYS_INLINE void BeginStartTag(UChar character) {
    DCHECK(character);
    DCHECK_EQ(type_, kUninitialized);
    type_ = kStartTag;
    self_closing_ = false;
    DCHECK(!current_attribute_);
    DCHECK(attributes_.empty());

    data_.AddChar(character);
  }

  ALWAYS_INLINE void BeginStartTag(LChar character) {
    DCHECK(character);
    DCHECK_EQ(type_, kUninitialized);
    type_ = kStartTag;
    self_closing_ = false;
    DCHECK(!current_attribute_);
    DCHECK(attributes_.empty());

    data_.AddChar(character);
  }

  ALWAYS_INLINE void BeginEndTag(UChar character) {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kEndTag;
    self_closing_ = false;
    DCHECK(!current_attribute_);
    DCHECK(attributes_.empty());

    data_.AddChar(character);
  }

  ALWAYS_INLINE void BeginEndTag(LChar character) {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kEndTag;
    self_closing_ = false;
    DCHECK(!current_attribute_);
    DCHECK(attributes_.empty());

    data_.AddChar(character);
  }

  ALWAYS_INLINE void BeginEndTag(const LCharLiteralBuffer<32>& characters) {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kEndTag;
    self_closing_ = false;
    DCHECK(!current_attribute_);
    DCHECK(attributes_.empty());

    data_.AppendLiteral(characters);
  }

  ALWAYS_INLINE void BeginEndTag(const UCharLiteralBuffer<32>& characters) {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kEndTag;
    self_closing_ = false;
    DCHECK(!current_attribute_);
    DCHECK(attributes_.empty());

    for (wtf_size_t zi = 0; zi < characters.size(); zi++) {
      data_.AddChar(characters[zi]);
    }
  }

  ALWAYS_INLINE void AddNewAttribute(UChar character) {
    DCHECK(type_ == kStartTag || type_ == kEndTag);
    attributes_.Grow(attributes_.size() + 1);
    current_attribute_ = &attributes_.back();
    current_attribute_->AppendToName(character);
  }

  ALWAYS_INLINE void AppendToAttributeName(UChar character) {
    DCHECK(character);
    DCHECK(type_ == kStartTag || type_ == kEndTag);
    current_attribute_->AppendToName(character);
  }

  ALWAYS_INLINE void AppendToAttributeValue(UChar character) {
    DCHECK(character);
    DCHECK(type_ == kStartTag || type_ == kEndTag);
    current_attribute_->AppendToValue(character);
  }

  const AttributeList& Attributes() const {
    DCHECK(type_ == kStartTag || type_ == kEndTag);
    return attributes_;
  }

  const Attribute* GetAttributeItem(const QualifiedName& name) const {
    for (unsigned i = 0; i < attributes_.size(); ++i) {
      if (attributes_.at(i).GetName() == name.LocalName())
        return &attributes_.at(i);
    }
    return nullptr;
  }

  /* Character Tokens */

  // Starting a character token works slightly differently than starting
  // other types of tokens because we want to save a per-character branch.
  ALWAYS_INLINE void EnsureIsCharacterToken() {
    DCHECK(type_ == kUninitialized || type_ == kCharacter);
    type_ = kCharacter;
  }

  const DataVector& Characters() const {
    DCHECK_EQ(type_, kCharacter);
    return data_;
  }

  ALWAYS_INLINE void AppendToCharacter(char character) {
    DCHECK_EQ(type_, kCharacter);
    data_.AddChar(character);
  }

  ALWAYS_INLINE void AppendToCharacter(UChar character) {
    DCHECK_EQ(type_, kCharacter);
    data_.AddChar(character);
  }

  ALWAYS_INLINE void AppendToCharacter(
      const LCharLiteralBuffer<32>& characters) {
    DCHECK_EQ(type_, kCharacter);
    data_.AppendLiteral(characters);
  }

  ALWAYS_INLINE void AppendToCharacter(
      const UCharLiteralBuffer<32>& characters) {
    DCHECK_EQ(type_, kCharacter);
    for (wtf_size_t zi = 0; zi < characters.size(); zi++) {
      data_.AddChar(characters[zi]);
    }
  }

  ALWAYS_INLINE void AppendToProcessingInstructionTarget(UChar character) {
    DCHECK_EQ(type_, kProcessingInstruction);
    DCHECK(processing_instruction_target_);
    processing_instruction_target_->AddChar(character);
  }

  bool HasEntity() const { return has_entity_; }
  void SetHasEntity() { has_entity_ = true; }

  /* Comment Tokens */

  const DataVector& Comment() const {
    DCHECK_EQ(type_, kComment);
    return data_;
  }

  ALWAYS_INLINE void BeginComment() {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kComment;
  }

  ALWAYS_INLINE void AppendToComment(UChar character) {
    DCHECK(character);
    DCHECK_EQ(type_, kComment);
    data_.AddChar(character);
  }

  ALWAYS_INLINE void AppendToProcessingInstructionData(UChar character) {
    DCHECK(character);
    DCHECK_EQ(type_, kProcessingInstruction);
    data_.AddChar(character);
  }

  /* Processing Instruction Tokens */

  ALWAYS_INLINE void BeginProcessingInstruction() {
    DCHECK_EQ(type_, kUninitialized);
    type_ = kProcessingInstruction;
    processing_instruction_target_ = std::make_optional<DataVector>();
  }

 private:
  DataVector data_;

  AttributeList attributes_;

  // A pointer into attributes_ used during lexing.
  // UnprotectedInReleaseForPerformance: Based on analysis of Speedometer 3
  // (Editor-TipTap).
  raw_ptr<Attribute, UnprotectedInReleaseForPerformance | DanglingUntriaged>
      current_attribute_ = nullptr;

  // For DOCTYPE
  std::unique_ptr<DoctypeData> doctype_data_;

  std::optional<DataVector> processing_instruction_target_;

  TokenType type_ = kUninitialized;

  // True if this token contains an entity reference.
  bool has_entity_ = false;

  // For StartTag and EndTag
  bool self_closing_;
};

#ifndef NDEBUG
const char* ToString(HTMLToken::TokenType);
#endif

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PARSER_HTML_TOKEN_H_

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

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PARSER_ATOMIC_HTML_TOKEN_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PARSER_ATOMIC_HTML_TOKEN_H_

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>

#include "base/check_op.h"
#include "base/notreached.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/dom/attribute.h"
#include "third_party/blink/renderer/core/html/parser/html_token.h"
#include "third_party/blink/renderer/core/html_element_attribute_name_lookup_trie.h"
#include "third_party/blink/renderer/core/html_element_lookup_trie.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/hash_set.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string_hash.h"
#include "third_party/blink/renderer/platform/wtf/text/character_visitor.h"

namespace blink {

class AtomicHTMLToken;

// HTMLTokenName represents a parsed token name (the local name of a
// QualifiedName). The token name contains the local name as an AtomicString and
// if the name is a valid html tag name, an HTMLTag. As this class is created
// from tokenized input, it does not know the namespace, the namespace is
// determined later in parsing (see HTMLTreeBuilder and HTMLStackItem).
class CORE_EXPORT HTMLTokenName {
 public:
  explicit HTMLTokenName(html_names::HTMLTag tag) : tag_(tag) {
    if (tag != html_names::HTMLTag::kUnknown)
      local_name_ = html_names::TagToQualifiedName(tag).LocalName();
  }

  // Returns an HTMLTokenName for the specified string. This function looks up
  // the HTMLTag from the supplied string.
  static HTMLTokenName FromLocalName(const AtomicString& local_name) {
    if (local_name.empty())
      return HTMLTokenName(html_names::HTMLTag::kUnknown);

    return VisitCharacters(local_name, [&local_name](auto chars) {
      return HTMLTokenName(LookupHtmlTag(chars), local_name);
    });
  }

  bool operator==(const HTMLTokenName& other) const {
    return other.local_name_ == local_name_;
  }

  bool IsValidHTMLTag() const { return tag_ != html_names::HTMLTag::kUnknown; }

  html_names::HTMLTag GetHTMLTag() const { return tag_; }

  const AtomicString& GetLocalName() const { return local_name_; }

 private:
  // For access to constructor.
  friend class AtomicHTMLToken;

  explicit HTMLTokenName(html_names::HTMLTag tag, const AtomicString& name)
      : tag_(tag), local_name_(name) {
#if DCHECK_IS_ON()
    if (tag == html_names::HTMLTag::kUnknown) {
      // If the tag is unknown, then `name` must either be empty, or not
      // identify any other HTMLTag.
      if (!name.empty()) {
        VisitCharacters(name, [](auto chars) {
          DCHECK_EQ(html_names::HTMLTag::kUnknown, LookupHtmlTag(chars));
        });
      }
    }
#endif
  }

  // This constructor is intended for use by AtomicHTMLToken when it is known
  // the string is not a known html tag.
  explicit HTMLTokenName(const AtomicString& name)
      : HTMLTokenName(html_names::HTMLTag::kUnknown, name) {}

  // Store both the tag and the name. The tag is enough to lookup the name, but
  // enough code makes use of the name that's it's worth caching (this performs
  // a bit better than using a variant for the two and looking up on demand).
  html_names::HTMLTag tag_;
  AtomicString local_name_;
};

class CORE_EXPORT AtomicHTMLToken {
  STACK_ALLOCATED();

 public:
  bool ForceQuirks() const {
    DCHECK_EQ(type_, HTMLToken::DOCTYPE);
    return doctype_data_->force_quirks_;
  }

  HTMLToken::TokenType GetType() const { return type_; }

  // TODO(sky): for consistency, rename to GetLocalName().
  const AtomicString& GetName() const {
    DCHECK(UsesName());
    return name_.GetLocalName();
  }

  void SetTokenName(const HTMLTokenName& name) {
    DCHECK(UsesName());
    name_ = name;
  }

  html_names::HTMLTag GetHTMLTag() const { return name_.GetHTMLTag(); }

  bool IsValidHTMLTag() const { return name_.IsValidHTMLTag(); }

  const HTMLTokenName& GetTokenName() const { return name_; }

  bool SelfClosing() const {
    DCHECK(type_ == HTMLToken::kStartTag || type_ == HTMLToken::kEndTag);
    return self_closing_;
  }

  void SetSelfClosingToFalse() {
    DCHECK(self_closing_);
    DCHECK_EQ(type_, HTMLToken::kStartTag);
    DCHECK_EQ(GetHTMLTag(), html_names::HTMLTag::kScript);
    self_closing_ = false;
  }

  bool HasDuplicateAttribute() const { return duplicate_attribute_; }

  Attribute* GetAttributeItem(const QualifiedName& attribute_name) {
    DCHECK(UsesAttributes());
    return FindAttributeInVector(attributes_, attribute_name);
  }

  Vector<Attribute, kAttributePrealloc>& Attributes() {
    DCHECK(UsesAttributes());
    return attributes_;
  }

  const Vector<Attribute, kAttributePrealloc>& Attributes() const {
    DCHECK(UsesAttributes());
    return attributes_;
  }

  const String& Characters() const {
    DCHECK_EQ(type_, HTMLToken::kCharacter);
    return data_;
  }

  const String& Comment() const {
    DCHECK_EQ(type_, HTMLToken::kComment);
    return data_;
  }

  const String& ProcessingInstructionTarget() const {
    DCHECK_EQ(type_, HTMLToken::kProcessingInstruction);
    return processing_instruction_target_;
  }

  const String& ProcessingInstructionData() const {
    DCHECK_EQ(type_, HTMLToken::kProcessingInstruction);
    return data_;
  }

  // FIXME: Distinguish between a missing public identifer and an empty one.
  Vector<UChar>& PublicIdentifier() const {
    DCHECK_EQ(type_, HTMLToken::DOCTYPE);
    return doctype_data_->public_identifier_;
  }

  // FIXME: Distinguish between a missing system identifer and an empty one.
  Vector<UChar>& SystemIdentifier() const {
    DCHECK_EQ(type_, HTMLToken::DOCTYPE);
    return doctype_data_->system_identifier_;
  }

  bool HasEntity() const { return has_entity_; }

  explicit AtomicHTMLToken(HTMLToken& token)
      : type_(token.GetType()),
        name_(HTMLTokenNameFromToken(token)),
        has_entity_(token.HasEntity()) {
    switch (type_) {
      case HTMLToken::kUninitialized:
        NOTREACHED();
      case HTMLToken::DOCTYPE:
        doctype_data_ = token.ReleaseDoctypeData();
        break;
      case HTMLToken::kEndOfFile:
        break;
      case HTMLToken::kStartTag:
        [[fallthrough]];
      case HTMLToken::kEndTag: {
        self_closing_ = token.SelfClosing();
        const HTMLToken::AttributeList& attributes = token.Attributes();

        // This limit is set fairly arbitrarily; the main point is to avoid
        // DDoS opportunities or similar with O(n²) behavior by setting lots
        // of attributes.
        const int kMinimumNumAttributesToDedupWithHash = 10;

        if (attributes.size() >= kMinimumNumAttributesToDedupWithHash) {
          InitializeAttributes</*DedupWithHash=*/true>(token.Attributes());
        } else if (attributes.size()) {
          InitializeAttributes</*DedupWithHash=*/false>(token.Attributes());
        }
        break;
      }
      case HTMLToken::kCharacter:
      case HTMLToken::kComment:
        data_ = token.Data().AsString();
        break;
      case HTMLToken::kProcessingInstruction:
        data_ = token.Data().AsString();
        processing_instruction_target_ =
            token.GetProcessingInstructionTarget().AsString();
        break;
    }
  }

  explicit AtomicHTMLToken(HTMLToken::TokenType type)
      : type_(type), name_(html_names::HTMLTag::kUnknown) {}

  AtomicHTMLToken(HTMLToken::TokenType type,
                  html_names::HTMLTag tag,
                  const Vector<Attribute>& attributes = Vector<Attribute>())
      : type_(type), name_(tag), attributes_(attributes) {
    DCHECK(UsesName());
  }

  AtomicHTMLToken(HTMLToken::TokenType type,
                  const HTMLTokenName& name,
                  const Vector<Attribute>& attributes = Vector<Attribute>())
      : type_(type), name_(name), attributes_(attributes) {
    DCHECK(UsesName());
  }

  AtomicHTMLToken(const AtomicHTMLToken&) = delete;
  AtomicHTMLToken& operator=(const AtomicHTMLToken&) = delete;

#ifndef NDEBUG
  void Show() const;
#endif

 private:
  static HTMLTokenName HTMLTokenNameFromToken(HTMLToken& token) {
    switch (token.GetType()) {
      case HTMLToken::DOCTYPE:
        // Doctype name may be empty, but not start/end tags.
        if (token.GetName().IsEmpty())
          return HTMLTokenName(html_names::HTMLTag::kUnknown);
        [[fallthrough]];
      case HTMLToken::kStartTag:
      case HTMLToken::kEndTag: {
        const html_names::HTMLTag html_tag = LookupHtmlTag(token.GetName());
        if (html_tag != html_names::HTMLTag::kUnknown)
          return HTMLTokenName(html_tag);
        return HTMLTokenName::FromLocalName(
            token.EnsureConvertedName().AsAtomicString());
      }
      default:
        return HTMLTokenName(html_names::HTMLTag::kUnknown);
    }
  }

  // Sets up and deduplicates attributes.
  //
  // We can deduplicate attributes in two ways; using a hash table
  // (DedupWithHash=true) or by simple linear scanning (DedupWithHash=false).
  // If we don't have many attributes, the linear scan is cheaper than
  // setting up and searching in a hash table, even though the big-O
  // complexity is higher. Thus, we use the hash table only if the caller
  // expects a lot of attributes.
  template <bool DedupWithHash>
  ALWAYS_INLINE void InitializeAttributes(
      const HTMLToken::AttributeList& attributes);

  bool UsesName() const;

  bool UsesAttributes() const;

  HTMLToken::TokenType type_;

  // "name" for DOCTYPE, StartTag, and EndTag
  HTMLTokenName name_;

  // "data" for Comment, "characters" for Character
  String data_;

  // For DOCTYPE
  std::unique_ptr<DoctypeData> doctype_data_;

  // For DOM Parts
  std::unique_ptr<DOMPartData> dom_part_data_;

  // For Processing Instructions
  String processing_instruction_target_;

  DOMPartsNeeded dom_parts_needed_;

  // For StartTag and EndTag
  bool self_closing_ = false;

  bool duplicate_attribute_ = false;

  // True if this token contains an entity reference.
  bool has_entity_ = false;

  Vector<Attribute, kAttributePrealloc> attributes_;
};

namespace {
// 中文属性值/属性名 → 英文等价映射表（表驱动，UTF-16 字面量 + memcmp 内容比较；
// 与 html_token.h 的 kZhTagTable 同风格，不用手写 if 嵌套）。
struct ZhAttrEntry {
  const char16_t* zh;
  size_t zh_len;
  const char* en;
};

// 中文属性值映射表（type/target/rel/loading/direction 等值）。
constexpr ZhAttrEntry kZhAttrValueTable[] = {
    // type 值
    {u"文本", 2, "text"},      // 文本
    {u"密码", 2, "password"},  // 密码
    {u"提交", 2, "submit"},    // 提交
    {u"重置", 2, "reset"},     // 重置
    {u"按钮", 2, "button"},    // 按钮
    {u"文件", 2, "file"},      // 文件
    {u"隐藏", 2, "hidden"},    // 隐藏
    {u"日期", 2, "date"},      // 日期
    {u"邮箱", 2, "email"},     // 邮箱
    {u"数字", 2, "number"},    // 数字
    {u"范围", 2, "range"},     // 范围
    {u"搜索", 2, "search"},    // 搜索
    {u"电话", 2, "tel"},       // 电话
    {u"链接", 2, "url"},       // 链接
    {u"颜色", 2, "color"},     // 颜色
    // target 值
    {u"空白", 2, "_blank"},    // 空白
    {u"自身", 2, "_self"},     // 自身
    {u"父级", 2, "_parent"},   // 父级
    {u"顶级", 2, "_top"},      // 顶级
    // rel 值
    {u"图标", 2, "icon"},      // 图标
    // loading 值
    {u"急切", 2, "eager"},     // 急切
    // 自动
    {u"自动", 2, "autoplay"},  // 自动(播放)
    // 预取（原 length==3 分支笔误，实际 2 字）
    {u"预取", 2, "prefetch"},  // 预取
    // 匿名（原 length==4 分支笔误，实际 2 字）
    {u"匿名", 2, "anonymous"},  // 匿名
    // type 值（3 字）
    {u"复选框", 3, "checkbox"},  // 复选框
    {u"单选框", 3, "radio"},     // 单选框
    // rel 值（3 字）
    {u"样式表", 3, "stylesheet"},  // 样式表
    {u"预加载", 3, "preload"},     // 预加载
    {u"无跟随", 3, "nofollow"},    // 无跟随
    // loading 值（3 字）
    {u"懒加载", 3, "lazy"},  // 懒加载
    // 方向（3 字）
    {u"左到右", 3, "ltr"},   // 左到右
    {u"右到左", 3, "rtl"},   // 右到左
    // 自动（4 字）
    {u"自动播放", 4, "autoplay"},   // 自动播放
    {u"自动聚焦", 4, "autofocus"},  // 自动聚焦
};

// 中文属性名映射表。
constexpr ZhAttrEntry kZhAttrNameTable[] = {
    // length == 1
    {u"名", 1, "name"},    // 名
    {u"值", 1, "value"},   // 值
    {u"类", 1, "class"},   // 类
    {u"源", 1, "src"},     // 源
    {u"链", 1, "href"},    // 链
    {u"宽", 1, "width"},   // 宽
    {u"高", 1, "height"},  // 高
    {u"式", 1, "style"},   // 式
    // length == 2
    {u"类型", 2, "type"},      // 类型
    {u"类名", 2, "class"},     // 类名
    {u"标识", 2, "id"},        // 标识
    {u"样式", 2, "style"},     // 样式
    {u"来源", 2, "src"},       // 来源
    {u"标题", 2, "title"},     // 标题
    {u"名称", 2, "name"},      // 名称
    {u"宽度", 2, "width"},     // 宽度
    {u"高度", 2, "height"},    // 高度
    {u"边框", 2, "border"},    // 边框
    {u"对齐", 2, "align"},     // 对齐
    {u"链接", 2, "href"},      // 链接
    {u"角色", 2, "role"},      // 角色
    {u"显示", 2, "hidden"},    // 显示 (mapped to hidden for hidden attribute)
    {u"媒体", 2, "media"},     // 媒体
    {u"目标", 2, "target"},    // 目标
    {u"加载", 2, "loading"},   // 加载
    {u"关系", 2, "rel"},       // 关系
    {u"替代", 2, "alt"},       // 替代
    {u"方向", 2, "dir"},       // 方向
    {u"可见", 2, "hidden"},    // 可见 (hidden when not visible)
    {u"图片", 2, "src"},       // 图片 (for img src)
    {u"只读", 2, "readonly"},  // 只读
    {u"禁用", 2, "disabled"},  // 禁用
    {u"选中", 2, "checked"},   // 选中
    {u"语言", 2, "lang"},      // 语言
    {u"半径", 2, "r"},         // 半径 (SVG)
    {u"填充", 2, "fill"},      // 填充 (SVG)
    {u"描边", 2, "stroke"},    // 描边 (SVG)
    // length == 3
    {u"替代文", 3, "alt"},          // 替代文(本)
    {u"字符集", 3, "charset"},      // 字符集
    {u"数据列", 3, "list"},         // 数据列(表)
    {u"最大值", 3, "max"},          // 最大值
    {u"最小值", 3, "min"},          // 最小值
    {u"占位符", 3, "placeholder"},  // 占位符
    {u"微调器", 3, "controls"},     // 微调器
    {u"横坐标", 3, "x"},            // 横坐标 (SVG)
    {u"纵坐标", 3, "y"},            // 纵坐标 (SVG)
    // length == 4
    {u"自动播放", 4, "autoplay"},      // 自动播放
    {u"微调控件", 4, "controls"},      // 微调控件
    {u"日期时间", 4, "datetime"},      // 日期时间
    {u"图像描述", 4, "alt"},           // 图像描述
    {u"数据属性", 4, "data-*"},        // 数据属性
    {u"名称空间", 4, "xmlns"},         // 名称空间
    {u"描边宽度", 4, "stroke-width"},  // 描边宽度 (SVG)
    {u"跨列", 2, "colspan"},          // 跨列 (表格)
    {u"跨行", 2, "rowspan"},          // 跨行 (表格)
    {u"必填", 2, "required"},         // 必填 (表单)
    {u"多选", 2, "multiple"},         // 多选 (表单)
    {u"内容", 2, "content"},          // 内容 (meta)
    {u"自动补全", 4, "autocomplete"}, // 自动补全 (表单)
    // length == 5
    {u"中心横坐标", 5, "cx"},         // 中心横坐标 (SVG)
    {u"中心纵坐标", 5, "cy"},         // 中心纵坐标 (SVG)
};

// 表驱动查找：data（UTF-16 码元）与条目 UTF-16 字面量内容相等 → 返回英文；否则 nullptr。
inline const char* LookupZhAttr(const ZhAttrEntry* table, size_t table_size,
                                const UChar* data, unsigned length) {
  if (length == 0) {
    return nullptr;
  }
  for (size_t i = 0; i < table_size; ++i) {
    const ZhAttrEntry& entry = table[i];
    if (entry.zh_len != length) {
      continue;
    }
    if (memcmp(data, entry.zh, length * sizeof(UChar)) == 0) {
      return entry.en;
    }
  }
  return nullptr;
}
}  // namespace

// Convert Chinese attribute value to English equivalent.
// Returns nullptr if not a Chinese value. Empty string means boolean attribute.
inline const char* ConvertChineseAttributeValue(const UChar* data, unsigned length) {
  return LookupZhAttr(kZhAttrValueTable,
                      sizeof(kZhAttrValueTable) / sizeof(kZhAttrValueTable[0]),
                      data, length);
}

// Convert Chinese attribute name to English equivalent.
// Returns empty string if not a Chinese attribute name.
// Input is the raw UChar buffer from the HTML token.
inline const char* ConvertChineseAttributeName(const UChar* data, unsigned length) {
  return LookupZhAttr(kZhAttrNameTable,
                      sizeof(kZhAttrNameTable) / sizeof(kZhAttrNameTable[0]),
                      data, length);
}

template <bool DedupWithHash>
void AtomicHTMLToken::InitializeAttributes(
    const HTMLToken::AttributeList& attributes) {
  wtf_size_t size = attributes.size();

  // Track which attributes have already been inserted to avoid N^2
  // behavior with repeated linear searches when populating `attributes_`.
  std::conditional_t<DedupWithHash, HashSet<AtomicString>, int>
      added_attributes;
  if constexpr (DedupWithHash) {
    added_attributes.ReserveCapacityForSize(size);
  }

  // This is only called once, so `attributes_` should be empty.
  DCHECK(attributes_.empty());
  attributes_.ReserveInitialCapacity(size);
  for (const auto& attribute : attributes) {
    if (attribute.NameIsEmpty())
      continue;

    // Convert Chinese attribute names to English before lookup. This only
    // applies to names containing non-ASCII characters, which is when
    // Chinese characters can appear.
    const UChar* name_data = attribute.NameBuffer().data();
    unsigned name_len = attribute.NameBuffer().size();
    bool name_has_non_ascii = false;
    for (unsigned i = 0; i < name_len; i++) {
      if (name_data[i] >= 0x80) {
        name_has_non_ascii = true;
        break;
      }
    }
    AtomicString attr_name;
    QualifiedName name = g_null_name;
    if (name_has_non_ascii) {
      attr_name = attribute.GetName();
      const char* mapped_name = ConvertChineseAttributeName(name_data, name_len);
      if (mapped_name) {
        attr_name = AtomicString(mapped_name);
      }
      if (attr_name.Is8Bit()) {
        name = LookupHTMLAttributeName(attr_name.Span8().data(),
                                       attr_name.length());
      } else {
        name = LookupHTMLAttributeName(attr_name.Span16().data(),
                                       attr_name.length());
      }
      if (name == g_null_name) {
        name = QualifiedName(attr_name);
      }
    } else {
      name = LookupHTMLAttributeName(name_data, name_len);
      if (name == g_null_name) {
        name = QualifiedName(attribute.GetName());
      }
    }

    if constexpr (DedupWithHash) {
      if (!added_attributes.insert(name.LocalName()).is_new_entry) {
        duplicate_attribute_ = true;
        continue;
      }
    } else {
      if (std::ranges::contains(attributes_, name.LocalName(),
                                &Attribute::LocalName)) {
        duplicate_attribute_ = true;
        continue;
      }
    }

    // The string pointer in |value| is null for attributes with no values, but
    // the null atom is used to represent absence of attributes; attributes with
    // no values have the value set to an empty atom instead.
    AtomicString value(attribute.GetValue());
    if (value.IsNull()) {
      value = g_empty_atom;
    }
    // Convert Chinese attribute values to English.
    if (!value.empty() && !value.Is8Bit()) {
      const auto* val_data = value.Span16().data();
      unsigned val_len = value.length();
      bool val_has_non_ascii = false;
      for (unsigned i = 0; i < val_len; i++) {
        if (val_data[i] >= 0x80) {
          val_has_non_ascii = true;
          break;
        }
      }
      if (val_has_non_ascii) {
        const char* mapped_val = ConvertChineseAttributeValue(val_data, val_len);
        if (mapped_val) {
          value = AtomicString(mapped_val);
        }
      }
    }
    attributes_.UncheckedAppend(Attribute(std::move(name), std::move(value)));
  }
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PARSER_ATOMIC_HTML_TOKEN_H_

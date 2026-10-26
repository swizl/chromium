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

#include <cstring>
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

// 中文标签名 → 标准英文标签名。HTML 解析器与 CSS 类型选择器共用此单表，
// 避免双表漂移。含 HTML 标签 + SVG/MathML 内容标签。
struct ZhTagEntry {
  const char16_t* zh;
  size_t zh_len;
  const char* en;
};
inline constexpr ZhTagEntry kZhTagTable[] = {
{u"列", 1, "col"},
        {u"块", 1, "div"},
        {u"段", 1, "p"},
        {u"行", 1, "tr"},
        {u"上标", 2, "sup"},
        {u"下标", 2, "sub"},
        {u"主体", 2, "body"},
        {u"主要", 2, "main"},
        {u"代码", 2, "code"},
        {u"内框", 2, "iframe"},
        {u"列组", 2, "colgroup"},
        {u"删除", 2, "del"},
        {u"区块", 2, "section"},
        {u"区域", 2, "area"},
        {u"原样", 2, "pre"},
        {u"参数", 2, "param"},
        {u"变量", 2, "var"},
        {u"图形", 2, "figure"},
        {u"图片", 2, "picture"},
        {u"地图", 2, "map"},
        {u"地址", 2, "address"},
        {u"基址", 2, "base"},
        {u"页头区块", 4, "header"},
        {u"对象", 2, "object"},
        {u"导航", 2, "nav"},
        {u"小字", 2, "small"},
        {u"嵌入", 2, "embed"},
        {u"引用", 2, "cite"},
        {u"强调", 2, "em"},
        {u"拼音", 2, "rt"},
        {u"按钮", 2, "button"},
        {u"换行", 2, "br"},
        {u"描述", 2, "dl"},
        {u"插入", 2, "ins"},
        {u"插槽", 2, "slot"},
        {u"摘要", 2, "summary"},
        {u"数学", 2, "math"},
        {u"数据", 2, "data"},
        {u"斜体", 2, "i"},
        {u"无序", 2, "ul"},
        {u"时间", 2, "time"},
        {u"有序", 2, "ol"},
        {u"术语", 2, "dt"},
        {u"条目", 2, "li"},
        {u"来源", 2, "source"},
        {u"标签", 2, "label"},
        {u"标题", 2, "title"},
        {u"样式", 2, "style"},
        {u"样本", 2, "samp"},
        {u"模板", 2, "template"},
        {u"注音", 2, "ruby"},
        {u"画布", 2, "canvas"},
        {u"突出", 2, "mark"},
        {u"粗体", 2, "b"},
        {u"缩写", 2, "abbr"},
        {u"网页", 2, "html"},
        {u"脚本", 2, "script"},
        {u"范围", 2, "span"},
        {u"菜单", 2, "menu"},
        {u"表体", 2, "tbody"},
        {u"表单", 2, "form"},
        {u"表头", 2, "thead"},
        {u"表尾", 2, "tfoot"},
        {u"表格", 2, "table"},
        {u"视频", 2, "video"},
        {u"详情", 2, "details"},
        {u"详解", 2, "dd"},
        {u"轨道", 2, "track"},
        {u"输入", 2, "input"},
        {u"输出", 2, "output"},
        {u"选项", 2, "option"},
        {u"重要", 2, "strong"},
        {u"键盘", 2, "kbd"},
        {u"音频", 2, "audio"},
        {u"页头", 2, "head"},
        {u"页尾", 2, "footer"},
        {u"下划线", 3, "u"},
        {u"代脚本", 3, "noscript"},
        {u"传送门", 3, "portal"},
        {u"元数据", 3, "meta"},
        {u"矢量图", 3, "svg"},
        {u"删除线", 3, "s"},
        {u"单元格", 3, "td"},
        {u"对话框", 3, "dialog"},
        {u"控件组", 3, "fieldset"},
        {u"文本域", 3, "textarea"},
        {u"标题1", 3, "h1"},
        {u"标题2", 3, "h2"},
        {u"标题3", 3, "h3"},
        {u"标题4", 3, "h4"},
        {u"标题5", 3, "h5"},
        {u"标题6", 3, "h6"},
        {u"水平线", 3, "hr"},
        {u"短引文", 3, "q"},
        {u"组标题", 3, "legend"},
        {u"计量表", 3, "meter"},
        {u"超链接", 3, "a"},
        {u"软换行", 3, "wbr"},
        {u"进度条", 3, "progress"},
        {u"选择框", 3, "select"},
        {u"选项组", 3, "optgroup"},
        {u"长引文", 3, "blockquote"},
        {u"内容区域", 4, "article"},
        {u"双向重载", 4, "bdo"},
        {u"双向隔离", 4, "bdi"},
        {u"图形标题", 4, "figcaption"},
        {u"头单元格", 4, "th"},
        {u"数据列表", 4, "datalist"},
        {u"注音备注", 4, "rp"},
        {u"注音文本", 4, "rb"},
        {u"注音语义", 4, "rtc"},
        {u"表格标题", 4, "caption"},
        {u"附加区域", 4, "aside"},
        // ---- SVG/MathML 内容标签（映射到标准英文标签，元素工厂按英文名创建正确类型）----
        {u"圆", 1, "circle"},
        {u"线", 1, "line"},
        {u"组", 1, "g"},
        {u"无", 1, "none"},
        {u"根", 1, "mroot"},
        {u"椭圆", 2, "ellipse"},
        {u"矩形", 2, "rect"},
        {u"折线", 2, "polyline"},
        {u"路径", 2, "path"},
        {u"文本", 2, "text"},
        {u"图像", 2, "image"},
        {u"定义", 2, "defs"},
        {u"使用", 2, "use"},
        {u"裁剪", 2, "clipPath"},
        {u"遮罩", 2, "mask"},
        {u"图案", 2, "pattern"},
        {u"停止", 2, "stop"},
        {u"动画", 2, "animate"},
        {u"设置", 2, "set"},
        {u"滤镜", 2, "filter"},
        {u"模糊", 2, "feGaussianBlur"},
        {u"混合", 2, "feBlend"},
        {u"偏移", 2, "feOffset"},
        {u"合并", 2, "feMerge"},
        {u"平铺", 2, "feTile"},
        {u"洪水", 2, "feFlood"},
        {u"符号", 2, "symbol"},
        {u"切换", 2, "switch"},
        {u"视图", 2, "view"},
        {u"位移", 2, "feDisplacementMap"},
        {u"分数", 2, "mfrac"},
        {u"数字", 2, "mn"},
        {u"错误", 2, "merror"},
        {u"字形", 2, "mglyph"},
        {u"占位", 2, "mphantom"},
        {u"行组", 2, "mrow"},
        {u"字符串", 2, "ms"},
        {u"空间", 2, "mspace"},
        {u"上盖", 2, "mover"},
        {u"下盖", 2, "munder"},
        {u"语义", 2, "semantics"},
        {u"动作", 2, "maction"},
        {u"注解", 2, "annotation"},
        {u"多边形", 3, "polygon"},
        {u"平方根", 3, "msqrt"},
        {u"上下标", 3, "msubsup"},
        {u"多脚本", 3, "mmultiscripts"},
        {u"前脚本", 3, "mprescripts"},
        {u"标识符", 3, "mi"},
        {u"运算符", 3, "mo"},
        {u"内边距", 3, "mpadded"},
        {u"上下盖", 3, "munderover"},
        {u"线性渐变", 4, "linearGradient"},
        {u"径向渐变", 4, "radialGradient"},
        {u"变换动画", 4, "animateTransform"},
        {u"运动动画", 4, "animateMotion"},
        {u"合并节点", 4, "feMergeNode"},
        {u"颜色矩阵", 4, "feColorMatrix"},
        {u"漫射光照", 4, "feDiffuseLighting"},
        {u"镜面光照", 4, "feSpecularLighting"},
        {u"图像滤镜", 4, "feImage"},
        {u"对齐标记", 4, "malignmark"},
        {u"位图", 2, "img"},
        {u"外链", 2, "link"},
        {u"定义术语", 4, "dfn"},
        {u"标题组", 3, "hgroup"},
        {u"搜索", 2, "search"},
        {u"文本跨度", 4, "tspan"},
        {u"文本路径", 4, "textPath"},
        {u"标记", 2, "marker"},
        {u"外来对象", 4, "foreignObject"},
        {u"运动路径", 4, "mpath"},
        {u"噪点", 2, "feTurbulence"},
        {u"投影", 2, "feDropShadow"},
        {u"合成", 2, "feComposite"},
        {u"形态", 2, "feMorphology"},
        {u"卷积矩阵", 4, "feConvolveMatrix"},
        {u"分量转移", 4, "feComponentTransfer"},
        {u"远光", 2, "feDistantLight"},
        {u"点光", 2, "fePointLight"},
        {u"聚光", 2, "feSpotLight"},
        {u"函数α", 3, "feFuncA"},
        {u"函数蓝", 3, "feFuncB"},
        {u"函数绿", 3, "feFuncG"},
        {u"函数红", 3, "feFuncR"},
        {u"文档元数据", 5, "metadata"},
        {u"xml注解", 5, "annotation-xml"},
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
    // 中文标签名 → 标准英文标签名（查全局 kZhTagTable，与 CSS 类型选择器共用单表）。

    const size_t len = data_.size();
    if (len == 0) {
      return;
    }
    const UChar* const d = data_.data();
    for (const auto& e : kZhTagTable) {
      if (e.zh_len != len) {
        continue;
      }
      if (memcmp(d, e.zh, len * sizeof(UChar)) == 0) {
        data_.clear();
        for (const char* p = e.en; *p; ++p) {
          data_.AddChar(*p);
        }
        return;
      }
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

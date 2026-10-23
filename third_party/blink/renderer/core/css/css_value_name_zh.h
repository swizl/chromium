// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_CSS_CSS_VALUE_NAME_ZH_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_CSS_CSS_VALUE_NAME_ZH_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/css_value_keywords.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

// 中文序列化名（CSSOM 输出层中文化）。返回空 StringView 表示未覆盖，
// 调用点应回退英文 GetCSSValueNameAs。
CORE_EXPORT StringView GetCSSValueNameZh(CSSValueID value_id);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_CSS_CSS_VALUE_NAME_ZH_H_

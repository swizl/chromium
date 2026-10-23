#!/usr/bin/env python
# Copyright (C) 2013 Google Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from collections import defaultdict

import json5_generator
import template_expander

from make_qualified_names import MakeQualifiedNamesWriter


class MakeElementFactoryWriter(MakeQualifiedNamesWriter):
    default_parameters = {
        'JSInterfaceName': {},
        'Conditional': {},
        'constructorNeedsCreateElementFlags': {},
        'interfaceHeaderDir': {},
        'interfaceName': {},
        'noConstructor': {},
        'noTypeHelpers': {},
        'runtimeEnabled': {},
        'runtimeFlagHasOriginTrial': {},
        'enName': {},
    }
    default_metadata = dict(
        MakeQualifiedNamesWriter.default_metadata,
        **{
            'fallbackInterfaceName': '',
            'fallbackJSInterfaceName': '',
        },
    )
    filters = MakeQualifiedNamesWriter.filters

    def __init__(self, json5_file_paths, output_dir, generate_tag_enum=False):
        super(MakeElementFactoryWriter, self).__init__(
            json5_file_paths, output_dir
        )

        basename = self.namespace.lower() + '_element_factory'
        self._outputs.update(
            {
                (basename + '.h'): self.generate_factory_header,
                (basename + '.cc'): self.generate_factory_implementation,
            }
        )

        fallback_interface = self.tags_json5_file.metadata[
            'fallbackInterfaceName'
        ].strip('"')
        fallback_js_interface = (
            self.tags_json5_file.metadata['fallbackJSInterfaceName'].strip('"')
            or fallback_interface
        )

        interface_counts = defaultdict(int)
        tags = self._template_context['tags']
        # 中文条目（json5 里带 enName 字段关联英文标签）不生成自己的 C++
        # 元素类，而是在 factory 里注册「中文 localName → 英文 Constructor」
        # 映射（map key=LocalName()），使 createElement('矩形') 创建
        # SVGRectElement 且 localName 保留中文。无 enName 的中文条目跳过
        # （回退通用元素）。
        en_entries = {}
        for tag in tags:
            if tag['name'].original.isascii():
                en_entries[tag['name'].original] = tag
        kept = []
        for tag in tags:
            if tag['name'].original.isascii():
                kept.append(tag)
            elif tag.get('enName') and tag['enName'] in en_entries:
                tag['zh_alias'] = True
                kept.append(tag)
        tags = kept
        self._template_context['tags'] = tags
        for tag in tags:
            if tag.get('zh_alias'):
                continue  # interface 等信息从英文条目继承（第二遍）
            tag['has_js_interface'] = self._has_js_interface(tag)
            tag['js_interface'] = self._js_interface(tag)
            tag['interface'] = self._interface(tag)
            tag['interface_header'] = '%s/%s.h' % (
                self._interface_header_dir(tag),
                self.get_file_basename(tag['interface']),
            )
            tag['constructor_name'] = tag['name'].to_upper_camel_case()
            interface_counts[tag['interface']] += 1

        # 中文别名继承英文条目的构造信息（英文条目的 interface 等已算好）。
        for tag in tags:
            if not tag.get('zh_alias'):
                continue
            base = en_entries[tag['enName']]
            tag['interface'] = base['interface']
            tag['interface_header'] = base['interface_header']
            tag['has_js_interface'] = base['has_js_interface']
            tag['js_interface'] = base['js_interface']
            tag['noConstructor'] = base.get('noConstructor', False)
            tag['constructor_name'] = base['constructor_name']

        for tag in tags:
            tag['multipleTagNames'] = (
                interface_counts[tag['interface']] > 1
                or tag['interface'] == fallback_interface
            )

        self._template_context.update(
            {
                'fallback_interface': fallback_interface,
                'fallback_interface_header': self.get_file_basename(
                    fallback_interface
                )
                + '.h',
                'fallback_js_interface': fallback_js_interface,
                'input_files': self._input_files,
                'generate_tag_enum': generate_tag_enum,
            }
        )

    @template_expander.use_jinja(
        'templates/element_factory.h.tmpl', filters=filters
    )
    def generate_factory_header(self):
        return self._template_context

    @template_expander.use_jinja(
        'templates/element_factory.cc.tmpl', filters=filters
    )
    def generate_factory_implementation(self):
        return self._template_context

    def _interface(self, tag):
        if tag['interfaceName']:
            return tag['interfaceName']
        name = tag['name'].to_upper_camel_case()
        # FIXME: We shouldn't hard-code HTML here.
        if name == 'HTML':
            name = 'Html'
        return '%s%sElement' % (self.namespace, name)

    def _js_interface(self, tag):
        if tag['JSInterfaceName']:
            return tag['JSInterfaceName']
        return self._interface(tag)

    def _has_js_interface(self, tag):
        return not tag['noConstructor'] and self._js_interface(tag) != (
            '%sElement' % self.namespace
        )

    def _interface_header_dir(self, tag):
        if tag['interfaceHeaderDir']:
            return tag['interfaceHeaderDir']
        return 'third_party/blink/renderer/core/' + self.namespace.lower()


if __name__ == "__main__":
    json5_generator.Maker(MakeElementFactoryWriter).main()

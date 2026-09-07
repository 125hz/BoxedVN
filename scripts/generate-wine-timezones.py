#!/usr/bin/env python3
"""Generate the prefix timezone seed from Wine's kernelbase.rgs (Wine 11.0).

Only timezone keys are exported. No executable or title-specific data is used.
The generated data retains Wine's LGPL-2.1-or-later license.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def generate(source):
    stack, pending, sections = [], None, {}
    for line in source.splitlines():
        line = line.strip()
        if not line or line.startswith('//'):
            continue
        if line == '{':
            assert pending is not None
            stack.append(pending)
            pending = None
        elif line == '}':
            stack.pop()
        elif line.startswith('val '):
            if 'Time Zones' not in stack:
                continue
            match = re.fullmatch(r"val '([^']+)' = ([sbd]) (.*)", line)
            if not match:
                raise ValueError(line)
            name, kind, value = match.groups()
            if kind == 's':
                assert value.startswith("'") and value.endswith("'")
                value = json.dumps(value[1:-1], ensure_ascii=False)
            elif kind == 'd':
                value = 'dword:%08x' % int(value, 0)
            else:
                value = 'hex:' + ','.join('%02x' % b for b in bytes.fromhex(value))
            section = '\\\\'.join(stack[1:])
            sections.setdefault(section, []).append(json.dumps(name) + '=' + value)
        else:
            pending = re.sub(r'^NoRemove ', '', line).strip("'")
    assert not stack
    assert len(sections) > 150
    data = ''.join('R"BW_TZ([' + k + '] 0\n' + '\n'.join(v) + '\n)BW_TZ",\n' for k, v in sections.items())
    return ('// Generated from Wine 11.0 dlls/kernelbase/kernelbase.rgs.\n'
            '// Data: LGPL-2.1-or-later, Wine project. Do not edit by hand.\n'
            '// Source SHA256: ' + hashlib.sha256(source.encode()).hexdigest() + '\n'
            '#pragma once\nnamespace boxedvn {\n'
            'inline constexpr const char* wineTimezoneRegistrySeed[] = {\n' + data +
            '};\n}\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(generate(args.source.read_text(encoding='utf-8')), encoding='utf-8', newline='\n')

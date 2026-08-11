#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fills msgstr entries in a .po file from a JSON translation table.

Handles multi-line msgids, plural forms and fuzzy entries. The obvious
one-line regex approach silently skips exactly those cases, which leaves
strings untranslated with nothing to show for it. This exists so that does
not happen again.

    tools/fill-po.py po/de/pdf-smithy.po tools/de.json
"""
import json
import re
import sys


def joined_id(parts):
    return ''.join(re.findall(r'"(.*)"', p, re.S)[0] for p in parts if re.match(r'^\s*".*"\s*$', p))


def strip_fuzzy(line):
    if line.startswith('#,') and 'fuzzy' in line:
        cleaned = line.replace(', fuzzy', '').replace('#, fuzzy', '#,').rstrip()
        return None if cleaned in ('#,', '#') else cleaned
    return line


def rewrite(block, replacements):
    """Replaces the msgstr lines, dropping any continuation lines and fuzzy flag."""
    out, skipping = [], False
    for line in block.split('\n'):
        key = next((k for k in replacements if line.startswith(k)), None)
        if key:
            out.append('%s "%s"' % (key, replacements[key]))
            skipping = True
            continue
        if skipping and line.startswith('"'):
            continue
        skipping = False
        line = strip_fuzzy(line)
        if line is not None:
            out.append(line)
    return '\n'.join(out)


def fill(po_path, singles, plurals):
    blocks = open(po_path, encoding='utf-8').read().split('\n\n')
    filled = 0

    for index, block in enumerate(blocks):
        ident, plural, context, mode = [], [], [], None
        for line in block.split('\n'):
            if line.startswith('msgctxt'):
                mode = 'ctxt'
                context.append(line[len('msgctxt '):])
            elif line.startswith('msgid_plural'):
                mode = 'plural'
                plural.append(line[len('msgid_plural '):])
            elif line.startswith('msgid'):
                mode = 'id'
                ident.append(line[len('msgid '):])
            elif line.startswith('msgstr'):
                mode = None
            elif line.startswith('"') and mode:
                {'id': ident, 'plural': plural, 'ctxt': context}[mode].append(line)

        key = joined_id(ident)
        if not key:
            continue

        # The same word can need different translations in different places:
        # "Highlight" is a verb on a button and a noun in a list. A context-
        # qualified entry wins over the plain one, which is how both can exist
        # without one silently overwriting the other.
        qualified = joined_id(context) + '|' + key if context else None
        if qualified and qualified in singles and not plural:
            blocks[index] = rewrite(block, {'msgstr': singles[qualified]})
            filled += 1
            continue
        if qualified and qualified in plurals and plural:
            one, many = plurals[qualified]
            blocks[index] = rewrite(block, {'msgstr[0]': one, 'msgstr[1]': many})
            filled += 1
            continue

        if plural and key in plurals:
            one, many = plurals[key]
            blocks[index] = rewrite(block, {'msgstr[0]': one, 'msgstr[1]': many})
            filled += 1
        elif not plural and key in singles:
            blocks[index] = rewrite(block, {'msgstr': singles[key]})
            filled += 1

    open(po_path, 'w', encoding='utf-8').write('\n\n'.join(blocks))
    return filled


if __name__ == '__main__':
    table = json.load(open(sys.argv[2], encoding='utf-8'))
    count = fill(sys.argv[1], table.get('single', {}), {k: tuple(v) for k, v in table.get('plural', {}).items()})
    print('filled %d messages' % count)

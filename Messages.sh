#!/bin/sh
# Extracts translatable strings into po/pdf-smithy.pot.
# Run from the project root:  ./Messages.sh
#
# KXmlGui .rc files carry the menu and toolbar titles. KDE ships an
# "extractrc" helper for those, but it lives in a package many systems do not
# have, so the handful of lines it would do are inlined below instead. One
# less thing that has to be installed before the project can be translated.
set -e

podir=${podir:-po}
mkdir -p "$podir"

python3 - <<'PY' > rc.cpp
import glob, re, html
for path in glob.glob('src/**/*.rc', recursive=True):
    for line in open(path, encoding='utf-8'):
        m = re.search(r'<text(?:\s+context="([^"]*)")?\s*>(.*?)</text>', line)
        if not m:
            continue
        context, text = m.group(1), html.unescape(m.group(2))
        text = text.replace('\\', '\\\\').replace('"', '\\"')
        if context:
            print('i18nc("%s", "%s");' % (context, text))
        else:
            print('i18n("%s");' % text)
PY

xgettext \
    --from-code=UTF-8 \
    --c++ --kde \
    --keyword=i18n:1 --keyword=i18nc:1c,2 \
    --keyword=i18np:1,2 --keyword=i18ncp:1c,2,3 \
    --keyword=I18N_NOOP:1 --keyword=I18NC_NOOP:1c,2 \
    --keyword=kli18n:1 --keyword=kli18nc:1c,2 \
    --keyword=kli18np:1,2 --keyword=kli18ncp:1c,2,3 \
    --package-name="PDF Smithy" \
    --msgid-bugs-address="tombueng@gmail.com" \
    --copyright-holder="Tom Bueng" \
    -o "$podir/pdf-smithy.pot" \
    $(find src -name '*.cpp' -o -name '*.h') rc.cpp

rm -f rc.cpp
echo "Wrote $podir/pdf-smithy.pot"

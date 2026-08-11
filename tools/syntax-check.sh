#!/usr/bin/env bash
# Compiles one source file for its errors only, without touching the build.
#
# Several people (or several agents) editing different files at once would
# otherwise serialise on a single build directory, and a failed parallel build
# tells you far less than a clean syntax error. The flags are taken from the
# real build so that what passes here is what the build sees.
#
#   tools/syntax-check.sh src/cli/CommandsColour.cpp src/ui/ToolSupport.cpp
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
db="$root/build/compile_commands.json"
[ -f "$db" ] || { echo "no build/compile_commands.json; configure the build first" >&2; exit 1; }
[ $# -ge 1 ] || { echo "usage: $0 <source file…>" >&2; exit 2; }

# Per file, not once for all of them: a window needs Qt Widgets on the include
# path and the command line does not, so one shared set of flags would report
# a missing QDialog as if it were the file's own mistake.
extract="$root/tools/.syntax-flags.py"
cat > "$extract" <<'SCRIPT'
import json, os, shlex, sys

db = json.load(open(sys.argv[1]))
wanted = os.path.abspath(sys.argv[2])

entry = next((e for e in db if os.path.abspath(e["file"]) == wanted), None)
if entry is None:
    # A file the build has not been told about yet (a new one) borrows from a
    # neighbour in the same directory, which is nearly always the right set.
    folder = os.path.dirname(wanted)
    entry = next((e for e in db if os.path.dirname(os.path.abspath(e["file"])) == folder), None)
if entry is None:
    entry = next(e for e in db if e["file"].endswith("cli/main.cpp"))

parts = shlex.split(entry["command"])
keep, skip = [], False
for part in parts[1:]:
    if skip:
        skip = False
        continue
    if part in ("-c", "-o"):
        skip = part == "-o"
        continue
    if part.endswith(".cpp") or part.endswith(".o"):
        continue
    keep.append(part)
print(shlex.join(keep))
SCRIPT

status=0
for file in "$@"; do
    flags=$(python3 "$extract" "$db" "$file")
    # shellcheck disable=SC2086
    if c++ -fsyntax-only $flags "$file"; then
        echo "ok: $file"
    else
        status=1
    fi
done
rm -f "$extract"
exit $status

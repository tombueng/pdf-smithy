#!/usr/bin/env python3
"""Checks that every action exists in a menu and every menu entry exists as an action.

KXmlGui fails silently in both directions, which is why this is worth a test:

  * An `<Action name="…"/>` in the .rc file that no code registers is dropped
    without a word. The menu simply has one fewer item than it says.
  * An action registered in code that no .rc file mentions never appears
    anywhere. It is reachable by shortcut, if it has one, and by nothing else.

Neither shows up in the build, in a warning, or in the end-to-end test: the
window comes up looking fine. The only way to notice is to go looking for the
menu item, which is what a user does and what a developer does not.

Toolbar-only and dynamically inserted actions are the honest exception, so a
short allow-list lives below rather than in a comment somewhere.

    tools/check-actions.py
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Actions that legitimately live outside the menu bar. Keep this short, and
# keep the reason next to each one.
ALLOWED_ABSENT = {
    "file_open_recent",  # KStandardAction places itself
    "hamburger_menu",  # likewise; it is in the toolbar and registers itself
    "options_show_menubar",
    "options_show_toolbar",
    "options_configure_keybinding",
    "options_configure_toolbars",
    "help_contents",
    "help_whats_this",
    "help_report_bug",
    "help_about_app",
    "help_about_kde",
    "help_switch_language",
}


# Anything that ends up calling actionCollection()->addAction with the name it
# was given. Local helpers that wrap it are normal (one sets up a whole group
# of checkable actions, another might set an icon convention), but this script
# cannot see through a call it does not know about, so each has to be named. A
# helper missing from here fails loudly, with the action listed as unreachable,
# which is the right way round: a false alarm costs a minute, a missed menu
# entry ships.
REGISTRARS = ("addAction", "addFit", "addPen", "addField", "addMode", "addTool", "addGroup")


def registered_actions():
    """Names passed to actionCollection()->addAction, however it is spelt."""
    names = set()
    pattern = re.compile(r'\b(?:%s)\(\s*QStringLiteral\("([a-z0-9_]+)"\)' % "|".join(REGISTRARS))
    for path in (ROOT / "src" / "ui").rglob("*.cpp"):
        text = path.read_text(encoding="utf-8")
        # addAction(QStringLiteral("name"), …) and actions->addAction(…) alike.
        for match in pattern.finditer(text):
            names.add(match.group(1))
    return names


def menu_actions():
    """Names referenced by any .rc file."""
    names = set()
    for path in (ROOT / "src").rglob("*.rc"):
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r'<Action\s+name="([a-z0-9_]+)"', text):
            names.add(match.group(1))
    return names


def main():
    registered = registered_actions()
    inmenu = menu_actions()

    if not registered:
        print("no actions found at all; has the source layout changed?", file=sys.stderr)
        return 1

    # KStandardAction registers its own names, which this cannot see in the
    # source. Anything in a menu that looks standard is taken on trust.
    standard = {n for n in inmenu if n.startswith(("file_", "edit_", "help_", "options_", "settings_"))}

    orphan_menu = sorted(inmenu - registered - standard - ALLOWED_ABSENT)
    hidden = sorted(registered - inmenu - ALLOWED_ABSENT)

    problems = []
    if orphan_menu:
        problems.append("These are in a menu but nothing registers them, so the entry is dropped silently:\n  "
                        + "\n  ".join(orphan_menu))
    if hidden:
        problems.append("These are registered but appear in no menu, so nothing can reach them:\n  "
                        + "\n  ".join(hidden))

    if problems:
        print("\n\n".join(problems), file=sys.stderr)
        return 1

    print(f"All {len(registered)} registered actions appear in a menu, and every menu entry exists.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

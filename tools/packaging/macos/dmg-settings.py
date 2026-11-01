# SPDX-License-Identifier: AGPL-3.0-or-later
#
# dmgbuild settings for Sedef-Amp-macOS.dmg; build-dmg.sh passes the paths as -D defines.
# dmgbuild writes the Finder window layout (.DS_Store) itself, so no Finder or AppleScript runs.
#
# The icon positions are the card centres in tools/packaging/assets/dmg-background.html.

# `defines` is provided by dmgbuild when it runs this file.
pkg = defines["pkg"]  # noqa: F821
uninstall = defines["uninstall"]  # noqa: F821
readme = defines["readme"]  # noqa: F821

format = "ULFO"  # LZFSE compression, macOS 10.11 and later
filesystem = "HFS+"

files = [
    (pkg, "Install Sedef Amp.pkg"),
    (readme, "Read Me.txt"),
    (uninstall, "Uninstall Sedef Amp.command"),
]
hide_extensions = ["Install Sedef Amp.pkg", "Read Me.txt", "Uninstall Sedef Amp.command"]

icon = defines.get("icon")  # noqa: F821
background = defines["background"]  # noqa: F821  (dmgbuild picks up the @2x file next to it)

# The bounds include the title bar: 440 points of content plus about 28 for the bar.
window_rect = ((200, 140), (720, 468))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = False
include_icon_view_settings = True
include_list_view_settings = False

arrange_by = None
icon_size = 128
text_size = 13
label_pos = "bottom"
icon_locations = {
    "Install Sedef Amp.pkg": (190, 244),
    "Read Me.txt": (435, 244),
    "Uninstall Sedef Amp.command": (605, 244),
}

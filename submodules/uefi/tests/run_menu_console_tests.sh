#!/bin/sh
# Host tests without an Android/UEFI cross toolchain. Optional PPM paths:
# phone menu, tablet menu, phone launch message, tablet entering-menu message,
# phone EFI selector, tablet Reboot Tools menu, phone/tablet EFI launch errors.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d)
trap 'rm -f "$test_tmp/menu_console_test" "$test_tmp/menu_draw.inc" "$test_tmp/tool_messages.inc" "$test_tmp/browser_draw.inc" "$test_tmp/advanced_draw.inc" "$test_tmp/msd_draw.inc" "$test_tmp/fastboot_draw.inc"; rmdir "$test_tmp"' EXIT HUP INT TERM
# Compile the real boot-menu drawing/window functions with minimal test entry
# types, without linking unrelated boot/partition operations into host tests.
awk '
  /^#define SFB_ATTR_/ { print }
  /^#define SFB_ENTER_MENU_DELAY_S/ { print }
  /^(SfbShowFastbootMode|SfbShowBootingScreen|SfbShowActionScreen|SfbShowEnteringMenu) \(/ { print "STATIC VOID"; emit = 1 }
  /^SfbReportStatus \(/ { print "STATIC VOID"; emit = 1 }
  /^(SfbBeginScreen|SfbEndScreen|SfbDrawRow|SfbDrawMenu) \(/ { print "STATIC VOID"; emit = 1 }
  /^SfbWindowStart \(/ { print "STATIC UINTN"; emit = 1 }
  emit { print }
  /^}/ { emit = 0 }
' "$test_root/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbMenu.c" > "$test_tmp/menu_draw.inc"
awk '
  /^#define AT_ATTR_/ || /^#define AT_ENTER_MENU_DELAY_S/ { print }
  /^(AtUiEnterMenu|AtUiShowMessage|AtUiReportStatus|AtUiBeginScreen|AtUiEndScreen|AtUiDrawRow|AtUiDrawMenu|AtUiDrawConfirmation) \(/ { print "STATIC VOID"; emit = 1 }
  /^AtUiWindowStart \(/ { print "STATIC UINTN"; emit = 1 }
  emit { print }
  /^}/ { emit = 0 }
' "$test_root/edk2/AndroidToolsPkg/Library/AndroidToolsUi/AndroidToolsUi.c" > "$test_tmp/tool_messages.inc"
awk '
  /^(SfbDrawActions|SfbDrawDirectory|SfbDrawVolumes) \(/ { print "STATIC VOID"; emit = 1 }
  /^SfbIsEfiFile \(/ { print "STATIC BOOLEAN"; emit = 1 }
  emit { print }
  /^}/ { emit = 0 }
' "$test_root/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbBrowser.c" > "$test_tmp/browser_draw.inc"
awk '
  /^#define SFB_ADV_ROW_CHARS/ { print }
  /^SfbAdvDrawChooser \(/ { print "STATIC VOID"; emit = 1 }
  emit { print }
  /^}/ { emit = 0 }
' "$test_root/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbAdvanced.c" > "$test_tmp/advanced_draw.inc"
awk '
  /^SfbMsdDrawSession \(/ { print "STATIC VOID"; emit = 1 }
  emit { print }
  /^}/ { emit = 0 }
' "$test_root/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbUsbMsd.c" > "$test_tmp/msd_draw.inc"
awk '
  /^#define FB_(ACTION_ROWS|ATTR_)/ || /^STATIC UINTN mFbActionCursor/ { print }
  /^STATIC CONST CHAR16 \*mFbActionRow/ || /^typedef enum/ { emit = 1 }
  /^(FastbootDrawModeScreen|FastbootShowActionScreen) \(/ { print "STATIC VOID"; emit = 1 }
  emit { print }
  /^}/ { emit = 0 }
' "$test_root/edk2/QcomModulePkg/Library/FastbootLib/FastbootMain.c" > "$test_tmp/fastboot_draw.inc"
case $(uname -m) in
  arm64|aarch64) test_arch=AArch64 ;;
  x86_64) test_arch=X64 ;;
  *) echo 'Unsupported host architecture' >&2; exit 1 ;;
esac
"${CC:-clang}" -fshort-wchar -DNO_MSABI_VA_FUNCS -Wall -Wextra -Werror \
  -Wno-unused-parameter -fsanitize=address,undefined -O1 \
  -I "$test_root/edk2/MdePkg/Include" \
  -I "$test_root/edk2/MdePkg/Include/$test_arch" \
  -I "$test_root/edk2/AndroidToolsPkg/Include" \
  -I "$test_tmp" \
  "$test_root/tests/menu_console_test.c" -o "$test_tmp/menu_console_test"
"$test_tmp/menu_console_test" "$@"

# Rebootless KernelSU installation tests

Run `python3 targets/magisk_module/tests/test_live_install.py`. The tests source
the real activation helper, use temporary module trees and simulate process
lifetimes, installer completion, failed copies and competing updates. The
customizer is also run for both languages and each activation result. They do
not access an Android device; SELinux relabeling is mocked on the host.
Discovery tests use NUL-delimited cmdlines and stat fixtures with the actual
parser, including wrappers, missing ZIPFILE, PID reuse and zombie processes.
Two integration tests launch the actual detached prepare/finish processes with
only module/proc paths redirected and `chcon` stubbed; they verify delayed
native marker writes and unknown-installer partial activation.

The non-first-install path can activate this **file-only** module immediately:

- Eligibility uses normal boot mode, the fixed
  `/data/adb/modules_update/fake_bl_efisp` layout and file-only payload checks,
  not `KSU=true` or a manager/executable name whitelist. Recovery, unsupported
  paths, mounted module roots/subtrees, boot hooks, system overlays and
  unexpected symlinks keep the standard staged path. Preflight failures print
  bilingual reason codes to the installation console, before a log is safe.
- The updater holds the existing `tmp/flash.lock`. It never kills a flash task
  or replaces its files; a busy lock aborts installation and asks to retry later.
- All bundled files are copied into a temporary snapshot inside the module,
  labeled and hashed before replacing the explicit payload set. Swap failures
  restore the old payload; a failed restore retains its backup and lock.
  Runtime logs, imported ABLs and backups under `tmp/` are not replaced.
- WebUI and its matching backend are published synchronously, before the
  installer returns. The background worker is only for final state cleanup,
  not for making WebUI appear. Tests check this before the worker finishes,
  including when there was no previously installed module.
- A detached helper waits for the **installer shell and outermost matching
  installer ancestor** using their PID/start-time identities, not a fixed
  delay. Discovery walks at most 16 ancestors and matches separate arguments
  `module install <this ZIPFILE>`; names such as `ksud`, `libksud.so` and fork
  executable names are equivalent. A shell `-c` string is not an invocation.
  Missing ZIPFILE, incomplete/cyclic ancestry or an unrecognised install
  protocol prevents marker cleanup, not synchronous file activation. KSU may write
  `update` after the shell exits. The worker checks its token and both payload
  hashes before deleting only this module's `update` flag. Timeouts or mismatches
  leave the flag intact; another module's flags are never touched.
- Unknown completion returns a distinct partial-success result (4). The worker
  waits for the installer shell, then releases its lock while retaining the
  marker, staging and backup. Files are active, but a manager that gates WebUI
  on pending-update status may still require rebooting. Even result 0 only
  means files are active and the cleanup worker is ready, not cleanup success.
- The staged copy is retained for KSU's regular next boot. No module-image,
  kernel, mount, global KSU state, ABL, efisp or persist operation is involved.
  Vol+ first setup still requires the original Recovery/format procedure.

Android acceptance is still required: select NO, wait for the installer to
finish, then return to the module list **without manually refreshing**. Its new
WebUI and binaries should be usable before rebooting. Current official KSU
reloads on returning to this page and does not gate WebUI on `update`; the
pending-update label can briefly lag behind background cleanup. If a different
manager caches the entry/status, refreshing is a troubleshooting step, not an
activation requirement. The manager's generic reboot button and list cache are
not controlled by this module.
Check the installation console for preflight rejection reasons and
`/data/adb/modules/fake_bl_efisp/tmp/live-install.log` for activation/cleanup
failures. Also test
a module upgrade, the first helper install after formatting, active flash-task
rejection, SELinux labels, disabled modules, and the next normal reboot. Do not
perform a destructive flash merely to test immediate activation.

Reference review: [ksu_toolkit's installer](https://github.com/backslashxx/ksu_toolkit/blob/master/module/customize.sh)
either requests Mountify hot installation or replaces the active directory in
the background after a fixed delay. Neither branch calls a manager refresh API.
[Mountify's handler](https://github.com/backslashxx/mountify/blob/master/module/metainstall.sh)
also replaces the entire active directory. We do not request that handler or
copy its whole-directory replacement: this module keeps flash locks, logs and
ABL backups in its active `tmp/`. See KSU's
[page reload](https://github.com/tiann/KernelSU/blob/main/manager/app/src/main/java/me/weishu/kernelsu/ui/screen/module/ModuleScreen.kt)
and [WebUI eligibility](https://github.com/tiann/KernelSU/blob/main/manager/app/src/main/java/me/weishu/kernelsu/ui/screen/module/ModuleMaterial.kt).

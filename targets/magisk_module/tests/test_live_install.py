#!/usr/bin/env python3
"""Host-only tests. All module trees are temporary; no Android device is used."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "module/bin/live_install.sh"


class LiveInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fakebl-live-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.active = self.root / "modules/fake_bl_efisp"
        self.staged = self.root / "modules_update/fake_bl_efisp"
        for base, version in ((self.active, "old"), (self.staged, "new")):
            for directory in ("bin", "webroot", "efisp", "ablrepo"):
                (base / directory).mkdir(parents=True)
                (base / directory / "fixture").write_text(version)
            for name in ("BDS.efi", "lang.txt", "uninstall.sh"):
                (base / name).write_text(version)
            (base / "module.prop").write_text("id=fake_bl_efisp\nversion=" + version + "\n")
            for name in ("bl_flasher.sh", "extractfv", "patch_abl"):
                (base / "bin" / name).write_text(version)
            shutil.copyfile(SCRIPT, base / "bin/live_install.sh")
            (base / "webroot/index.html").write_text(version)
        (self.active / "tmp/expert").mkdir(parents=True)
        (self.active / "tmp/expert/backup.img").write_text("keep backup")
        (self.active / "tmp/flash.log").write_text("keep log")
        (self.active / "update").touch()
        other = self.root / "modules/other"
        other.mkdir()
        (other / "update").touch()
        (self.root / "proc").mkdir()
        (self.root / "proc/mounts").write_text("/dev/data /data ext4 rw 0 0\n")

    def shell(self, body, real_processes=False):
        prelude = '''
live_lib=$1
set -- --library
. "$live_lib"
LIVE_ACTIVE="$CASE_DIR/modules/fake_bl_efisp"
LIVE_STAGED="$CASE_DIR/modules_update/fake_bl_efisp"
LIVE_TXN="$LIVE_ACTIVE/tmp/live-install.test"
LIVE_TOKEN=test-token
LIVE_SHELL_PID=123 LIVE_SHELL_STAMP=shell-start
LIVE_PARENT_PID=456 LIVE_PARENT_STAMP=ksud-start
LIVE_PROC="$CASE_DIR/proc"
LIVE_INSTALL_ZIP=/data/local/tmp/module.zip
# SELinux is exercised on Android, not the host's filesystem.
chcon() { return 0; }
'''
        if not real_processes:
            prelude += 'live_process_stamp() { return 0; }\n'
        result = subprocess.run(
            ["sh", "-c", prelude + body, "test", str(SCRIPT)],
            env={**os.environ, "CASE_DIR": str(self.root)},
            capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def process(self, pid, parent, args, stamp="100", state="S"):
        proc = self.root / "proc" / str(pid)
        proc.mkdir(exist_ok=True)
        # Deliberately exercise comm with spaces and parentheses.
        fields = [state, str(parent)] + ["0"] * 17 + [stamp]
        (proc / "stat").write_text(f"{pid} (install (wrapper)) " + " ".join(fields))
        (proc / "cmdline").write_bytes(b"\0".join(a.encode() for a in args) + b"\0")

    def installer_chain(self, executable="/app/lib/arm64/libksud.so"):
        self.process(123, 456, ["sh", "-c", "install_module"], "shell-start")
        self.process(456, 789, [executable, "module", "install",
                               "/data/local/tmp/module.zip"], "ksud-start")
        self.process(789, 1, ["sh"], "wrapper-start")

    def snapshot(self):
        return '''
mkdir "$LIVE_TXN" "$LIVE_ACTIVE/tmp/flash.lock" || exit 1
echo "$LIVE_TOKEN" > "$LIVE_ACTIVE/tmp/flash.lock/live-install"
echo "$LIVE_TOKEN" > "$LIVE_STAGED/.live-install-token"
live_validate_payload || exit 2
live_snapshot || exit 3
'''

    def assert_payload(self, version):
        for name in ("bin", "webroot", "efisp", "ablrepo"):
            self.assertEqual((self.active / name / "fixture").read_text(), version)
        self.assertEqual((self.active / "BDS.efi").read_text(), version)
        self.assertEqual((self.active / "tmp/expert/backup.img").read_text(), "keep backup")
        self.assertEqual((self.active / "tmp/flash.log").read_text(), "keep log")

    def test_publish_and_finish_preserve_runtime_and_other_modules(self):
        self.shell(self.snapshot() + '''
live_publish || exit 4
live_finish || exit 5
live_release_lock
''')
        self.assert_payload("new")
        self.assertFalse((self.active / "update").exists())
        self.assertFalse((self.active / "tmp/flash.lock").exists())
        self.assertTrue((self.root / "modules/other/update").exists())
        self.assertTrue((self.staged / "bin/bl_flasher.sh").exists())

    def test_first_module_install_after_format(self):
        shutil.rmtree(self.active)
        (self.active / "tmp").mkdir(parents=True)
        (self.active / "update").touch()
        self.shell(self.snapshot() + 'live_publish && live_finish || exit 4\n')
        self.assertEqual((self.active / "webroot/index.html").read_text(), "new")
        self.assertFalse((self.active / "update").exists())

    def check_ready_before_installer_finishes(self):
        self.installer_chain()
        backend = self.staged / "bin/bl_flasher.sh"
        backend.write_text("#!/bin/sh\nprintf '%s\\n' 'new backend'\n")
        backend.chmod(0o755)
        self.shell('''
live_supported() { return 0; }
live_process_stamp() { echo installer-still-running; }
# Model a ready worker that has not performed its post-installer cleanup yet.
nohup() { : > "$LIVE_TXN/ready"; command sleep 1; }
sleep() { command sleep 0.02; }
live_prepare "$LIVE_STAGED" 123 456 || exit 1
wait "$LIVE_WORKER_PID" || exit 2
[ "$(live_process_stamp 123)" = installer-still-running ] || exit 3
[ "$(live_process_stamp 456)" = installer-still-running ] || exit 4
# A manager list reload sees the new WebUI immediately; a pending update
# marker does not stop its matching backend from being installed/readable.
[ "$(cat "$LIVE_ACTIVE/webroot/index.html")" = new ] || exit 5
[ "$("$LIVE_ACTIVE/bin/bl_flasher.sh")" = 'new backend' ] || exit 6
grep -q '^version=new$' "$LIVE_ACTIVE/module.prop" || exit 7
touch "$LIVE_ACTIVE/update"
[ -f "$LIVE_STAGED/module.prop" ] || exit 8
# Finish only after checking availability, mirroring the actual native exit.
live_process_stamp() { return 0; }
live_finish || exit 9
live_release_lock
rm -rf "$LIVE_TXN"
''')
        self.assertFalse((self.active / "update").exists())
        self.assertFalse((self.active / "tmp/flash.lock").exists())

    def test_upgrade_webui_and_backend_ready_before_installer_finishes(self):
        self.check_ready_before_installer_finishes()
        self.assert_payload("new")

    def test_new_module_webui_and_backend_ready_before_installer_finishes(self):
        shutil.rmtree(self.active)
        self.check_ready_before_installer_finishes()

    def test_waits_for_native_parent_after_shell_and_final_marker(self):
        self.shell(self.snapshot() + '''
live_publish || exit 4
echo 0 > "$CASE_DIR/ticks"
live_process_stamp() {
  ticks=$(cat "$CASE_DIR/ticks")
  if [ "$1" = 123 ] && [ "$ticks" -lt 1 ]; then echo shell-start; fi
  if [ "$1" = 456 ] && [ "$ticks" -lt 3 ]; then echo ksud-start; fi
}
sleep() {
  ticks=$(cat "$CASE_DIR/ticks")
  ticks=$((ticks + 1))
  echo "$ticks" > "$CASE_DIR/ticks"
  # Simulate native KSU marking update again AFTER customize.sh exits.
  [ "$ticks" -ne 2 ] || touch "$LIVE_ACTIVE/update"
}
live_finish || exit 5
[ "$(cat "$CASE_DIR/ticks")" = 3 ] || exit 6
[ ! -e "$LIVE_ACTIVE/update" ] || exit 7
''')

    def test_timeout_keeps_pending_update(self):
        self.shell(self.snapshot() + '''
live_publish || exit 4
live_process_stamp() { echo shell-start; }
sleep() { :; }
if live_finish; then exit 5; fi
[ -f "$LIVE_ACTIVE/update" ] || exit 6
''')

    def test_pid_reuse_does_not_wait_for_unrelated_process(self):
        self.shell(self.snapshot() + '''
live_publish || exit 4
live_process_stamp() { echo different-start-time; }
sleep() { exit 99; }
live_finish || exit 5
''')

    def test_new_install_token_keeps_pending_update(self):
        self.shell(self.snapshot() + '''
live_publish || exit 4
echo another-install > "$LIVE_STAGED/.live-install-token"
if live_finish; then exit 5; fi
[ -f "$LIVE_ACTIVE/update" ] || exit 6
''')

    def test_modified_payload_is_not_acknowledged(self):
        for destination in ("$LIVE_ACTIVE", "$LIVE_STAGED"):
            with self.subTest(destination=destination):
                self.shell(self.snapshot() + f'''
live_publish || exit 4
echo changed > "{destination}/BDS.efi"
if live_finish; then exit 5; fi
[ -f "$LIVE_ACTIVE/update" ] || exit 6
live_restore || exit 7
rm -rf "$LIVE_TXN" "$LIVE_ACTIVE/tmp/flash.lock"
''')

    def test_failed_swap_restores_old_payload(self):
        self.shell(self.snapshot() + '''
mv() {
  [ "$1" != "$LIVE_TXN/new/webroot" ] || return 1
  command mv "$@"
}
if live_publish; then exit 4; fi
live_restore || exit 5
''')
        self.assert_payload("old")

    def test_failed_snapshot_never_changes_active_payload(self):
        self.shell('''
mkdir "$LIVE_TXN"
chcon() { return 1; }
if live_snapshot; then exit 1; fi
live_restore || exit 2
''')
        self.assert_payload("old")

    def test_active_flash_blocks_prepare(self):
        self.installer_chain()
        self.shell('''
mkdir "$LIVE_ACTIVE/tmp/flash.lock"
echo real-task > "$LIVE_ACTIVE/tmp/flash.lock/owner"
live_supported() { return 0; }
live_process_stamp() { echo start-time; }
live_prepare "$LIVE_STAGED" 123 456
[ "$?" = 3 ] || exit 1
[ "$(cat "$LIVE_ACTIVE/tmp/flash.lock/owner")" = real-task ] || exit 2
''')
        self.assert_payload("old")

    def test_worker_launch_failure_rolls_back_and_releases_lock(self):
        self.installer_chain()
        self.shell('''
live_supported() { return 0; }
live_process_stamp() { echo start-time; }
nohup() { return 1; }
sleep() { :; }
live_prepare "$LIVE_STAGED" 123 456
[ "$?" = 1 ] || exit 1
# The production EXIT trap restores the payload after failed preparation.
''')
        self.assert_payload("old")
        self.assertFalse((self.active / "tmp/flash.lock").exists())
        self.assertTrue((self.active / "update").exists())

    def test_finish_preserves_disable_and_remove_flags(self):
        self.shell(self.snapshot() + '''
live_publish || exit 4
# A user can disable/remove the module after installation; never undo that.
touch "$LIVE_ACTIVE/disable" "$LIVE_ACTIVE/remove"
live_finish || exit 5
[ -f "$LIVE_ACTIVE/disable" ] && [ -f "$LIVE_ACTIVE/remove" ] || exit 6
''')

    def test_capabilities_not_manager_brand_and_wrong_path(self):
        self.shell('''
KSU=false BOOTMODE=true
live_supported "$LIVE_STAGED" || exit 1
unset KSU
live_supported "$LIVE_STAGED" || exit 4
KSU=true BOOTMODE=false
if live_supported "$LIVE_STAGED"; then exit 2; fi
BOOTMODE=true
if live_supported /data/adb/modules_update/other; then exit 3; fi
''')

    def test_mounted_module_root_or_subtree_is_rejected_with_reason(self):
        for mount in (self.active.parent, self.staged.parent, self.active,
                      self.staged, self.active / "webroot", self.staged / "bin"):
            with self.subTest(mount=mount):
                (self.root / "proc/mounts").write_text(f"/dev/loop0 {mount} ext4 rw 0 0\n")
                result = self.shell('''
BOOTMODE=true
if live_supported "$LIVE_STAGED"; then exit 1; fi
''')
                self.assertIn("mounted-tree", result.stderr)

    def test_real_cmdline_parser_accepts_names_and_follows_wrappers(self):
        for executable in ("/data/adb/ksu/ksud", "/app/lib/arm64/libksud.so",
                           "/arbitrary/fork-installer"):
            with self.subTest(executable=executable):
                self.installer_chain(executable)
                self.process(123, 222, ["sh"], "shell-start")
                self.process(222, 456, ["wrapper", "--install"], "wrapper-start")
                self.shell('''
live_find_installer 222 || exit 1
[ "$LIVE_PARENT_PID" = 456 ] && [ "$LIVE_PARENT_STAMP" = ksud-start ] || exit 2
[ "$(live_process_stamp 123)" = shell-start ] || exit 3
''', real_processes=True)

    def test_discovery_requires_this_zip_and_separate_arguments(self):
        invalid = (
            ["ksud"],
            ["ksud", "module", "install", "/other.zip"],
            ["sh", "-c", "ksud module install /data/local/tmp/module.zip"],
            ["daemon", "module", "uninstall", "/data/local/tmp/module.zip"],
        )
        for args in invalid:
            with self.subTest(args=args):
                self.installer_chain()
                self.process(456, 789, args)
                self.shell('''
if live_find_installer 456; then exit 1; fi
[ "$LIVE_PARENT_PID" = 0 ] && [ -z "$LIVE_PARENT_STAMP" ] || exit 2
''', real_processes=True)

    def test_discovery_uses_outermost_matching_ancestor(self):
        self.installer_chain()
        self.process(789, 1, ["outer", "module", "install", "/data/local/tmp/module.zip"], "outer")
        self.shell('''
live_find_installer 456 || exit 1
[ "$LIVE_PARENT_PID" = 789 ] && [ "$LIVE_PARENT_STAMP" = outer ] || exit 2
''', real_processes=True)

    def test_broken_or_cyclic_ancestry_disables_acknowledgement(self):
        for parent in (999, 456):
            self.installer_chain()
            self.process(789, parent, ["wrapper"])
            self.shell('''
if live_find_installer 456; then exit 1; fi
[ -z "$LIVE_PARENT_STAMP" ] || exit 2
''', real_processes=True)

    def test_zombie_or_reused_ancestor_is_not_acknowledged(self):
        self.installer_chain()
        self.process(456, 789, ["daemon", "module", "install", "/data/local/tmp/module.zip"], state="Z")
        self.shell('''
if live_find_installer 456; then exit 1; fi
''', real_processes=True)
        self.installer_chain()
        self.shell('''
echo 0 > "$CASE_DIR/calls"
live_process_stamp() {
  n=$(cat "$CASE_DIR/calls")
  echo $((n + 1)) > "$CASE_DIR/calls"
  echo "$n"
}
if live_find_installer 456; then exit 1; fi
[ -z "$LIVE_PARENT_STAMP" ] || exit 2
''')

    def test_missing_zip_keeps_completion_unknown(self):
        self.installer_chain()
        self.shell('''
unset LIVE_INSTALL_ZIP
if live_find_installer 456; then exit 1; fi
''', real_processes=True)

    def check_real_background_worker(self, known):
        self.installer_chain()
        if not known:
            self.process(456, 789, ["unknown-install-protocol"])
        self.process(123, 222, ["sh"], "shell-start")
        self.process(222, 456, ["wrapper"])
        # Run the actual prepare/finish subprocesses with only filesystem/proc
        # roots redirected to fixtures. No detection or worker functions mocked.
        script = SCRIPT.read_text()
        script = script.replace("LIVE_ACTIVE=/data/adb/modules/fake_bl_efisp",
                                f'LIVE_ACTIVE="{self.active}"')
        script = script.replace("LIVE_STAGED=/data/adb/modules_update/fake_bl_efisp",
                                f'LIVE_STAGED="{self.staged}"')
        script = script.replace("LIVE_PROC=/proc", f'LIVE_PROC="{self.root / "proc"}"')
        staged_helper = self.staged / "bin/live_install.sh"
        staged_helper.write_text(script)
        host_bin = self.root / "host-bin"
        host_bin.mkdir()
        chcon = host_bin / "chcon"
        chcon.write_text("#!/bin/sh\nexit 0\n")
        chcon.chmod(0o755)
        result = subprocess.run(
            ["sh", str(staged_helper), "prepare", str(self.staged), "123", "222"],
            env={**os.environ, "BOOTMODE": "true", "KSU": "false",
                 "LIVE_INSTALL_ZIP": "/data/local/tmp/module.zip",
                 "PATH": str(host_bin) + os.pathsep + os.environ["PATH"]},
            capture_output=True, text=True, timeout=10,
        )
        # Ensure a failed assertion does not leave a detached worker alive.
        def end_install():
            for pid in (123, 222, 456):
                (self.root / "proc" / str(pid) / "stat").unlink(missing_ok=True)
            deadline = time.monotonic() + 5
            while (self.active / "tmp/flash.lock").exists() and time.monotonic() < deadline:
                time.sleep(0.05)
        self.addCleanup(end_install)
        self.assertEqual(result.returncode, 0 if known else 4, result.stdout + result.stderr)
        self.assert_payload("new")
        self.assertTrue((self.active / "update").exists())
        (self.root / "proc/123/stat").unlink()
        (self.root / "proc/222/stat").unlink()
        # Native installer writes the marker after the shell/wrapper exits.
        (self.active / "update").write_text("native-final-marker")
        time.sleep(1.2)
        self.assertEqual((self.active / "update").read_text(), "native-final-marker")
        end_install()
        self.assertFalse((self.active / "tmp/flash.lock").exists())
        self.assertEqual((self.active / "update").exists(), not known)
        self.assertEqual(bool(list((self.active / "tmp").glob("live-install.??????"))), not known)
        self.assertIn("WebUI ready without reboot" if known else "completion-unknown",
                      (self.active / "tmp/live-install.log").read_text())

    def test_real_background_worker_waits_for_wrapped_installer(self):
        self.check_real_background_worker(known=True)

    def test_real_background_worker_retains_unknown_installer_marker(self):
        self.check_real_background_worker(known=False)

    def test_unknown_installer_still_publishes_but_keeps_marker_and_backup(self):
        self.installer_chain("unknown")
        self.process(456, 789, ["persistent-root-shell"])
        result = self.shell('''
BOOTMODE=true
nohup() { : > "$LIVE_TXN/ready"; command sleep 1; }
sleep() { command sleep 0.02; }
live_prepare "$LIVE_STAGED" 123 456
[ "$?" = 4 ] || exit 1
wait "$LIVE_WORKER_PID" || exit 2
# End only the installer shell. An unrecognised parent must not be awaited.
live_process_stamp() { return 0; }
if live_finish; then exit 3; fi
[ -f "$LIVE_ACTIVE/update" ] && [ -d "$LIVE_TXN/old" ] || exit 4
live_release_lock
''', real_processes=True)
        self.assertIn("completion-unknown", result.stderr)
        self.assert_payload("new")
        self.assertFalse((self.active / "tmp/flash.lock").exists())

    def test_boot_hooks_and_symlinks_refuse_hot_activation(self):
        self.shell('''
touch "$LIVE_STAGED/service.sh"
if live_validate_payload; then exit 1; fi
rm "$LIVE_STAGED/service.sh"
ln -s /dev/null "$LIVE_STAGED/bin/link"
if live_validate_payload; then exit 2; fi
''')

    def test_release_does_not_remove_a_different_owner_lock(self):
        self.shell('''
mkdir "$LIVE_ACTIVE/tmp/flash.lock"
echo another-owner > "$LIVE_ACTIVE/tmp/flash.lock/live-install"
live_release_lock
[ -f "$LIVE_ACTIVE/tmp/flash.lock/live-install" ] || exit 1
''')

    def test_customizer_nonfirst_bilingual_messages_and_activation_results(self):
        for language in ("zh", "en"):
            for activation_result in (0, 1, 2, 3, 4):
                with self.subTest(language=language, result=activation_result):
                    (self.root / "keys").write_text("0")
                    script = r'''
MODPATH="$CASE_DIR/modules_update/fake_bl_efisp"
# A compatible fork need not export a brand variable.
unset KSU
BOOTMODE=true
ZIPFILE=/data/local/tmp/module.zip
# Match the real installer: BOOTMODE is a shell variable, not exported.
ui_print() { printf '%s\n' "$*"; }
abort() { ui_print "$*"; exit 90; }
ksud() { :; }
sleep() { :; }
set_perm() { :; }
set_perm_recursive() { :; }
getprop() { [ "$1" != ro.boot.slot_suffix ] || echo _a; }
timeout() {
  n=$(cat "$CASE_DIR/keys")
  echo $((n + 1)) > "$CASE_DIR/keys"
  if [ "$n" = 0 ] && [ "$TEST_LANG" = zh ]; then
    echo KEY_VOLUMEUP
  else
    echo KEY_VOLUMEDOWN
  fi
}
sh() {
  [ "$1" = "$MODPATH/bin/live_install.sh" ] || exit 91
  [ "$2" = prepare ] && [ "$3" = "$MODPATH" ] || exit 92
  env | grep -q '^BOOTMODE=true$' || exit 93
  env | grep -q '^LIVE_INSTALL_ZIP=/data/local/tmp/module.zip$' || exit 94
  echo activation-called
  return "$TEST_RESULT"
}
. "$1"
'''
                    result = subprocess.run(
                        ["sh", "-c", script, "test", str(ROOT / "module/customize.sh")],
                        env={**os.environ, "CASE_DIR": str(self.root),
                             "TEST_LANG": language, "TEST_RESULT": str(activation_result)},
                        capture_output=True, text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 90 if activation_result in (1, 3) else 0,
                                     result.stdout + result.stderr)
                    self.assertIn("activation-called", result.stdout)
                    if activation_result == 0:
                        self.assertIn("安装完成。返回模块页即可打开WebUI。" if language == "zh" else
                                      "Installation complete. Return to the module page to open WebUI.", result.stdout)
                        self.assertIn("返回模块页" if language == "zh" else
                                      "Return to the module page", result.stdout)
                        self.assertNotIn("刷新模块列表", result.stdout)
                        self.assertNotIn("Refresh the module list", result.stdout)
                    if activation_result == 2:
                        self.assertIn("无法安全即时激活" if language == "zh" else
                                      "Safe immediate activation is unavailable", result.stdout)
                    if activation_result == 4:
                        self.assertIn("保留待重启标记" if language == "zh" else
                                      "keeping the pending-update marker", result.stdout)
                    prop = (self.staged / "module.prop").read_text()
                    self.assertIn("version=6.2.192-reF1nd\n", prop)
                    self.assertIn("versionCode=16\n", prop)
                    self.assertIn("name=假回锁\n" if language == "zh" else
                                  "name=Fake BL EFISP\n", prop)


if __name__ == "__main__":
    unittest.main()

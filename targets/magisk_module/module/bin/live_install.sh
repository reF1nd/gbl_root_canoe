#!/system/bin/sh
# Activate this file-only module without rebooting. Keep KSU's staged copy for
# its normal next-boot processing; never alter another module or a partition.

LIVE_ACTIVE=/data/adb/modules/fake_bl_efisp
LIVE_STAGED=/data/adb/modules_update/fake_bl_efisp
LIVE_ITEMS="bin webroot efisp ablrepo BDS.efi lang.txt uninstall.sh module.prop"
LIVE_PROC=/proc

live_notice() {
  # Preflight diagnostics must reach the install console even before tmp is safe.
  printf '[live-install:%s] %s / %s\n' "$1" "$2" "$3" >&2
}

live_reject() {
  live_notice "$@"
  return 1
}

live_process_stamp() {
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  [ "$1" -gt 1 ] || return 1
  # comm can contain spaces and parentheses. Fields after its last ')' start
  # with state (3); starttime (22) is therefore field 20.
  sed 's/^.*) //' "$LIVE_PROC/$1/stat" 2>/dev/null |
    awk '$1 != "Z" && NF >= 20 { print $20 }'
}

live_supported() {
  [ "${BOOTMODE:-}" = true ] || {
    live_reject boot-mode '需要正常开机环境' 'Normal boot required'; return 1;
  }
  [ "$1" = "$LIVE_STAGED" ] || {
    live_reject staging-path '安装暂存路径不支持' 'Unsupported staging path'; return 1;
  }
  [ ! -L "$LIVE_ACTIVE" ] && [ ! -L "$LIVE_STAGED" ] &&
    [ ! -L "${LIVE_ACTIVE%/*}" ] && [ ! -L "${LIVE_STAGED%/*}" ] || {
    live_reject symlink '模块路径包含符号链接' 'Symlink in module path'; return 1;
  }
  [ -r "$LIVE_PROC/mounts" ] || {
    live_reject mounts '无法核验挂载布局' 'Cannot inspect mount layout'; return 1;
  }
  # Old image-based KSU unmounts its staging directory when install ends.
  # Do not manipulate its module images or an unfamiliar installer layout.
  awk -v a="${LIVE_ACTIVE%/*}" -v s="${LIVE_STAGED%/*}" \
      -v am="$LIVE_ACTIVE" -v sm="$LIVE_STAGED" '
    $2 == a || $2 == s || $2 == am || $2 == sm ||
    index($2, am "/") == 1 || index($2, sm "/") == 1 { found = 1 }
    END { exit found }
  ' "$LIVE_PROC/mounts" || {
    live_reject mounted-tree '独立挂载的模块目录暂不支持' 'Mounted module trees are not supported'; return 1;
  }
  return 0
}

live_process_args() {
  tr '\000' '\n' < "$LIVE_PROC/$1/cmdline" 2>/dev/null
}

live_parent_pid() {
  sed 's/^.*) //' "$LIVE_PROC/$1/stat" 2>/dev/null | awk 'NF >= 2 { print $2 }'
}

live_install_invocation() {
  # Match the install operation and this ZIP, never the executable's brand/name.
  # A shell's single "-c" string is deliberately not treated as argv tokens.
  [ -n "${LIVE_INSTALL_ZIP:-}" ] || return 1
  live_process_args "$1" | awk -v zip="$LIVE_INSTALL_ZIP" '
    { arg[NR] = $0 }
    END {
      for (i = 2; i + 2 <= NR; i++)
        if (arg[i] == "module" && arg[i+1] == "install" && arg[i+2] == zip)
          exit 0
      exit 1
    }'
}

live_find_installer() {
  LIVE_PARENT_PID=0 LIVE_PARENT_STAMP=
  live_ancestor=$1 live_depth=0
  # Follow wrappers to the outermost matching installer. If the bounded walk
  # cannot be completed, do not acknowledge pending-update cleanup.
  while [ "$live_ancestor" != 1 ]; do
    case "$live_ancestor" in ''|*[!0-9]*|0) break ;; esac
    [ "$live_depth" -lt 16 ] || break
    live_stamp=$(live_process_stamp "$live_ancestor")
    [ -n "$live_stamp" ] || break
    live_next=$(live_parent_pid "$live_ancestor")
    if live_install_invocation "$live_ancestor"; then
      LIVE_PARENT_PID=$live_ancestor LIVE_PARENT_STAMP=$live_stamp
    fi
    [ "$(live_process_stamp "$live_ancestor")" = "$live_stamp" ] || break
    [ "$live_next" != "$live_ancestor" ] || break
    live_ancestor=$live_next
    live_depth=$((live_depth + 1))
  done
  if [ "$live_ancestor" != 1 ]; then
    LIVE_PARENT_PID=0 LIVE_PARENT_STAMP=
  fi
  [ -n "$LIVE_PARENT_STAMP" ]
}

live_validate_payload() {
  for live_item in bin webroot efisp ablrepo; do
    [ -d "$LIVE_STAGED/$live_item" ] || return 1
  done
  for live_item in BDS.efi lang.txt uninstall.sh module.prop bin/bl_flasher.sh \
                   bin/live_install.sh bin/extractfv bin/patch_abl webroot/index.html; do
    [ -f "$LIVE_STAGED/$live_item" ] || return 1
  done
  [ "$(sed -n 's/^id=//p' "$LIVE_STAGED/module.prop")" = fake_bl_efisp ] || return 1
  # Rebootless activation is only valid for this module's file-only design.
  for live_dir in "$LIVE_STAGED" "$LIVE_ACTIVE"; do
    [ ! -d "$live_dir" ] && continue
    for live_hook in system vendor product system_ext odm system.prop sepolicy.rule \
                     post-fs-data.sh post-mount.sh service.sh boot-completed.sh; do
      [ ! -e "$live_dir/$live_hook" ] && [ ! -L "$live_dir/$live_hook" ] || return 1
    done
  done
  for live_item in $LIVE_ITEMS; do
    [ ! -L "$LIVE_ACTIVE/$live_item" ] || return 1
    [ -z "$(find "$LIVE_STAGED/$live_item" ! -type f ! -type d -print)" ] || return 1
  done
  [ ! -L "$LIVE_ACTIVE/tmp" ] && [ ! -L "$LIVE_ACTIVE/tmp/live-install.log" ] || return 1
  [ ! -L "$LIVE_STAGED/.live-install-token" ] || return 1
  return 0
}

live_snapshot() {
  mkdir "$LIVE_TXN/new" "$LIVE_TXN/old" || return 1
  for live_item in $LIVE_ITEMS; do
    cp -a "$LIVE_STAGED/$live_item" "$LIVE_TXN/new/" || return 1
  done
  # Preserve the Android SELinux label, not merely Unix permissions.
  chcon -R u:object_r:system_file:s0 "$LIVE_TXN/new" || return 1
  (cd "$LIVE_TXN/new" && find . -type f) > "$LIVE_TXN/files" || return 1
  (
    cd "$LIVE_TXN/new" || exit 1
    while IFS= read -r live_file; do
      sha256sum "$live_file" || exit 1
    done < "$LIVE_TXN/files"
  ) > "$LIVE_TXN/manifest" || return 1
  [ -s "$LIVE_TXN/manifest" ] || return 1
}

live_restore() {
  # A failed copy/swap must leave the old payload usable. tmp (logs, ABL
  # backups, imports, PID and flash.lock) is never part of the replaced set.
  for live_item in $LIVE_ITEMS; do
    if [ -e "$LIVE_TXN/installed-$live_item" ]; then
      rm -rf "$LIVE_ACTIVE/$live_item" || return 1
    fi
    if [ -e "$LIVE_TXN/old/$live_item" ]; then
      mv "$LIVE_TXN/old/$live_item" "$LIVE_ACTIVE/$live_item" || return 1
    fi
  done
}

live_publish() {
  for live_item in $LIVE_ITEMS; do
    if [ -e "$LIVE_ACTIVE/$live_item" ]; then
      mv "$LIVE_ACTIVE/$live_item" "$LIVE_TXN/old/$live_item" || return 1
    fi
    # Journal BEFORE the rename, so failure or a handled signal can roll back.
    : > "$LIVE_TXN/installed-$live_item" || return 1
    mv "$LIVE_TXN/new/$live_item" "$LIVE_ACTIVE/$live_item" || return 1
  done
  (cd "$LIVE_ACTIVE" && sha256sum -c "$LIVE_TXN/manifest") || return 1
}

live_release_lock() {
  if [ "$(cat "$LIVE_ACTIVE/tmp/flash.lock/live-install" 2>/dev/null)" = "$LIVE_TOKEN" ]; then
    rm -f "$LIVE_ACTIVE/tmp/flash.lock/live-install"
    rmdir "$LIVE_ACTIVE/tmp/flash.lock"
  fi
}

live_prepare_cleanup() {
  if [ "$LIVE_TRANSFERRED" != yes ]; then
    if [ -n "$LIVE_WORKER_PID" ]; then
      kill "$LIVE_WORKER_PID" 2>/dev/null
      wait "$LIVE_WORKER_PID" 2>/dev/null
    fi
    if [ -n "$LIVE_TXN" ] && [ -d "$LIVE_TXN/old" ]; then
      if ! live_restore; then
        echo "Rollback failed; backup retained at $LIVE_TXN. Flashing stays locked until module files are recovered." >&2
        # Retain the lock if restoration failed: do not flash a mixed payload.
        return
      fi
    fi
    [ -z "$LIVE_TXN" ] || rm -rf "$LIVE_TXN"
    live_release_lock
  fi
}

live_finish() {
  # KSU's native parent can write update AGAIN after the installer shell exits.
  # Wait for both exact process instances; a fixed sleep races slow installs.
  live_wait=0
  while [ "$(live_process_stamp "$LIVE_SHELL_PID")" = "$LIVE_SHELL_STAMP" ] || \
        { [ -n "$LIVE_PARENT_STAMP" ] &&
          [ "$(live_process_stamp "$LIVE_PARENT_PID")" = "$LIVE_PARENT_STAMP" ]; }; do
    [ "$live_wait" -lt 60 ] || {
      live_reject timeout '安装结束未确认，保留待更新标记和备份' 'Installer exit unconfirmed; keeping update marker and backup'; return 1;
    }
    sleep 1
    live_wait=$((live_wait + 1))
  done
  [ -n "$LIVE_PARENT_STAMP" ] || {
    live_reject completion-unknown '文件已启用，无法确认安装器收尾；保留待更新标记' 'Files active, installer completion unknown; keeping update marker'; return 1;
  }
  [ "$(cat "$LIVE_ACTIVE/tmp/flash.lock/live-install" 2>/dev/null)" = "$LIVE_TOKEN" ] || return 1
  [ "$(cat "$LIVE_STAGED/.live-install-token" 2>/dev/null)" = "$LIVE_TOKEN" ] || return 1
  [ -f "$LIVE_ACTIVE/update" ] || return 1
  # A second install or a failed native copy must not be acknowledged as ours.
  (cd "$LIVE_STAGED" && sha256sum -c "$LIVE_TXN/manifest") || return 1
  (cd "$LIVE_ACTIVE" && sha256sum -c "$LIVE_TXN/manifest") || return 1
  [ "$(cat "$LIVE_STAGED/.live-install-token" 2>/dev/null)" = "$LIVE_TOKEN" ] || return 1
  rm -f "$LIVE_ACTIVE/update" || return 1
  echo 'WebUI ready without reboot. / WebUI 已就绪，无需重启。'
}

live_prepare() {
  live_supported "$1" || return 2
  LIVE_SHELL_PID=$2
  LIVE_SHELL_STAMP=$(live_process_stamp "$LIVE_SHELL_PID")
  [ -n "$LIVE_SHELL_STAMP" ] || {
    live_notice shell-identity '无法跟踪安装脚本进程' 'Cannot track installer shell'; return 2;
  }
  live_find_installer "$3" || live_notice completion-unknown \
    '将启用文件，但安装器结束状态无法可靠跟踪' 'Will activate files; installer completion cannot be reliably tracked'
  live_validate_payload || {
    live_notice payload '模块内容、启动钩子或路径检查未通过' 'Payload, boot-hook or path validation failed'; return 2;
  }
  mkdir -p "$LIVE_ACTIVE/tmp" || {
    live_notice storage '无法创建活动模块工作目录' 'Cannot create active module work directory'; return 1;
  }
  if ! mkdir "$LIVE_ACTIVE/tmp/flash.lock" 2>/dev/null; then
    [ ! -d "$LIVE_ACTIVE/tmp/flash.lock" ] || return 3
    live_notice storage '无法创建更新互斥锁' 'Cannot create update lock'
    return 1
  fi
  LIVE_TOKEN="live-$LIVE_SHELL_PID-$LIVE_SHELL_STAMP"
  LIVE_TXN=
  LIVE_WORKER_PID=
  LIVE_TRANSFERRED=no
  trap live_prepare_cleanup EXIT
  trap 'exit 1' HUP INT TERM
  echo "$LIVE_TOKEN" > "$LIVE_ACTIVE/tmp/flash.lock/live-install" || return 1
  : > "$LIVE_ACTIVE/tmp/live-install.log" || return 1
  LIVE_TXN=$(mktemp -d "$LIVE_ACTIVE/tmp/live-install.XXXXXX") || return 1
  live_snapshot >>"$LIVE_ACTIVE/tmp/live-install.log" 2>&1 || return 1
  # Publish WebUI AND its matching backend before returning to the installer.
  # KSU can discover them on its normal post-install list reload, even while
  # the detached worker is still clearing the pending-update marker.
  live_publish >>"$LIVE_ACTIVE/tmp/live-install.log" 2>&1 || return 1
  echo "$LIVE_TOKEN" > "$LIVE_STAGED/.live-install-token" || return 1
  # Do not keep KSU's installer stdout/stderr pipe open in the background.
  nohup sh "$LIVE_ACTIVE/bin/live_install.sh" finish "$LIVE_TXN" "$LIVE_TOKEN" \
    "$LIVE_SHELL_PID" "$LIVE_SHELL_STAMP" "$LIVE_PARENT_PID" "$LIVE_PARENT_STAMP" \
    </dev/null >>"$LIVE_ACTIVE/tmp/live-install.log" 2>&1 &
  LIVE_WORKER_PID=$!
  live_ready_wait=0
  while [ ! -f "$LIVE_TXN/ready" ]; do
    kill -0 "$LIVE_WORKER_PID" 2>/dev/null || return 1
    [ "$live_ready_wait" -lt 5 ] || return 1
    sleep 1
    live_ready_wait=$((live_ready_wait + 1))
  done
  LIVE_TRANSFERRED=yes
  [ -n "$LIVE_PARENT_STAMP" ] || return 4
  return 0
}

case "$1" in
  --library) return 0 ;;
  prepare) shift; live_prepare "$@"; exit $? ;;
  finish)
    LIVE_TXN=$2 LIVE_TOKEN=$3 LIVE_SHELL_PID=$4 LIVE_SHELL_STAMP=$5
    LIVE_PARENT_PID=$6 LIVE_PARENT_STAMP=$7
    case "$LIVE_TXN" in "$LIVE_ACTIVE/tmp/live-install."*) ;; *) exit 1 ;; esac
    live_suffix=${LIVE_TXN#"$LIVE_ACTIVE/tmp/live-install."}
    case "$live_suffix" in ''|*[!a-zA-Z0-9]*) exit 1 ;; esac
    [ -d "$LIVE_TXN" ] && [ ! -L "$LIVE_TXN" ] || exit 1
    [ ! -L "$LIVE_ACTIVE/tmp" ] || exit 1
    trap live_release_lock EXIT
    trap 'exit 1' HUP INT TERM
    : > "$LIVE_TXN/ready" || exit 1
    if live_finish; then
      rm -rf "$LIVE_TXN"
    else
      live_notice cleanup-incomplete '标记清理未完成，暂存副本和备份已保留' "Marker cleanup incomplete; staged copy and backup retained: $LIVE_TXN"
      exit 1
    fi
    ;;
  *) exit 1 ;;
esac

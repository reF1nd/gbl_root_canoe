#!/system/bin/sh

# Flash BDS plus the validated raw fast-boot copy. The header is committed last,
# so an interrupted update is ignored by BDS and falls back to persist/efisp.

if [ "$1" = "--bds-only" ]; then
  bds_only=yes
  part=$2
  bds=$3
  log=$4
  tmp=${5:-${bds}.verify}
else
  bds_only=no
  part=$1
  bds=$2
  image=$3
  header=$4
  log=$5
  tmp=${6:-${image}.verify}
fi

HEADER_OFFSET=524288
HEADER_BYTES=4096
IMAGE_OFFSET=528384
TAIL_RESERVE=1048576
LEGACY_HEADER_BLOCK=1024
HEADER_BLOCK=$((HEADER_OFFSET / HEADER_BYTES))
IMAGE_BLOCK=$((IMAGE_OFFSET / HEADER_BYTES))

fail() {
  echo "fast efisp layout: $*" >> "$log"
  exit 1
}

verify_bds() {
  rm -f "$tmp.bds"
  dd if="$part" of="$tmp.bds" bs=$HEADER_BYTES count=$(((bds_size + HEADER_BYTES - 1) / HEADER_BYTES)) \
    >> "$log" 2>&1 || fail "BDS readback failed"
  bds_expected=$(sha256sum "$bds" | cut -d' ' -f1)
  bds_actual=$(head -c "$bds_size" "$tmp.bds" | sha256sum | cut -d' ' -f1)
  rm -f "$tmp.bds"
  [ "$bds_expected" = "$bds_actual" ] || fail "BDS verify failed"
}

[ -b "$part" ] || fail "partition not found"
[ -f "$bds" ] || fail "BDS missing"

part_size=$(blockdev --getsize64 "$part" 2>/dev/null) || fail "size unavailable"
bds_size=$(wc -c < "$bds" | tr -d '[:space:]')
[ "$bds_size" -le "$part_size" ] || fail "partition smaller than BDS"

blockdev --setrw "$part" >> "$log" 2>&1 || fail "setrw failed"

if [ "$bds_only" = "yes" ]; then
  [ "$bds_size" -le "$HEADER_OFFSET" ] || fail "BDS overlaps fast header"
  dd if="$bds" of="$part" bs=4M conv=fsync >> "$log" 2>&1 || fail "BDS write failed"
  sync
  verify_bds
  exit 0
fi

[ -f "$image" ] && [ -f "$header" ] || fail "input missing"
image_size=$(wc -c < "$image" | tr -d '[:space:]')
header_size=$(wc -c < "$header" | tr -d '[:space:]')
image_blocks=$(((image_size + HEADER_BYTES - 1) / HEADER_BYTES))

[ "$header_size" -eq "$HEADER_BYTES" ] || fail "bad fast header size"
[ "$image_size" -gt 0 ] || fail "empty fast image"

# Invalidate first. Until the final header write, every intermediate state
# takes the normal persist/menu path on the next boot.
if [ "$part_size" -ge $((HEADER_OFFSET + HEADER_BYTES)) ]; then
  dd if=/dev/zero of="$part" bs=$HEADER_BYTES seek=$HEADER_BLOCK count=1 conv=fsync \
    >> "$log" 2>&1 || fail "header invalidation failed"
fi
if [ "$part_size" -ge $(((LEGACY_HEADER_BLOCK + 1) * HEADER_BYTES)) ]; then
  dd if=/dev/zero of="$part" bs=$HEADER_BYTES seek=$LEGACY_HEADER_BLOCK count=1 conv=fsync \
    >> "$log" 2>&1 || fail "legacy header invalidation failed"
fi

if [ "$bds_size" -gt "$HEADER_OFFSET" ] || \
   [ $((IMAGE_OFFSET + image_blocks * HEADER_BYTES + TAIL_RESERVE)) -gt "$part_size" ]; then
  echo "fast efisp layout: image does not fit, BDS-only fallback" >> "$log"
  dd if="$bds" of="$part" bs=4M conv=fsync >> "$log" 2>&1 || fail "BDS write failed"
  sync
  verify_bds
  exit 0
fi

dd if="$image" of="$part" bs=$HEADER_BYTES seek=$IMAGE_BLOCK conv=fsync \
  >> "$log" 2>&1 || fail "fast image write failed"
dd if="$bds" of="$part" bs=4M conv=fsync >> "$log" 2>&1 || fail "BDS write failed"
dd if="$header" of="$part" bs=$HEADER_BYTES seek=$HEADER_BLOCK count=1 conv=fsync \
  >> "$log" 2>&1 || fail "header commit failed"
sync

rm -f "$tmp.header" "$tmp.image"
verify_bds

dd if="$part" of="$tmp.header" bs=$HEADER_BYTES skip=$HEADER_BLOCK count=1 \
  >> "$log" 2>&1 || fail "header readback failed"
header_expected=$(sha256sum "$header" | cut -d' ' -f1)
header_actual=$(sha256sum "$tmp.header" | cut -d' ' -f1)
[ "$header_expected" = "$header_actual" ] || fail "header verify failed"

dd if="$part" of="$tmp.image" bs=$HEADER_BYTES skip=$IMAGE_BLOCK count=$image_blocks \
  >> "$log" 2>&1 || fail "image readback failed"
expected=$(sha256sum "$image" | cut -d' ' -f1)
actual=$(head -c "$image_size" "$tmp.image" | sha256sum | cut -d' ' -f1)
rm -f "$tmp.header" "$tmp.image"
[ "$expected" = "$actual" ] || fail "image verify failed"

exit 0

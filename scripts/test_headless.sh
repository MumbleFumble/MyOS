#!/usr/bin/env bash
set -e

ISO=/mnt/c/Users/marpl/Documents/Projects/MyOS/build/myos.iso
DISK=/mnt/c/Users/marpl/Documents/Projects/MyOS/build/disk.img
SERIAL=/tmp/myos_serial.txt
SOCK=/tmp/qemu_mon.sock

rm -f "$SERIAL" "$SOCK"

qemu-system-x86_64 \
  -cdrom "$ISO" \
  -drive file="$DISK",format=raw,if=ide,index=0 \
  -boot d -m 512M \
  -serial file:"$SERIAL" \
  -monitor unix:"$SOCK",server,nowait \
  -display none &

QPID=$!
echo "[test] QEMU pid=$QPID, booting..."
sleep 6
echo "[test] boot complete"

# Build up all sendkey commands as a single stream piped to socat,
# keeping the connection alive until we're done then sending "quit".
{
  # Helper: type a word, then press Enter
  type_word() {
    local str="$1"
    local i
    for (( i=0; i<${#str}; i++ )); do
      local c="${str:$i:1}"
      case "$c" in
        ' ') key='spc' ;;
        '.') key='dot' ;;
        '-') key='minus' ;;
        '_') key='underscore' ;;
        '/') key='slash' ;;
        *)   key="$c" ;;
      esac
      echo "sendkey $key"
      sleep 0.06
    done
    echo "sendkey ret"
    sleep 0.6   # wait for shell to process + print output
  }

  type_word "date"
  type_word "ls"
  type_word "cat motd"
  type_word "cat test.txt"
  type_word "getpid"
  type_word "memtest"

  sleep 0.5
  echo "quit"
} | socat - UNIX-CONNECT:"$SOCK"

wait "$QPID" 2>/dev/null || true

echo ""
echo "=== SERIAL OUTPUT ==="
cat "$SERIAL"

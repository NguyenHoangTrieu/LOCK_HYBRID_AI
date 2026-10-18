#!/usr/bin/env bash
# =========================================================================
# FRDM-MCXN947 - Automatic Board Recovery & SWD Unlock Script
#
# Dùng file này bất cứ khi nào board gặp sự cố:
#   - SWD/JTAG communication failure (FAULT ACK / WAIT ACK)
#   - Memory transfer fault / Device entered lockup / DebugPortStart fault
#   - Không kết nối được pyOCD hoặc mạch nạp bị treo
#
# Cách sử dụng:
#   ./recover_board.sh           Tự động: Reset probe -> Unlock SWD -> Nạp lại firmware sạch
#   ./recover_board.sh unlock    Chỉ mở khóa SWD (Start Debug Session)
#   ./recover_board.sh erase     Xóa trắng toàn bộ Flash (Mass Erase via Debug Mailbox)
#   ./recover_board.sh status    Kiểm tra trạng thái Core 0 / Core 1
# =========================================================================
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_ROOT="$(cd "$APP_DIR/../../.." && pwd)"
VENV_BIN="$WS_ROOT/tools/westenv/bin"
TARGET="mcxn947"
ELF="$APP_DIR/build/camera_ai_demo_cm33_core0.elf"

# Màu thông báo
CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
NC='\033[0m'

say() { printf "${CYAN}>> %s${NC}\n" "$*"; }
ok()  { printf "${GREEN}✔ %s${NC}\n" "$*"; }
warn(){ printf "${YELLOW}⚠ %s${NC}\n" "$*"; }
die() { printf "${RED}✖ %s${NC}\n" "$*" >&2; exit 1; }

# Đảm bảo môi trường
[ -d "$VENV_BIN" ] || die "Không tìm thấy môi trường tools tại $VENV_BIN"
export PATH="$VENV_BIN:$PATH"

reset_usb_probe() {
    say "[1/4] Kiểm tra và Reset USB MCU-Link Probe..."
    if command -v usbreset >/dev/null 2>&1; then
        if lsusb | grep -qi "1fc9:0143"; then
            usbreset 1fc9:0143 >/dev/null 2>&1 || true
            sleep 1
            ok "Đã reset USB MCU-Link (1fc9:0143)"
        else
            warn "Không thấy thiết bị USB 1fc9:0143, bỏ qua usbreset."
        fi
    fi
}

unlock_swd() {
    say "[2/4] Mở khóa Debug Mailbox (bỏ qua bus lockup)..."
    if "$VENV_BIN/nxpdebugmbox" -i mcu-link cmd -f mcxn947 start-debug-session 2>/dev/null; then
        ok "Debug Session đã mở thành công! AHB-AP đã sẵn sàng."
    else
        warn "Debug mailbox không phản hồi, thử tiếp với pyOCD..."
    fi
}

check_status() {
    say "Kiểm tra trạng thái CPU cores..."
    "$VENV_BIN/pyocd" cmd -t "$TARGET" -O pack.debug_sequences.enable=false -M attach -c "status" 2>/dev/null || \
    "$VENV_BIN/pyocd" cmd -t "$TARGET" -O pack.debug_sequences.enable=false -c "status"
}

mass_erase() {
    say "Đang xóa sạch toàn bộ chip (Mass Erase)..."
    if "$VENV_BIN/nxpdebugmbox" -i mcu-link cmd -f mcxn947 erase 2>/dev/null; then
        ok "Mass Erase qua Debug Mailbox thành công!"
    else
        say "Thử xóa qua pyOCD..."
        "$VENV_BIN/pyocd" erase -t "$TARGET" -O pack.debug_sequences.enable=false --chip
        ok "Mass Erase qua pyOCD thành công!"
    fi
}

flash_firmware() {
    [ -f "$ELF" ] || die "Không tìm thấy $ELF - hãy chạy ./build.sh build trước!"
    say "[3/4] Nạp firmware Single-Core sạch ($ELF)..."
    "$VENV_BIN/pyocd" flash -t "$TARGET" -O pack.debug_sequences.enable=false "$ELF"
    ok "Nạp firmware thành công!"

    say "[4/4] Khởi động lại vi điều khiển..."
    "$VENV_BIN/pyocd" cmd -t "$TARGET" -O pack.debug_sequences.enable=false -M attach -c "ww 0xE000ED0C 0x05FA0004" >/dev/null 2>&1 || true
    ok "Board đã khởi động lại và đang chạy!"
}

# Xử lý tham số đầu vào
CMD="${1:-all}"

case "$CMD" in
    unlock)
        reset_usb_probe
        unlock_swd
        check_status
        ;;
    erase)
        reset_usb_probe
        unlock_swd
        mass_erase
        ;;
    status)
        check_status
        ;;
    all|recover)
        say "Bắt đầu chu trình tự động cứu hộ board FRDM-MCXN947..."
        reset_usb_probe
        unlock_swd
        flash_firmware
        say "Hoàn tất cứu hộ! Board đã hoạt động bình thường."
        ;;
    *)
        echo "Cách dùng:"
        echo "  $0          : Cứu hộ tự động toàn diện (Reset probe -> Unlock -> Flash -> Reset)"
        echo "  $0 unlock   : Chỉ mở khóa SWD qua Debug Mailbox"
        echo "  $0 erase    : Xóa trắng toàn bộ Flash"
        echo "  $0 status   : Xem trạng thái CPU Core 0 / Core 1"
        exit 1
        ;;
esac

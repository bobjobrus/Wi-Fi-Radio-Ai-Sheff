#!/bin/bash
# Установка моста раций на Linux-сервер (Ubuntu) как службы systemd.
#   sudo ./install_linux.sh            — из папки с walkie_hub.py, wt_proto.py и hub.json
# Служба: walkie-hub (автозапуск, перезапуск при сбое). Журнал: journalctl -u walkie-hub -f
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${SUDO_USER:-$(whoami)}"
test -f "$DIR/hub.json" || { echo "нет $DIR/hub.json (скопируйте hub.example.json и впишите ключ)"; exit 1; }
cat > /etc/systemd/system/walkie-hub.service <<UNIT
[Unit]
Description=Мост WiFi-раций
After=network-online.target
Wants=network-online.target

[Service]
User=$USER_NAME
WorkingDirectory=$DIR
ExecStart=/usr/bin/python3 $DIR/walkie_hub.py --config $DIR/hub.json
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT
# самопроверка раз в 2 минуты: не ответил — перезапуск
cat > /etc/systemd/system/walkie-hub-check.service <<UNIT
[Unit]
Description=Самопроверка моста WiFi-раций

[Service]
Type=oneshot
ExecStart=/bin/sh -c '/usr/bin/python3 $DIR/healthcheck.py --config $DIR/hub.json || { logger -t walkie-hub "мост не ответил на самопроверку — перезапуск"; systemctl restart walkie-hub; }'
UNIT
cat > /etc/systemd/system/walkie-hub-check.timer <<UNIT
[Unit]
Description=Самопроверка моста WiFi-раций раз в 2 минуты

[Timer]
OnBootSec=3min
OnUnitActiveSec=2min

[Install]
WantedBy=timers.target
UNIT
systemctl daemon-reload
systemctl enable walkie-hub
systemctl restart walkie-hub      # подхватить новый код, если мост уже работал
systemctl enable --now walkie-hub-check.timer
# если включён ufw — открыть порты моста
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
  ufw allow 47000/udp
  ufw allow 47080/tcp
fi
sleep 1
systemctl --no-pager status walkie-hub | head -5

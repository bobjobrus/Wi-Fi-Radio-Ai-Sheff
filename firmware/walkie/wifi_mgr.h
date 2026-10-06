// Wi-Fi: до трёх сетей (подключаемся к самой сильной из знакомых); нет связи — точка доступа RADIO-XXXX
// со страницей настройки (открывается на телефоне сама, как в гостинице).
#pragma once

void wifi_begin(bool force_portal);
void wifi_loop();
void wifi_reconnect();          // сети поменяли — переподключиться
bool wifi_portal_active();

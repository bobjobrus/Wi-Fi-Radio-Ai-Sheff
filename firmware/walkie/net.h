// Связь с мостом: HELLO, запрос эфира, отправка/приём речи, обновление прошивки с моста.
#pragma once
#include <stdint.h>

void net_begin();
void net_ptt(bool down);          // из кнопки
void net_server_changed();        // поменяли адрес моста или ключ — переподключиться

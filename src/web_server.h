#pragma once
#include <WebServer.h>

extern WebServer g_webServer;

void webServerSetup();
void webServerLoop(); // вызывать каждую итерацию main loop() -- WebServer синхронный

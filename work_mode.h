#ifndef LAMPGO_WORK_MODE_H
#define LAMPGO_WORK_MODE_H

namespace WorkMode {

bool start();
bool connectNetwork();
bool startServices();

void loop();

bool isActive();

unsigned long uptimeSeconds();

}

#endif

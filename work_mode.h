#ifndef LAMPGO_WORK_MODE_H
#define LAMPGO_WORK_MODE_H

namespace WorkMode {

bool connectNetwork();
bool startServices();

void loop();

bool isActive();

}

#endif

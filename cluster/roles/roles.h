#ifndef PICOCLUSTER_ROLES_H
#define PICOCLUSTER_ROLES_H

#include "../detect/detect.h"

// Each role entry point — called from main after detection
// These functions never return (they contain the main loop)

void role_head_run(void);       // W5500 detected — gateway/scheduler
void role_storage_run(void);    // SD detected — card server + compute
void role_worker_run(void);     // Neither — pure compute

#endif // PICOCLUSTER_ROLES_H

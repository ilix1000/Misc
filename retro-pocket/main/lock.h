#pragma once
/* One recursive lock shared by the app task and the web server task, guarding
 * the game library (scan results, favourites, recents) and the bridge. */
#ifdef SIM
#include <pthread.h>
typedef pthread_mutex_t lib_lock_t;
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif
void lib_lock_init(void); /* call once at boot, before any task uses it */
void lib_lock(void);
void lib_unlock(void);

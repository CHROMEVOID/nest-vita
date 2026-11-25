/* Nest port: force-included into every source file (-include port/port.h).
   Nest's code keeps its Vita paths (ux0:, app0:) and calls; this header and
   port/*.c make them mean the right thing on Android and on a PC. */
#ifndef NEST_PORT_H
#define NEST_PORT_H

#define NEST_PORT 1
#ifndef __vita__
#define __vita__ 1 /* take Nest's Vita code paths: the port provides those calls */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "ux0:data/Nest/nest.db" -> where that lives on this device */
const char *port_path(const char *vita_path, char *out, size_t n);
FILE *port_fopen(const char *path, const char *mode);
/* what "ux0:", "uma0:" ... are on this device, for the folder picker */
int port_devices(const char **ids, const char **names, int max);
/* how the screen is set up: Nest draws at 960x544, scaled to fit */
void port_screen_scale(float *scale, float *ox, float *oy);

#ifdef __cplusplus
}
#endif

#define fopen(p, m) port_fopen((p), (m))

#ifdef _WIN32
#define memalign(a, s) malloc(s)
#endif

/* SDL owns the program's entry point on Android (and Windows) */
#define main SDL_main

#endif

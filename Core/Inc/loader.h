#ifndef LOADER_H
#define LOADER_H

/* Load a raw .bin module from SD into the reserved RAM region and run it.
 * Returns the module's own return value, or -1 if loading failed. */
int Loader_RunModule(const char *path);

#endif /* LOADER_H */

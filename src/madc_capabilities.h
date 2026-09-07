/* madc_capabilities.h — machine-readable CLI capability manifest. */

#ifndef __MADC_CAPABILITIES_H
#define __MADC_CAPABILITIES_H 1

// Print the stable, build-specific capability manifest and return to the CLI.
// This path is intentionally source-free and does not initialize a Program.
void madc_print_capabilities_json();

#endif // __MADC_CAPABILITIES_H

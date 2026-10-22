/* Macros: small G-code files the operator picks on the Macros page. Nothing is built in -
 * Sea Otter lists whatever .nc files are in the macro folders:
 *
 *   <app folder> macro-*.nc     /ddcsv4/macro-*.nc on the controller: installed and updated
 *                               with the USB package (the package brings a default set)
 *   <programs>/macros  (*.nc)  /local/macros/ = the CNCDISK network share, folder "macros":
 *                               add or edit your own from a PC, no USB stick needed
 *
 * A macro file is plain G-code. Optional header comments at the top:
 *   (name: Park)                shown on the Macros page (default: the file name)
 *   (info: Z up, then XY ...)   one line under the name
 *   (needs: homed)              refused until the machine is homed
 * and {placeholders}, filled in when the macro is armed:
 *   {jog.safe_z}  {macro.park_x}  ...   any setting key (Settings / API names)
 *   {machine.x} {work.z} ...            the position at that moment (x y z a)
 *   {max:A,B}  {min:A,B}                A, B = numbers or any of the names above
 *   {tool} {tool.prev} {tool.dia} {tool.len}   tool-change macros: the tool an M6 asks for,
 *                               the one in now, the new one's size (Tools table)
 * Tool-change macros (Settings > Tool change > Run a macro) are found by name in the same
 * folders: toolchange-T<n>.nc (one per tool) first, else toolchange.nc (all tools).
 * Running one is like running a program: it is armed on the Run page, Start runs it, Pause
 * holds it, Reset stops it - and afterwards the loaded program is back as it was. */
#ifndef MACRO_H
#define MACRO_H
#include "../common.h"

#define MACRO_MAX 40
typedef struct {
	char name[48], info[96], path[256];
	bool needs_homed;
	bool user;                       /* from the network-share folder (vs the package) */
} macro_t;

extern char macro_dir_sys[256], macro_dir_user[256];   /* set by main; "" = not used */

int macro_scan(macro_t *list, int max);                 /* sorted by name; returns the count */
int macro_find(const char *name_or_file, macro_t *out); /* 0 found */
/* the file with its {placeholders} filled in; malloc'd text, or NULL with err set */
char *macro_expand_file(const char *path, size_t *len, char *err, size_t errsz);
int macro_expand(const char *src, size_t n, char *out, size_t cap, char *err, size_t errsz);

#endif

/* progblank.h -- blank projects of the other programs (New case > CalculiX, Chrono or DEME > Blank case): a part of
 * your own (STL or OBJ) or a plain block. No graphics: the UI is in page_cases.c. */
#ifndef QC_PROGBLANK_H
#define QC_PROGBLANK_H
#include <stdbool.h>

/* a surface (STL, ASCII or binary, or Wavefront OBJ) scaled by scale (to metres), centred on x and y and standing on
   z = 0, its shared corners welded, written as an OBJ; lo/hi its bounds after. False with why in err */
bool part_to_obj(const char *src, double scale, const char *dstObj, double lo[3], double hi[3], char *err, int cap);
/* a closed block size[0] x size[1] x size[2] with a corner at the origin, as an ASCII STL */
bool block_stl(const char *path, const double size[3], char *err, int cap);

/* Chrono: CMakeLists.txt, src/body.cpp, params.ini; part (may be NULL: the block of size block[], metres) is dropped
   onto the ground. scale turns the part's units to metres */
bool chrono_write_blank(const char *dir, const char *part, double scale, const double block[3], char *err, int cap);
/* DEME: CMakeLists.txt, src/pour.cpp + qc_deme.hpp, params.ini; grains poured onto the part (NULL: into a box of
   size box[], metres) */
bool deme_write_blank(const char *dir, const char *part, double scale, const double box[3], char *err, int cap);
#endif

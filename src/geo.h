#ifndef __GEO_H__
#define __GEO_H__

#include "server.h"

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
struct geoPoint {
    double longitude;
    double latitude;
    double dist;
    double score;
    char *member;
};

struct geoArray {
    geoPoint *array;
    size_t buckets;
    size_t used;
};

#endif

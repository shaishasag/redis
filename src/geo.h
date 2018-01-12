#ifndef __GEO_H__
#define __GEO_H__

#include "server.h"

struct GeoHashBits;
struct GeoHashRadius;

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
struct geoPoint {
    ~geoPoint();

    char* pop_member()
    {
        char* retVal = m_member;
        m_member = NULL;
        return retVal;
    }

    double m_longitude;
    double m_latitude;
    double m_dist;
    double m_score;
    char*  m_member;
};

class geoArray
{
public:
    geoArray();
    ~geoArray();

    int membersOfAllNeighbors(robj *zobj, const GeoHashRadius& n, double lon, double lat, double radius);

    inline size_t used() const {return m_used;}
    inline geoPoint& operator[](const size_t index) {return *(m_array+index);}

    void sort_array(const int sort_direction);

private:
    geoPoint& geoArrayAppend();
    int geoAppendIfWithinRadius(double lon, double lat, double radius, double score, sds member);
    int geoGetPointsInRange(robj *zobj, double min, double max, double lon, double lat, double radius);
    int membersOfGeoHashBox(robj *zobj, const GeoHashBits& hash, double lon, double lat, double radius);

    geoPoint *m_array;
    size_t m_buckets;
    size_t m_used;
};

#endif

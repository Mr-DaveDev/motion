/*    track.hpp
 *
 *    Experimental motion tracking.
 *
 *    Copyright 2000, Jeroen Vreeken
 *    This program is published under the GNU Public license
 */

#ifndef _INCLUDE_TRACK_H
#define _INCLUDE_TRACK_H

struct ctx_coord;

struct ctx_track {
    int             dev;
    int     maxx;
    int     minx;
    int     maxy;
    int     miny;

    int    pan_angle; // degrees
    int    tilt_angle; // degrees
    int    posx;
    int    posy;
    int    minmaxfound;
};


enum track_action { TRACK_CENTER, TRACK_MOVE };

void track_init(struct ctx_cam *cam);
void track_deinit(struct ctx_cam *cam);

int track_center(struct ctx_cam *cam, int dev,
        int manual, int xoff, int yoff);

int track_move(struct ctx_cam *cam, int dev,struct ctx_coord *cent
        , struct ctx_images *imgs, int manual);

#endif /* _INCLUDE_TRACK_H */

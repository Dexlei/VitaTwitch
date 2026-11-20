#ifndef VT_CHAT_VIEW_H
#define VT_CHAT_VIEW_H

#include <vita2d.h>

/* The chat panel is drawn into its own texture only when something changed
 * (new messages, an emote finished loading, status text), at most a few times
 * per second; every video frame just draws that texture. Drawing hundreds of
 * glyphs per frame did not fit next to 60 fps decoding in busy chats.
 *
 * chat_view_update(): call OUTSIDE a vita2d scene (before vita2d_start_drawing).
 * chat_view_draw():   call inside the frame's scene. */
void chat_view_update(int w, int h, vita2d_pgf *f, const char *channel);
void chat_view_draw(int x, int y);
void chat_view_free(void);

/* stats for the log */
int chat_view_renders(void);
int chat_view_avg_ms_x10(void);

#endif

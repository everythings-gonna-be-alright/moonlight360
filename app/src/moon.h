/* The Play tab's night sky: a moon that floats in a breathing halo among twinkling stars, with random events now and
 * then (shooting stars, a passing cloud, a star's glint, a satellite). Drawing thread only, like gfx.h; timed by
 * gfx_frame at the menus' 60 frames a second. */
#pragma once

/* The moon centred on (cx, cy) with radius r; the sky's events use about 2.4 r around it. */
void moon_draw(float cx, float cy, float r);

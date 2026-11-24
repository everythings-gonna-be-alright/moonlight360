/* The Play tab's night sky: a moon that floats in a breathing halo among twinkling stars, with random events now and
 * then (shooting stars, a passing cloud, a star's glint, a satellite) and rare ones minutes apart (birds across the
 * moon, a comet, a meteor shower, a blood moon, a UFO, fireflies, a constellation, a plane, an aurora). Drawing
 * thread only, like gfx.h. Its clock counts gfx_frame at the menus' 60 frames a second, only while it's drawn. */
#pragma once

/* The moon centred on (cx, cy) with radius r; the sky's events use about 2.4 r around it. */
void moon_draw(float cx, float cy, float r);
/* Runs the sky's clock on by seconds without drawing (dev/menu_preview); what comes due starts at the next draw. */
void moon_skip(float seconds);

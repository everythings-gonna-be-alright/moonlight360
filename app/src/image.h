/* Image files to pixels. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decodes a PNG or JPEG and fits it to width x height (cropped to that shape around its centre), as bytes red,
 * green, blue, alpha, row after row. NULL if it can't; free() the result. */
uint8_t *image_decode_fit(const uint8_t *data, size_t size, int width, int height);

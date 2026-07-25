#pragma once

extern struct
{
    unsigned int  width;
    unsigned int  height;
    unsigned int  bytes_per_pixel;
    unsigned char pixel_data[32 * 32 * 4 + 1];
} gimp_image;

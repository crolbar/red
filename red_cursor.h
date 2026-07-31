#pragma once

extern struct
{
    unsigned int  width;
    unsigned int  height;
    unsigned int  bytes_per_pixel;
    unsigned char pixel_data[20 * 20 * 4 + 1];
} gimp_image;

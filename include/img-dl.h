/* Copyright (C) 2016 Ben Combee.  All rights reserved */

#ifndef DLIMG_DLIMG_H_
#define DLIMG_DLIMG_H_

#include <pebble.h>
#include <inttypes.h>

typedef struct ImgDLContext ImgDLContext;

enum DL_IMG_STATUS {
    DLIMG_READY,    // the JavaScript code is now ready to send you images
    DLIMG_ERROR,    // an error occured, see msg for details
    DLIMG_START,    // a download is starting, do not use the bitmap until you see DLIMG_COMPLETE
    DLIMG_COMPLETE  // download is complete, you can use the GBitmap returned in the init call
};

typedef void (*dl_img_callback)(ImgDLContext *ctx, int status, const char *msg);

// call this before calling events_app_message_open.
ImgDLContext *dl_img_init(uint16_t width, uint16_t height, dl_img_callback callback);

// call to get the bitmap associated with the download.  This does not transfer
// ownership of the bitmap to the caller; it will be freed only when dl_img_deinit
// is called
GBitmap *dl_img_get_bitmap(ImgDLContext *ctx);

// call this at the end of your program
void dl_img_deinit(ImgDLContext *ctx);

#endif // DLIMG_DLIMG_H_

/* Copyright (C) 2015-2016 Ben Combee.  All rights reserved */

#include "img-dl.h"
#include <pebble-events/pebble-events.h>
    
static char sErrorMsg[32];

/* message keys used:

    READY: sent by JS layer to tell library that it can receive messages
    CHUNK_SIZE: sent by app to indicate largest chunk size supported and bitmap width/height
    WIDTH: sent by app to indicate image height requested
    HEIGHT: sent by app to indicate image width requested
    BEGIN: sent at start of image, contains uint32_t image size
    DATA: transmit image data, contains byte array
    END: sent when image transmission is over
    ERROR: string with error message from JS side, usually a network failure
*/

typedef struct ImgDLContext {
    /* size of the data buffer allocated */
    uint32_t length;
    /* buffer of data that will contain the actual data */
    GBitmap *bitmap;
    /* Next byte to write */
    uint32_t index;
    /* width and height */
    uint16_t width;
    uint16_t height;
    /* Callback to call when we are done loading the data */
    dl_img_callback callback;
    /* message handlers handle */
    EventHandle appMessageHandlers;
} ImgDLContext;

static char *translate_error(AppMessageResult result) {
    switch (result) {
        case APP_MSG_OK: return "APP_MSG_OK";
        case APP_MSG_SEND_TIMEOUT: return "APP_MSG_SEND_TIMEOUT";
        case APP_MSG_SEND_REJECTED: return "APP_MSG_SEND_REJECTED";
        case APP_MSG_NOT_CONNECTED: return "APP_MSG_NOT_CONNECTED";
        case APP_MSG_APP_NOT_RUNNING: return "APP_MSG_APP_NOT_RUNNING";
        case APP_MSG_INVALID_ARGS: return "APP_MSG_INVALID_ARGS";
        case APP_MSG_BUSY: return "APP_MSG_BUSY";
        case APP_MSG_BUFFER_OVERFLOW: return "APP_MSG_BUFFER_OVERFLOW";
        case APP_MSG_ALREADY_RELEASED: return "APP_MSG_ALREADY_RELEASED";
        case APP_MSG_CALLBACK_ALREADY_REGISTERED: return "APP_MSG_CALLBACK_ALREADY_REGISTERED";
        case APP_MSG_CALLBACK_NOT_REGISTERED: return "APP_MSG_CALLBACK_NOT_REGISTERED";
        case APP_MSG_OUT_OF_MEMORY: return "APP_MSG_OUT_OF_MEMORY";
        case APP_MSG_CLOSED: return "APP_MSG_CLOSED";
        case APP_MSG_INTERNAL_ERROR: return "APP_MSG_INTERNAL_ERROR";
        default: return "UNKNOWN ERROR";
    }
}

static void init_downloader(ImgDLContext *ctx) {
    uint32_t inbox_max = app_message_inbox_size_maximum();
    DictionaryIterator *outbox;
    app_message_outbox_begin(&outbox);

    // need size for sending a buffer with a single tuple header
    uint32_t dict_size = dict_calc_buffer_size(1, 0);
    uint32_t chunk_size = inbox_max - dict_size;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "NETDL_CHUNK_SIZE: inbox_max %" PRIu32 " dict %" PRIu32 " chunk %" PRIu32,
            inbox_max, dict_size, chunk_size);

    dict_write_uint32(outbox, MESSAGE_KEY_CHUNK_SIZE, chunk_size);
    dict_write_uint16(outbox, MESSAGE_KEY_WIDTH,      ctx->width);
    dict_write_uint16(outbox, MESSAGE_KEY_HEIGHT,     ctx->height);
    app_message_outbox_send();
}

// unpacked: --AAAAA --BBBBBB --CCCCCC --DDDDDD
//   packed: AAAAAABB BBBBCCCC CCDDDDDD
static void unpackImage(uint8_t *data, uint32_t packedLength) {
    for (int32_t i = packedLength - 3, j = packedLength / 3 * 4 - 4; i >= 0; i -= 3, j -= 4) {
        data[j + 3] = 0xC0 | (data[i + 2] & 0x3F);
        data[j + 2] = 0xC0 | ((data[i + 2] & 0xC0) >> 6) | ((data[i + 1] & 0x0F) << 2);
        data[j + 1] = 0xC0 | ((data[i + 1] & 0xF0) >> 4) | ((data[i] & 0x03) << 4);
        data[j]     = 0xC0 | ((data[i] & 0xFC) >> 2);
    }
}
    
static void netdownload_receive(DictionaryIterator *iter, void *context) {
    ImgDLContext *ctx = (ImgDLContext*) context;
    Tuple *tuple = dict_read_first(iter);
    if (!tuple) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Got a message with no first key! Size of message: %" PRIo32, (uint32_t)iter->end - (uint32_t)iter->dictionary);
        return;
    }
    while (tuple) {
        if (tuple->key == MESSAGE_KEY_READY) {
            init_downloader(ctx);
            ctx->callback(ctx, DLIMG_READY, NULL);
        }
        else if (tuple->key == MESSAGE_KEY_DATA) {
            if (ctx->index + tuple->length <= ctx->length) {
                memcpy(gbitmap_get_data(ctx->bitmap) + ctx->index, tuple->value->data, tuple->length);
                ctx->index += tuple->length;
            }
            else {
                APP_LOG(APP_LOG_LEVEL_WARNING, "Not overriding rx buffer. Bufsize=%" PRIu32 " "
                        "BufIndex=%" PRIu32 " DataLen=%" PRIu16,
                        ctx->length, ctx->index, tuple->length);
            }
        }
        else if (tuple->key == MESSAGE_KEY_BEGIN) {
            APP_LOG(APP_LOG_LEVEL_DEBUG, "Start transmission. Size=%" PRIu32, tuple->value->uint32);
            ctx->length = tuple->value->uint32;
            // limit length to size of buffer
            if (ctx->length > (uint32_t)ctx->width * (uint32_t)ctx->height)
                ctx->length = ctx->width * ctx->height;
            ctx->index = 0;
            ctx->callback(ctx, DLIMG_START, NULL);
        }
        else if (tuple->key == MESSAGE_KEY_END) {
            app_comm_set_sniff_interval(SNIFF_INTERVAL_NORMAL);
            if (ctx->length > 0 && ctx->index > 0) {
                printf("Received complete file=%" PRIu32, ctx->length);
                unpackImage(gbitmap_get_data(ctx->bitmap), ctx->length);
                ctx->callback(ctx, DLIMG_COMPLETE, NULL);

                // We have transfered ownership of this memory to the app. Make sure we dont free it.
                // (see netdownload_destroy for cleanup)
                ctx->index = 0;
                ctx->length = 0;
            }
            else {
                APP_LOG(APP_LOG_LEVEL_DEBUG, "Got End message but we have no image...");
            }
        }
        else if (tuple->key == MESSAGE_KEY_ERROR) {
            APP_LOG(APP_LOG_LEVEL_ERROR, "error received: %s", tuple->value->cstring);
            strncpy(sErrorMsg, tuple->value->cstring, sizeof(sErrorMsg));
            sErrorMsg[sizeof(sErrorMsg) - 1] = 0;
            ctx->callback(ctx, DLIMG_ERROR, sErrorMsg);
        }
        tuple = dict_read_next(iter);
    }
}

static void netdownload_dropped(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Dropped message! Reason given: %s", translate_error(reason));
}

static void netdownload_out_success(DictionaryIterator *iter, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Message sent.");
}

static void netdownload_out_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Failed to send message. Reason = %s", translate_error(reason));
}

ImgDLContext *dl_img_init(uint16_t width, uint16_t height, dl_img_callback callback) {

    ImgDLContext *ctx = malloc(sizeof(ImgDLContext));
    ctx->length = 0;
    ctx->index = 0;

    ctx->bitmap = gbitmap_create_blank(GSize(width, height), GBitmapFormat8Bit);
    ctx->width = width;
    ctx->height = height;

    events_app_message_request_inbox_size(app_message_inbox_size_maximum());
    events_app_message_request_outbox_size(64);

    ctx->appMessageHandlers = events_app_message_subscribe_handlers(
        (EventAppMessageHandlers) {
            netdownload_out_success,
            netdownload_out_failed,
            netdownload_receive,
            netdownload_dropped
        }, ctx);

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Max buffer sizes are %" PRIu32 " / %" PRIu32,
            app_message_inbox_size_maximum(),
            app_message_outbox_size_maximum());
    // use largest possible inbox for efficient picture transfer,
    // but outbox can be very small since we never send much data.
    app_message_open(app_message_inbox_size_maximum(), 64);

    return ctx;
}

GBitmap *dl_img_get_bitmap(ImgDLContext *ctx) {
    return ctx->bitmap;
}

void dl_img_deinit(ImgDLContext *ctx) {
    events_app_message_unsubscribe(ctx->appMessageHandlers);
    gbitmap_destroy(ctx->bitmap);
    free(ctx);
}

/* jshint sub: true */
/* globals MessageQueue, JpegImage, vagueTime */

(function() {
    
"use strict";

var JpegImage = require("jpeg.js").JpegImage;
var MessageQueue = require("message-queue.js").MessageQueue;
var MsgKeys = require('message_keys');

/* set minimum based on pebble.h's APP_MESSAGE_INBOX_MINIMUM */
var CHUNK_SIZE = 124;

/* width of rectangular Pebble screen as default */
var IMG_WIDTH = 144;
var IMG_HEIGHT = 168;
    
/* flag used to reject search */
var transferInProgress = false;
             
function timeLog(msg) {
    console.log(new Date().toISOString() + " img-dl: " + msg);
}

var sendSuccess = function(e) {
    timeLog('Delivered transaction ' + e.data.transactionId); 
};

var sendFailure = function(e) {
    timeLog('Cannot deliver transaction ' + e.data.transactionId + ', error ' + e.error.message);
};

var sendError = function(errorMsg) {
    var msg = {};
    msg[MsgKeys.ERROR] = errorMsg;
    MessageQueue.sendAppMessage(msg, sendSuccess, sendFailure);
};

function transferImageBytes(bytes, chunkSize, successCb, failureCb) {
    var success = function() {
        if (successCb !== undefined) {
            successCb();
        }
    };
    var failure = function(e) {
        if (failureCb !== undefined) {
            failureCb(e);
        }
    };

    bytes = packImage(bytes);
    
    // This function sends chunks of data.
    var sendChunk = function(start) {
        var txbuf = Array.prototype.slice.call(bytes, start, start + chunkSize);
        timeLog("Sending " + txbuf.length + " bytes at offset " + start);
        var msg = {};
        msg.MsgKeys.DATA = txbuf;
        MessageQueue.sendAppMessage(
            msg,
            function(e) {
                // If there is more data to send - send it.
                if (bytes.length > start + chunkSize) {
                    sendChunk(start + chunkSize);
                }
                // Otherwise we are done sending. Send closing message.
                else {
                    var msg = {};
                    msg[MsgKeys.END] = 1;
                    MessageQueue.sendAppMessage(msg, success, failure);
                }
            },
            function (e) {
                failure(e);
            }
        );
    };

    // Let the pebble app know how much data we want to send.
    timeLog("Sending BEGIN, bytes.length = " + bytes.length);
    var msg = {};
    msg[MsgKeys.BEGIN] = bytes.length,
    MessageQueue.sendAppMessage(
        msg,
        function (e) {
            // success - start sending
            sendChunk(0);
        }, failure);
}
   
function downloadImage(url) {
    timeLog("downloading photo at url " + url);

    // load image into JPEG decoder library
    var j = new JpegImage();
    j.onload = function() {
        var w = j.width, h = j.height;
        timeLog("decoded image, size: " + w + "x" + h);
        
        var img = {
            width: IMG_WIDTH,
            height: IMG_WIDTH,
            data: new Uint8ClampedArray(IMG_WIDTH * IMG_WIDTH * 4)
        };
        j.copyToImageData(img);

        // now, resample the imgData to BGRA color
        ditherImage(img.data);
        timeLog("dithered image");
        var pebbleImg = downsampleImage(img.data);
        timeLog("downsampled image");

        transferInProgress = true;
        transferImageBytes(
            pebbleImg, CHUNK_SIZE,
            function() { timeLog("Done!"); transferInProgress = false; },
            function(e) { timeLog("Failed! " + e); transferInProgress = false; }
        );
    };
    j.onerror = function() {
        error("JPEG Decoder failed");
    };
    j.load(photo.url);
}

function ditherImage(imgData) {
    for (var i = 0; i < IMG_WIDTH * IMG_WIDTH * 4; i++) {
        // convert in place to 6-bit color with Floyd-Steinberg dithering
        var oldPixel = imgData[i];
        var newPixel = oldPixel & 0xC0;
        imgData[i] = newPixel;
        var quantError = oldPixel - newPixel;
        imgData[i + 4] += quantError * 7 / 16;
        imgData[i + ((IMG_WIDTH - 1) * 4)] += quantError * (3 / 16);
        imgData[i + (IMG_WIDTH * 4)] += quantError * (5 / 16);
        imgData[i + ((IMG_WIDTH + 1) * 4)] += quantError * (1 / 16);
    }
}

function downsampleImage(imgData) {
    var pebbleImg = new Uint8Array(IMG_WIDTH * IMG_WIDTH);
    for (var i = 0, d = 0; i < IMG_WIDTH * IMG_WIDTH; i++, d += 4) {
        pebbleImg[i] =
            0xC0 |
            /* R */ (imgData[d] & 0xC0) >> 2 |
            /* G */ (imgData[d + 1] & 0xC0) >> 4 |
            /* B */ (imgData[d + 2] & 0xC0) >> 6;
    }
    return pebbleImg;
}

// unpacked: --AAAAA --BBBBBB --CCCCCC --DDDDDD
//   packed: AAAAAABB BBBBCCCC CCDDDDDD
function packImage(imgData) {
    var packedImg = new Uint8Array(IMG_WIDTH * IMG_WIDTH * 3 / 4);
    for (var i = 0, j = 0; i < IMG_WIDTH * IMG_WIDTH * 4; i += 4, j += 3) {
        packedImg[j] = ((imgData[i] & 0x3F) << 2)     | ((imgData[i + 1] & 0x30) >> 4);
        packedImg[j + 1] = ((imgData[i + 1] & 0x0F) << 4) | ((imgData[i + 2] & 0x3C) >> 2);
        packedImg[j + 2] = ((imgData[i + 2] & 0x03) << 6) | (imgData[i + 3] & 0x3F);
    }
    return packedImg;
}

var isRound = false;
    
Pebble.addEventListener("ready", function(e) {
    timeLog("JS ready");
    var msg = {};
    msg[MsgKeys.READY] = 1;
    MessageQueue.sendAppMessage( msg, sendSuccess, sendFailure);
});

var lastRequest;    
    
Pebble.addEventListener("appmessage", function(e) {
    timeLog("Got message: " + JSON.stringify(e));
    if (MsgKeys.CHUNK_SIZE in e.payload) {
        CHUNK_SIZE = e.payload[MsgKeys.CHUNK_SIZE];
        timeLog("CHUNK_SIZE = " + CHUNK_SIZE);
    }
    if (MsgKeys.WIDTH in e.payload) {
        IMG_WIDTH = e.payload[MsgKeys.WIDTH];
        timeLog("IMG_WIDTH = " + IMG_WIDTH);
    }
    if (MsgKeys.HEIGHT in e.payload) {
        IMG_HEIGHT = e.payload[MsgKeys.HEIGHT];
        timeLog("IMG_HEIGHT = " + IMG_HEIGHT);
    }
});

exports.downloadImage = downloadImage;

})();
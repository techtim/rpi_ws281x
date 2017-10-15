/*
 * udpWS281x.c
 *
 * WS281x lib by Jeremy Garff with UDP listener by Tim Tavlintsev <tim@tvl.io>
 * Copyright (c) 2014 Jeremy Garff <jer @ jers.net>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *     1.  Redistributions of source code must retain the above copyright notice, this list of
 *         conditions and the following disclaimer.
 *     2.  Redistributions in binary form must reproduce the above copyright notice, this list
 *         of conditions and the following disclaimer in the documentation and/or other materials
 *         provided with the distribution.
 *     3.  Neither the name of the owner nor the names of its contributors may be used to endorse
 *         or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

// compile using: scons
// sudo apt-get install scons

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "clk.h"
#include "dma.h"
#include "gpio.h"
#include "pwm.h"

#include "ws2811.h"

#define ARRAY_SIZE(stuff) (sizeof(stuff) / sizeof(stuff[0]))

#define TARGET_FREQ WS2811_TARGET_FREQ
#define DMA 5
//#define STRIP_TYPE            WS2811_STRIP_RGB        // WS2812/SK6812RGB integrated chip+leds
#define STRIP_TYPE WS2811_STRIP_GBR // WS2812/SK6812RGB integrated chip+leds
//#define STRIP_TYPE            SK6812_STRIP_RGBW       // SK6812RGBW (NOT SK6812RGB)

#define GPIO_PIN_1 12
#define GPIO_PIN_2 13
#define LED_COUNT 1000
#define MESSAGE_SIZE 3000 * 3

#define SERVER_PORT 3001

#define MAX_CHANNELS 2

void stop_program(int sig);

int clear_on_exit = 0;
int continue_looping = 1;

ws2811_t ledstring =
{
    .freq = TARGET_FREQ,
    .dmanum = DMA,
    .channel =
    {
        [0] =
        {
            .gpionum = GPIO_PIN_1,
            .count = LED_COUNT,
            .invert = 0,
            .brightness = 255,
            .strip_type = STRIP_TYPE,
        },
        [1] =
        {
            .gpionum = GPIO_PIN_2,
            .count = LED_COUNT,
            .invert = 0,
            .brightness = 255,
            .strip_type = STRIP_TYPE,
        },
    },
};

static void ctrl_c_handler() { ws2811_fini(&ledstring); }

static void setup_handlers(void)
{
    struct sigaction sa = {
        .sa_handler = ctrl_c_handler,
    };

    sigaction(SIGKILL, &sa, NULL);
}

int main()
{
    int ret = 0;
    setup_handlers();

    // --- WS281x SETUP ---

    if (ws2811_init(&ledstring)) {
        printf("ws2811_init failed!\n");
        return -1;
    }

    // --- UDP SETUP ---
    char message[MESSAGE_SIZE];
    char *pixels;
    int sock;
    struct sockaddr_in name;
    // struct hostent *hp, *gethostbyname();
    int bytes;

    printf("Listen activating.\n");

    /* Create socket from which to read */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Opening datagram socket");
        exit(1);
    }

    /* Bind our local address so that the client can send to us */
    bzero((char *)&name, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    name.sin_port = htons(SERVER_PORT);

    if (bind(sock, (struct sockaddr *)&name, sizeof(name))) {
        perror("binding datagram socket");
        exit(1);
    }

    printf("Socket has port number #%d\n", ntohs(name.sin_port));

    signal(SIGINT, stop_program);

    int i = 0;
    int total_leds_num = 0;
    int chan_cntr = 0, cur_chan;
    int byte_offset = 0, pixel_offset = 0;
    uint16_t chan_leds_num[] = { 0, 0, 0, 0, 0, 0 };
    while (continue_looping) {
        while ((bytes = read(sock, message, MESSAGE_SIZE)) > 0) {
            if (bytes < 4)
                continue;

            chan_cntr = 0;
            while (chan_cntr + 1 < bytes
                   && (message[chan_cntr * 2] != 0xff && message[chan_cntr * 2 + 1] != 0xff)) {
                chan_leds_num[chan_cntr] = message[chan_cntr * 2 + 1] << 8 | message[chan_cntr * 2];
                // printf("%i chan_leds_num : %i\n", chan_cntr, chan_leds_num[chan_cntr]);
                ++chan_cntr;
            }

            byte_offset = chan_cntr * 2 + 2;
            pixels = message + byte_offset;

            if (chan_cntr > MAX_CHANNELS)
                chan_cntr = MAX_CHANNELS;

            total_leds_num = (bytes - byte_offset) / 3;
            // printf("num of chan %i recv leds: %i\n", chan_cntr, total_leds_num);

            pixel_offset = 0;
            for (cur_chan = 0; cur_chan < chan_cntr; ++cur_chan) {
                for (i = pixel_offset;
                     i < chan_leds_num[cur_chan] + pixel_offset && i < total_leds_num; i++) {

                    // printf("%i : %i -> %d  %d  %d\n", cur_chan, i - pixel_offset,
                    //        pixels[i * 3 + 0], pixels[i * 3 + 1],
                    //        pixels[i * 3 + 2]);

                    ledstring.channel[cur_chan].leds[i - pixel_offset]
                        = (pixels[i * 3 + 0] << 16) | (pixels[i * 3 + 1] << 8) | pixels[i * 3 + 2];
                }
                pixel_offset += chan_leds_num[cur_chan];
            }

            if ((ret = ws2811_render(&ledstring)) != WS2811_SUCCESS) {
                fprintf(stderr, "ws2811_render failed: %s\n", ws2811_get_return_t_str(ret));
                break;
            }

            // 15 frames /sec
            // usleep(1000000 / 15);
            usleep(1000);
        }
    }

    ws2811_fini(&ledstring);

    return ret;
}

void stop_program(int sig)
{
    /* Ignore the signal */
    signal(sig, SIG_IGN);
    /* stop the looping */
    continue_looping = 0;
    /* Put the ctrl-c to default action in case something goes wrong */
    signal(sig, SIG_DFL);
}

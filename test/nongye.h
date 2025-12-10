#ifndef __NONGYE_H__
#define __NONGYE_H__    

#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "lvgl/examples/lv_examples.h"
#include "lv_font_source_han_sans_bold.h"
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>


/* 创建界面（主线程） */
void nongye_ui_create();

/* 周期刷新（主线程 5 ms 调用） */
void nongye_ui_refresh();

void nongye_init();

#endif


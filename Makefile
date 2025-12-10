#
# Makefile
#-L/home/gec/freetype-2.12.1/tmp/lib -I/home/gec/freetype-2.12.1/tmp/include/freetype2
CC = arm-linux-gcc
LVGL_DIR_NAME ?= lvgl
LVGL_DIR ?= ${shell pwd}
# CFLAGS = -O0 -g3 -I$(LVGL_DIR)/ -Wall -std=gnu99 -pthread #调试时使用
CFLAGS ?= -O3 -g0   -I$(LVGL_DIR)/ -Wall  -std=gnu99 -pthread
LDFLAGS ?= -lm -pthread
BIN = demo


#Collect the files to compile
MAINSRC = ./main.c 
#MAINSRC = ./main.c ./test.c ./mywin.c  ./chinese_ziku.c  ./lv_font_source_han_sans_bold_20.c
TESTSRC = $(wildcard ./test/*.c) ##1. 添加新的源文件
#TESTSRC = ./test/mywin.c  ./test/chinese_ziku.c  ./test/lv_font_source_han_sans_bold_20.c

include $(LVGL_DIR)/lvgl/lvgl.mk
include $(LVGL_DIR)/lv_drivers/lv_drivers.mk

CSRCS +=$(LVGL_DIR)/mouse_cursor_icon.c 

OBJEXT ?= .o

AOBJS = $(ASRCS:.S=$(OBJEXT))
COBJS = $(CSRCS:.c=$(OBJEXT))

MAINOBJ = $(MAINSRC:.c=$(OBJEXT))
TESTOBJ = $(TESTSRC:.c=$(OBJEXT)) ##2. 添加.o文件

SRCS = $(ASRCS) $(CSRCS) $(MAINSRC)
OBJS = $(AOBJS) $(COBJS)

## MAINOBJ -> OBJFILES


all: default

%.o: %.c
	@$(CC)  $(CFLAGS) -c $< -o $@
	@echo "CC $<"

#3.将新的文件添加到目标文件中    
default: $(AOBJS) $(COBJS) $(MAINOBJ) $(TESTOBJ)
	$(CC) -o $(BIN) $(MAINOBJ) $(TESTOBJ) $(AOBJS) $(COBJS) $(LDFLAGS) 

#4.添加新删除的目标文件
clean: 
	rm -f $(BIN) $(AOBJS) $(COBJS) $(MAINOBJ) $(TESTOBJ)


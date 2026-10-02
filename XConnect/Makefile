# XConnect Linux/macOS 简单构建
# 用法: make SDK=/path/to/XPlaneSDK

CC      ?= gcc
CFLAGS  += -O2 -Wall -fPIC -shared -std=c99 -DLIN
CFLAGS  += -I$(SDK)/XPLM -Isrc
LDLIBS  += -lpthread

SRCS = src/xconnect.c src/json.c src/net.c src/csl.c
OUT  = XConnect.xpl

$(OUT): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDLIBS)

clean:
	rm -f $(OUT)

.PHONY: clean

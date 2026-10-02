# XConnect Linux/macOS 简单构建
# 用法: make SDK=/path/to/XPlaneSDK

CC      ?= gcc
CFLAGS  += -O2 -Wall -fPIC -shared -std=c99 -DLIN
# SDK 430 需显式声明版本宏，否则 XPLMObjectRef 等被条件编译隐藏
CFLAGS  += -DXPLM200 -DXPLM210 -DXPLM300 -DXPLM400 -DXPLM410 -DXPLM420 -DXPLM430
CFLAGS  += -I$(SDK)/XPLM -Isrc
LDLIBS  += -lpthread

SRCS = src/xconnect.c src/json.c src/net.c src/csl.c
OUT  = XConnect.xpl

$(OUT): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDLIBS)

clean:
	rm -f $(OUT)

.PHONY: clean

G++ := /home/zqh/tools/rv1126_rv1109_linux_sdk_v1.8.0_20210224/prebuilts/gcc/linux-x86/arm/gcc-arm-8.3-2019.03-x86_64-arm-linux-gnueabihf/bin/arm-linux-gnueabihf-g++
CFLAGS = -Wall -Wextra
INCLUDE_DIRS = rkmedia librtsp easymedia rknn_rockx_include im2d_api ../tools/arm_opencv_source/include
LIB_DIRS = ./rv1126_lib ./librtsp ../tools/arm_opencv_source/lib

INCLUDE_FLAGS = $(addprefix -I, $(INCLUDE_DIRS))
LDFLAGS = $(addprefix -L, $(LIB_DIRS)) ./librtsp/librtsp.a \
			-lpthread -leasymedia -ldrm -lrockchip_mpp \
	        -lavformat -lavcodec -lswresample -lavutil \
			-lasound -lv4l2 -lv4lconvert -lrga \
			-lRKAP_ANR -lRKAP_Common -lRKAP_3A \
			-lmd_share -lrkaiq -lod_share -lrknn_api \
			-lrockx \
			-lopencv_highgui -lopencv_ml -lopencv_objdetect -lopencv_photo -lopencv_stitching -lopencv_video -lopencv_calib3d -lopencv_features2d -lopencv_dnn -lopencv_flann -lopencv_videoio -lopencv_imgcodecs -lopencv_imgproc -lopencv_core

SRCS = rkmedia_face_recognition.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)

-include $(DEPS)

all : main

main : $(OBJS)
	$(G++) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(INCLUDE_FLAGS)

%.o: %.cpp
	$(G++) $(CFLAGS) -c -o $@ $< -MD -MP $(INCLUDE_FLAGS)

clean:
	rm -f $(DEPS) $(OBJS) main

.PHONY: all clean

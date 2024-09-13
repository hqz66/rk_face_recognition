#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include "rkmedia_api.h"
#include "rkmedia_buffer.h"
#include "rkmedia_venc.h"
#include "rockx.h"
#include "librtsp/rtsp_demo.h"
#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;

#define VI_WIDTH 1920
#define VI_HEIGHT 1080

#define RK_NN_INDEX 1

rtsp_demo_handle g_rtsplive = NULL;
rtsp_session_handle g_rtsp_session;

void ConvertColor(uint8_t *src_ptr, uint8_t *dst_ptr)
{
    // I420转NV12
    memcpy(dst_ptr, src_ptr, VI_WIDTH * VI_HEIGHT);
    for (int i = 0, j=0; j < VI_WIDTH * VI_HEIGHT / 4; i+=2,j++)
    {
        dst_ptr[VI_WIDTH * VI_HEIGHT + i] = src_ptr[VI_WIDTH * VI_HEIGHT + j];
        dst_ptr[VI_WIDTH * VI_HEIGHT + i+1] = src_ptr[VI_WIDTH * VI_HEIGHT + VI_WIDTH * VI_HEIGHT / 4 + j];
    }
}

void video_packet_cb(MEDIA_BUFFER mb)
{
    static RK_S32 packet_cnt = 0;
    // printf("#Get packet-%d, size %zu\n", packet_cnt, RK_MPI_MB_GetSize(mb));
    if (g_rtsplive && g_rtsp_session)
    {
        rtsp_tx_video(g_rtsp_session, (const uint8_t *)RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetSize(mb),
                      RK_MPI_MB_GetTimestamp(mb));
        rtsp_do_event(g_rtsplive);
    }
    RK_MPI_MB_ReleaseBuffer(mb);
    packet_cnt++;
}

void *set_rk_face_detection(void *arg)
{
    printf("%s started\n", __FUNCTION__);
    rockx_ret_t rockx_ret;
    rockx_handle_t object_det_handle;
    rockx_image_t input_image;
    input_image.width = VI_WIDTH;
    input_image.height = VI_HEIGHT;
    input_image.pixel_format = ROCKX_PIXEL_FORMAT_YUV420SP_NV12;
    input_image.is_prealloc_buf = 1;
    // 创建人脸识别句柄
    rockx_config_t *config = rockx_create_config();
    // 配置RockX 模型数据文件（如 .data 文件）所在的目录
    rockx_ret = rockx_add_config(config, ROCKX_CONFIG_DATA_PATH, "/mnt/nfs/rockx_data/");
    // rockx_ret = rockx_add_config(config, ROCKX_CONFIG_RKNN_RUNTIME_PATH, "/usr/lib/librknn_runtime.so");
    if (rockx_ret != ROCKX_RET_SUCCESS)
    {
        printf("rockx_add_config error %d\n", rockx_ret);
        exit(-1);
    }
    rockx_ret = rockx_create(&object_det_handle, ROCKX_MODULE_FACE_DETECTION_V2, config,
                             sizeof(rockx_config_t));
    if (rockx_ret != ROCKX_RET_SUCCESS)
    {
        printf("rockx_create error %d\n", rockx_ret);
        exit(-1);
    }
    // 创建数组保存人脸识别结果
    rockx_object_array_t person_array;
    memset(&person_array, 0, sizeof(person_array));
    MEDIA_BUFFER buffer = NULL;
    while (true)
    {
        // 从vi读取摄像头数据
        buffer = RK_MPI_SYS_GetMediaBuffer(RK_ID_VI, 0, -1);
        if (!buffer)
        {
            continue;
        }
        input_image.size = RK_MPI_MB_GetSize(buffer);
        input_image.data = (uint8_t *)RK_MPI_MB_GetPtr(buffer);
        Mat nv12_data(VI_HEIGHT + VI_HEIGHT / 2, VI_WIDTH, CV_8UC1, input_image.data);
        // 调用人脸检测接口
        rockx_ret = rockx_person_detect(object_det_handle, &input_image,
                                        &person_array, NULL);
        if (rockx_ret != ROCKX_RET_SUCCESS)
        {
            printf("rockx_person_detect error %d\n", rockx_ret);
            break;
        }
        // 处理检测结果
        // printf("Detected %d persons\n", person_array.count);
        Mat imgBGR;
        cvtColor(nv12_data, imgBGR, COLOR_YUV2BGR_NV12);
        for (int i = 0; i < person_array.count; i++)
        {
            rockx_object_t *obj = &person_array.object[i];
            printf("Person %d: box=(%d, %d, %d, %d), score=%f\n", i, obj->box.left, obj->box.top, obj->box.right, obj->box.bottom, obj->score);
            rectangle(imgBGR, Point(obj->box.left, obj->box.top),
                      Point(obj->box.right, obj->box.bottom), Scalar(0, 0, 255), 2);
        }
        Mat I420_with_border;
        cvtColor(imgBGR, I420_with_border, COLOR_BGR2YUV_I420);
        ConvertColor(I420_with_border.data, input_image.data);

        RK_MPI_SYS_SendMediaBuffer(RK_ID_VENC, 0, buffer);
        RK_MPI_MB_ReleaseBuffer(buffer);
    }
    rockx_image_release(&input_image);
    rockx_destroy(object_det_handle);
    return nullptr;
}

int main()
{
    int ret = 0;
    RK_MPI_SYS_Init();

    // VI输入视频捕获
    VI_CHN_ATTR_S vi_attr;
    memset(&vi_attr, 0, sizeof(vi_attr));
    vi_attr.pcVideoNode = "rkispp_scale0";
    vi_attr.u32Width = VI_WIDTH;
    vi_attr.u32Height = VI_HEIGHT;
    vi_attr.enPixFmt = IMAGE_TYPE_NV12;
    vi_attr.u32BufCnt = 3;
    vi_attr.enBufType = VI_CHN_BUF_TYPE_DMA;
    vi_attr.enWorkMode = VI_WORK_MODE_NORMAL;

    ret = RK_MPI_VI_SetChnAttr(0, 0, &vi_attr);
    ret |= RK_MPI_VI_EnableChn(0, 0);
    if (ret)
    {
        printf("Create VI[0] failed! ret=%d\n", ret);
        return -1;
    }

    printf("%s initial finish\n", __func__);
    // VENC：提供视频编码功能
    VENC_CHN_ATTR_S venc_chn_attr;
    memset(&venc_chn_attr, 0, sizeof(VENC_CHN_ATTR_S));
    // 编码器属性
    venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_H264; // 编码协议类型
    venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_NV12; // 输入图像类型
    venc_chn_attr.stVencAttr.u32VirWidth = VI_WIDTH;      // stride宽度，通常与buffer_width相同
    venc_chn_attr.stVencAttr.u32VirHeight = VI_HEIGHT;    // stride高度，通常与buffer_height相同
    venc_chn_attr.stVencAttr.u32Profile = 77;
    venc_chn_attr.stVencAttr.u32PicWidth = VI_WIDTH;
    venc_chn_attr.stVencAttr.u32PicHeight = VI_HEIGHT;
    // 码率控制器属性
    venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = 25;              // I帧间隔
    venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;  // 数据源帧率分子
    venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;   // 数据源帧率分母
    venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 30; // 目标帧率分子
    venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;  // 目标帧率分母
    venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = 7000000;     // 平均比特率
    // GOP属性省略
    ret = RK_MPI_VENC_CreateChn(0, &venc_chn_attr);
    if (ret != RK_SUCCESS)
    {
        printf("RK_MPI_VENC_CreateChn failed with error code %d\n", ret);
        return -1;
    }
    // rtsp初始化
    g_rtsplive = create_rtsp_demo(554);
    g_rtsp_session = rtsp_new_session(g_rtsplive, "/live/main_stream");
    rtsp_set_video(g_rtsp_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(g_rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime());

    pthread_t read_thread;
    pthread_create(&read_thread, NULL, set_rk_face_detection, NULL);

#if 0
    // 绑定后，数据源生成的数据将自动发送给接收者
    MPP_CHN_S stSrcChn;
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = 0;
    stSrcChn.s32ChnId = 0;
    MPP_CHN_S stDestChn;
    stDestChn.enModId = RK_ID_VENC;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = 0;
    ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (ret)
    {
        printf("ERROR: Bind VI[0] and VENC[0] error! ret=%d\n", ret);
        return 0;
    }
#endif
    ret = RK_MPI_VI_StartStream(0, 0);
    if (ret)
    {
        printf("Start VI[0] failed! ret=%d\n", ret);
        return -1;
    }
    // 注册回调，处理视频编码后的数据
    MPP_CHN_S stEncChn;
    stEncChn.enModId = RK_ID_VENC;
    stEncChn.s32DevId = 0;
    stEncChn.s32ChnId = 0;
    ret = RK_MPI_SYS_RegisterOutCb(&stEncChn, video_packet_cb);
    if (ret)
    {
        printf("ERROR: register output callback for VENC[0] error! ret=%d\n", ret);
        return 0;
    }
    while (1)
    {
        sleep(1);
    }
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VI_DisableChn(0, 0);
}

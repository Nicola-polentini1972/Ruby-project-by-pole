/*
    Ruby Licence
    Copyright (c) 2020-2025 Petru Soroaga petrusoroaga@yahoo.com
    All rights reserved.

    Redistribution and/or use in source and/or binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions and/or use of the source code (partially or complete) must retain
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Redistributions in binary form (partially or complete) must reproduce
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Copyright info and developer info must be preserved as is in the user
        interface, additions could be made to that info.
        * Neither the name of the organization nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.
        * Military use is not permitted.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR (PETRU SOROAGA) BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "mpp_core.h"
#include <sys/mman.h>
#include <stdio.h>
#include <time.h>

#define READ_VIDEO_BUF_SIZE (2*1024*1024) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179
#define MAX_VIDEO_FRAMES 128  // min 16 and 20+ recommended (mpp/readme.txt)
#define CODEC_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))

typedef struct
{
   int prime_fd;
   type_drm_buffer drmBufferInfo;
} type_mpp_frame_info;

shared_mem_process_stats* g_pSMProcessStats = NULL;
sem_t* g_pSemaphoreMPPDisplayFrameReadyWrite = NULL;
sem_t* g_pSemaphoreMPPDisplayFrameReadyRead = NULL;

MppCtx g_MPPCtx;
MppApi* g_pMPPApi = NULL;
MppBufferGroup g_MPPBufferGroup = NULL;
MppCodingType g_MPPDecodeType = MPP_VIDEO_CodingAVC;
int g_iMPPBuffersSize = MAX_VIDEO_FRAMES;
type_mpp_frame_info g_Frames[MAX_VIDEO_FRAMES];
uint8_t* g_pInputBuffer = NULL;
MppPacket g_MPPInputPacket;
u32 g_uTimeFirstFrame = 0;
u32 g_uTimeMPPPeriodicChecks = 0;
u32 g_uTimeLastHistogramUpdate = 0;
#define HISTOGRAM_UPDATE_INTERVAL_MS 300

bool g_bMPPFramesBuffersInitialised = false;
bool g_bMPPFrameEOS = false;
bool g_bMPPStreamChangedFlag = false;
bool g_bMPPEnableVSync = true;

u32 g_uMPPCPUAffinityMask = 0;
int g_iMPPCPUAffinityCoreIndex = -1;
int g_iMPPRawPriority = -1;

pthread_t g_MPPDecodeThread;
pthread_t g_MPPUpdateDisplayThread;
extern bool g_bQuit;
int g_iMPPFrameBufferIndexToDisplay = 0;
uint32_t g_uMPPDRMBufferIdToDisplay = 0;

int _mpp_send_command(MpiCmd command, RK_U32 value)
{
   RK_U32 res = g_pMPPApi->control(g_MPPCtx, command, &value);
   if ( res )
   {
      log_error_and_alarm("[MPP] Could not set MPP control command %d, value %d. Error: %d", command, value, (int)res);
      return -1;
   }
   return 0;
}


int mpp_feed_data_to_decoder(void* pData, int iLength)
{
    g_pSMProcessStats->uLoopCounter2++;
    mpp_packet_set_data(g_MPPInputPacket, pData);
    mpp_packet_set_size(g_MPPInputPacket, iLength);
    mpp_packet_set_pos(g_MPPInputPacket, pData);
    mpp_packet_set_length(g_MPPInputPacket, iLength);

    struct timespec spec;
    clock_gettime(RUBY_HW_CLOCK_ID, &spec);
    uint64_t tTime = spec.tv_sec * 1000 + spec.tv_nsec / 1e6;
    mpp_packet_set_pts(g_MPPInputPacket,(RK_S64) tTime);

    int iStallCount = 0;
    int iElapsedMs = 0;
    uint64_t tTimeStart = tTime;
    while ( (!g_bQuit) && (MPP_OK != g_pMPPApi->decode_put_packet(g_MPPCtx, g_MPPInputPacket)) )
    {
        iStallCount++;
        clock_gettime(RUBY_HW_CLOCK_ID, &spec);
        uint64_t tTimeNow = spec.tv_sec * 1000 + spec.tv_nsec / 1e6;
        iElapsedMs = (int)(tTimeNow - tTimeStart);
        if ( iElapsedMs > 100 )
        {
            log_softerror_and_alarm("[MPP] Failed to feed data to MPP decoder, stalled for %d ms, stall counter: %d", iElapsedMs, iStallCount);
            return iElapsedMs;
        }
        hardware_sleep_micros(1000);
    }
    if ( (iStallCount > 0) && (iElapsedMs > 5) )
       log_softerror_and_alarm("[MPP] Stalled feeding data for %d ms, stall count: %d", iElapsedMs, iStallCount);
    return iElapsedMs;
}

// --- RGB histogram sampling (feeds the OSD "RGB Histogram" plugin) ---------
// Reads a strided sample of pixels every HISTOGRAM_UPDATE_INTERVAL_MS (own
// throttle, independent of _mpp_core_periodic_checks() below) from the NV12
// buffer that is already decoded and about to be displayed, so it costs no
// extra decode/capture work. Frame buffers are mmap'd once, at init time, and
// reused for the lifetime of the buffer to avoid mmap/munmap syscalls on the
// realtime decode thread.

#define HISTOGRAM_NUM_BINS 64
#define HISTOGRAM_MAGIC 0x52474248u // 'RGBH', must match the OSD plugin
#define HISTOGRAM_SAMPLE_STRIDE 8   // sample every 8th pixel in each direction
#define HISTOGRAM_LIVE_FILE_PATH "/home/radxa/ruby/tmp/histogram_live.bin"

typedef struct
{
   unsigned int uMagic;
   unsigned int uVersion;
   unsigned int uNumBins;
   unsigned int uTimestampSec;
   unsigned int uHistR[HISTOGRAM_NUM_BINS];
   unsigned int uHistG[HISTOGRAM_NUM_BINS];
   unsigned int uHistB[HISTOGRAM_NUM_BINS];
} __attribute__((packed)) HistogramLiveData;

static void* s_pMappedFrameData[MAX_VIDEO_FRAMES];
static size_t s_uMappedFrameSize[MAX_VIDEO_FRAMES];
static int s_iHistFrameWidth = 0;
static int s_iHistFrameHeight = 0;
static int s_iHistStrideH = 0;
static int s_iHistStrideV = 0;

static inline void _histogram_yuv_to_rgb(int y, int u, int v, int* pR, int* pG, int* pB)
{
   int c = y - 16;
   int d = u - 128;
   int e = v - 128;

   int r = (298*c + 409*e + 128) >> 8;
   int g = (298*c - 100*d - 208*e + 128) >> 8;
   int b = (298*c + 516*d + 128) >> 8;

   *pR = r < 0 ? 0 : (r > 255 ? 255 : r);
   *pG = g < 0 ? 0 : (g > 255 ? 255 : g);
   *pB = b < 0 ? 0 : (b > 255 ? 255 : b);
}

static void _mpp_core_update_histogram()
{
   int idx = g_iMPPFrameBufferIndexToDisplay;
   if ( (idx < 0) || (idx >= g_iMPPBuffersSize) )
      return;
   if ( (NULL == s_pMappedFrameData[idx]) || (s_iHistFrameWidth <= 0) || (s_iHistFrameHeight <= 0) )
      return;

   const unsigned char* pY = (const unsigned char*)s_pMappedFrameData[idx];
   const unsigned char* pUV = pY + (size_t)s_iHistStrideH * (size_t)s_iHistStrideV;

   int nHist256R[256];
   int nHist256G[256];
   int nHist256B[256];
   memset(nHist256R, 0, sizeof(nHist256R));
   memset(nHist256G, 0, sizeof(nHist256G));
   memset(nHist256B, 0, sizeof(nHist256B));

   for( int y=0; y<s_iHistFrameHeight-1; y+=HISTOGRAM_SAMPLE_STRIDE )
   for( int x=0; x<s_iHistFrameWidth-1; x+=HISTOGRAM_SAMPLE_STRIDE )
   {
      int yy = pY[(size_t)y*s_iHistStrideH + x];
      int xUV = (x/2)*2;
      int yUV = y/2;
      int uu = pUV[(size_t)yUV*s_iHistStrideH + xUV];
      int vv = pUV[(size_t)yUV*s_iHistStrideH + xUV + 1];

      int r,g,b;
      _histogram_yuv_to_rgb(yy, uu, vv, &r, &g, &b);
      nHist256R[r]++; nHist256G[g]++; nHist256B[b]++;
   }

   HistogramLiveData data;
   memset(&data, 0, sizeof(data));
   data.uMagic = HISTOGRAM_MAGIC;
   data.uVersion = 1;
   data.uNumBins = HISTOGRAM_NUM_BINS;
   data.uTimestampSec = (unsigned int)time(NULL);

   int nSamplesPerBin = 256/HISTOGRAM_NUM_BINS;
   for( int bIdx=0; bIdx<HISTOGRAM_NUM_BINS; bIdx++ )
   {
      unsigned int sumR=0, sumG=0, sumB=0;
      for( int i=0; i<nSamplesPerBin; i++ )
      {
         sumR += nHist256R[bIdx*nSamplesPerBin+i];
         sumG += nHist256G[bIdx*nSamplesPerBin+i];
         sumB += nHist256B[bIdx*nSamplesPerBin+i];
      }
      data.uHistR[bIdx] = sumR;
      data.uHistG[bIdx] = sumG;
      data.uHistB[bIdx] = sumB;
   }

   char szTmp[300];
   snprintf(szTmp, sizeof(szTmp), "%s.tmp", HISTOGRAM_LIVE_FILE_PATH);
   FILE* fd = fopen(szTmp, "wb");
   if ( NULL == fd )
      return;
   fwrite(&data, sizeof(data), 1, fd);
   fclose(fd);
   rename(szTmp, HISTOGRAM_LIVE_FILE_PATH);
}

int _mpp_init_frames(MppFrame pFrame)
{
   log_line("[MPP] Init frames (%d frames)...", g_iMPPBuffersSize);
   u32 uTimeStart = get_current_timestamp_ms();

   int w = mpp_frame_get_width(pFrame);
   int h = mpp_frame_get_height(pFrame);
   RK_U32 stride_h = mpp_frame_get_hor_stride(pFrame);
   RK_U32 stride_v = mpp_frame_get_ver_stride(pFrame);
   MppFrameFormat fmt = mpp_frame_get_fmt(pFrame);

   if ( fmt == MPP_FMT_YUV420SP )
      log_line("[MPP] Received video format: YUV420SP");
   else if ( fmt == MPP_FMT_YUV420SP_10BIT )
      log_line("[MPP] Received video format: YUV420SP_10BIT");
   else
      log_line("[MPP] Received video format: Unknown");

   log_line("[MPP] Frame info changed to %dx%d, strides: %dx%d", w,h, stride_h, stride_v);

   s_iHistFrameWidth = w;
   s_iHistFrameHeight = h;
   s_iHistStrideH = (int)stride_h;
   s_iHistStrideV = (int)stride_v;

   for( int hi=0; hi<MAX_VIDEO_FRAMES; hi++ )
   {
      if ( NULL != s_pMappedFrameData[hi] )
         munmap(s_pMappedFrameData[hi], s_uMappedFrameSize[hi]);
      s_pMappedFrameData[hi] = NULL;
      s_uMappedFrameSize[hi] = 0;
   }

   int iRet = mpp_buffer_group_get_external(&g_MPPBufferGroup, MPP_BUFFER_TYPE_DRM);

   for (int i=0; i<g_iMPPBuffersSize; i++)
   {
      memset(&(g_Frames[i].drmBufferInfo), 0, sizeof(type_drm_buffer));
      struct drm_mode_create_dumb creq;
      struct drm_prime_handle dph;
 
      uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

      memset(&creq, 0, sizeof(creq));
      creq.width = stride_h;
      creq.height = 2*stride_v;
      creq.bpp = ((fmt==MPP_FMT_YUV420SP)?8:10);
      iRet = drmIoctl(ruby_drm_core_get_fd(), DRM_IOCTL_MODE_CREATE_DUMB, &creq);
      if ( iRet < 0 )
      {
         log_softerror_and_alarm("[MPP] Cannot create buffer (%d)", errno);
         return -errno;
      }
      g_Frames[i].drmBufferInfo.uWidth = creq.width;
      g_Frames[i].drmBufferInfo.uHeight = creq.height;
      g_Frames[i].drmBufferInfo.uStride = creq.pitch;
      g_Frames[i].drmBufferInfo.uSize = creq.size;
      g_Frames[i].drmBufferInfo.uHandle = creq.handle;

      // Commit DRM buffer to frame group
      memset(&dph, 0, sizeof(struct drm_prime_handle));
      dph.handle = g_Frames[i].drmBufferInfo.uHandle;
      dph.fd = -1;
      do
      {
         iRet = ioctl(ruby_drm_core_get_fd(), DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
      } while (iRet == -1 && (errno == EINTR || errno == EAGAIN));
  
      g_Frames[i].drmBufferInfo.uBufferId = dph.fd;
  
      MppBufferInfo info;
      memset(&info, 0, sizeof(info));
      info.type = MPP_BUFFER_TYPE_DRM;
      info.size = g_Frames[i].drmBufferInfo.uWidth * g_Frames[i].drmBufferInfo.uHeight;
      info.fd = dph.fd;
      iRet = mpp_buffer_commit(g_MPPBufferGroup, &info);
      g_Frames[i].prime_fd = info.fd;
      if (g_Frames[i].drmBufferInfo.uBufferId != (uint32_t)info.fd)
      {
         iRet = close(g_Frames[i].drmBufferInfo.uBufferId);
      }

      if ( (i < MAX_VIDEO_FRAMES) && (g_Frames[i].drmBufferInfo.uSize > 0) )
      {
         void* pMapped = mmap(NULL, g_Frames[i].drmBufferInfo.uSize, PROT_READ, MAP_SHARED, g_Frames[i].prime_fd, 0);
         if ( pMapped != MAP_FAILED )
         {
            s_pMappedFrameData[i] = pMapped;
            s_uMappedFrameSize[i] = g_Frames[i].drmBufferInfo.uSize;
         }
         else
            log_softerror_and_alarm("[MPP] Failed to mmap frame buffer %d for histogram sampling (%d).", i, errno);
      }

      log_line("[MPP] Allocated new frame");

     // Allocate DRM FB from DRM buffer
     memset(handles, 0, sizeof(handles));
     memset(pitches, 0, sizeof(pitches));
     memset(offsets, 0, sizeof(offsets));
     handles[0] = g_Frames[i].drmBufferInfo.uHandle;
     offsets[0] = 0;
     pitches[0] = stride_h;      
     handles[1] = g_Frames[i].drmBufferInfo.uHandle;
     offsets[1] = pitches[0] * stride_v;
     pitches[1] = pitches[0];
     drmModeAddFB2(ruby_drm_core_get_fd(), w, h, DRM_FORMAT_NV12, handles, pitches, offsets, &(g_Frames[i].drmBufferInfo.uBufferId), 0);
     
     log_line("[MPP] Allocated new (%d) DRM FB buffer: handle: %u, fb_id: %u",
         i, g_Frames[i].drmBufferInfo.uHandle, g_Frames[i].drmBufferInfo.uBufferId);
   }

   // Register external frame group
   g_pMPPApi->control(g_MPPCtx, MPP_DEC_SET_EXT_BUF_GROUP, g_MPPBufferGroup);
   g_pMPPApi->control(g_MPPCtx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
   
   ruby_drm_set_video_source_size(w, h);
   ruby_drm_core_set_plane_properties_and_buffer(g_Frames[0].drmBufferInfo.uBufferId);
   g_bMPPFramesBuffersInitialised = true;

   u32 uTimeDiff = get_current_timestamp_ms() - uTimeStart;
   
   log_line("[MPP] Init frames (%d frames) done (took %u ms)", g_iMPPBuffersSize, uTimeDiff);
   return 0;
}

void _mpp_core_periodic_checks()
{
   //log_line("[MPPThread periodic checks");
   //static uint64_t uCrtX = 0;
   //uCrtX += 20;
   //type_drm_object_info* pPlaneInfo = ruby_drm_get_plane_info();
   //ruby_drm_set_object_property(pPlaneInfo, "CRTC_X", uCrtX);
}

void* _mpp_thread_update_display(void *param)
{
   log_line("[MPPThreadUpdateDisplay] Started.");
   hw_log_current_thread_attributes("MPP display update");
   u32 uLastDRMBufferIdDisplayed = 0;
   u32 uNewDRMBufferIdToDisplay = 0;
   while ( (!g_bMPPFrameEOS) && (!g_bQuit) )
   {
      if ( -1 == g_iMPPFrameBufferIndexToDisplay )
         break;
      if ( NULL != g_pSemaphoreMPPDisplayFrameReadyRead )
      {
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         //ts.tv_nsec += 1000LL*(long long)10000; // 10 ms
         //if ( ts.tv_nsec > 999999999 )
         //{
         //   ts.tv_sec -= 999999999;
         //   ts.tv_sec++;
         //}
         ts.tv_sec++;
         int iRes = sem_timedwait(g_pSemaphoreMPPDisplayFrameReadyRead, &ts);
         if ( 0 != iRes )
         {
            if ( errno != ETIMEDOUT )
               log_softerror_and_alarm("[MPPThreadUpdateDisplay] Failed to timewait on semaphore. Error: %d, %s", errno, strerror(errno));
            continue;
         }
         uNewDRMBufferIdToDisplay = g_uMPPDRMBufferIdToDisplay;
         is_semaphore_signaled_clear_logok(g_pSemaphoreMPPDisplayFrameReadyRead, SEMAPHORE_MPP_DISPLAY_FRAME_READY, 0);
      }
      else
         uNewDRMBufferIdToDisplay = g_uMPPDRMBufferIdToDisplay;
      if ( -1 == g_iMPPFrameBufferIndexToDisplay )
         break;
      if ( uLastDRMBufferIdDisplayed == uNewDRMBufferIdToDisplay )
      {
         hardware_sleep_ms(1);
         continue;
      }

      g_pSMProcessStats->uLoopCounter4++;
      g_pSMProcessStats->lastIPCOutgoingTime = get_current_timestamp_ms();
      ruby_drm_core_set_plane_buffer(uNewDRMBufferIdToDisplay);
      uLastDRMBufferIdDisplayed = uNewDRMBufferIdToDisplay;
   }
   if ( g_bQuit )
      log_line("[MPPThreadUpdateDisplay] Ending render thread due to quit signal.");
   if ( g_bMPPFrameEOS )
      log_line("[MPPThreadUpdateDisplay] Ending render thread due to end of stream.");
   if ( -1 == g_iMPPFrameBufferIndexToDisplay )
      log_line("[MPPThreadUpdateDisplay] Ending render thread due signaled to end.");
   log_line("[MPPThreadUpdateDisplay] Finsihed.");
   return NULL;
}

void* _mpp_thread_frame_decode(void *param)
{
   log_line("[MPPThreadDecoder] Started.");
   hw_log_current_thread_attributes("MPP frame decode");
   MppFrame pFrame  = NULL;

   log_line("[MPPThreadDecoder] Start main thread loop...");
   while ( (!g_bMPPFrameEOS) && (!g_bQuit) ) 
   {
      g_pMPPApi->decode_get_frame(g_MPPCtx, &pFrame);
      if ( g_bQuit )
         break;
      if ( ! pFrame )
      {
         //log_softerror_and_alarm("[MPPThreadDecoder] Received invalid frame. Skipping it.");
         hardware_sleep_ms(1);
         continue;
      }
      // Frame with resolution update
      if ( mpp_frame_get_info_change(pFrame) )
      {
         log_line("[MPPThreadDecoder] Received new frame resolution update.");
         _mpp_init_frames(pFrame);
         g_bMPPStreamChangedFlag = true;
         g_bMPPFrameEOS = (mpp_frame_get_eos(pFrame))?true:false;
         mpp_frame_deinit(&pFrame);
         pFrame = NULL;
         continue;
      }

      if ( ! g_bMPPFramesBuffersInitialised )
      {
         log_softerror_and_alarm("[MPPThreadDecoder] Received a frame but MPP frame buffers are not initialised yet.");
         mpp_frame_deinit(&pFrame);
         pFrame = NULL;
         continue;
      }

      // Regular frame
      g_pSMProcessStats->uLoopCounter3++;
     
      if ( 0 == g_uTimeFirstFrame )
      {
         //log_line("[MPPThreadDecoder] Received first frame.");
         g_uTimeFirstFrame = get_current_timestamp_ms();
      }

      if ( g_bQuit )
         break;
      MppBuffer pBuffer = mpp_frame_get_buffer(pFrame);
      if ( pBuffer && (!g_bQuit) )
      {
         MppBufferInfo info;
         mpp_buffer_info_get(pBuffer, &info);
         int iPrimeIndex = -1;
         for (int i=0; i<g_iMPPBuffersSize; i++)
         {
            if ( ((uint32_t) g_Frames[i].prime_fd) == ((uint32_t) info.fd) )
            {
               iPrimeIndex = i;
               break;
            }
         }
         //static int s_iLastPrimeBufferIndex = -1;
         //if ( (iPrimeIndex != s_iLastPrimeBufferIndex+1) && (iPrimeIndex != 0) )
         //   log_line("[MPPThreadDecoder] Diff now index: %d, prev index: %d", iPrimeIndex, s_iLastPrimeBufferIndex);
         //log_line("[MPPThreadDecoder] Received a frame in primeId buffer index %d (max %d)", iPrimeIndex, g_iMPPBuffersSize);
         //s_iLastPrimeBufferIndex = iPrimeIndex;
         
         if ( (-1 != iPrimeIndex) && (! g_bQuit) )
         {
            //ruby_drm_core_set_plane_buffer(g_Frames[iPrimeIndex].drmBufferInfo.uBufferId);
            g_iMPPFrameBufferIndexToDisplay = iPrimeIndex;
            g_uMPPDRMBufferIdToDisplay = g_Frames[iPrimeIndex].drmBufferInfo.uBufferId;
            if ( (NULL == g_pSemaphoreMPPDisplayFrameReadyWrite) || (0 != sem_post(g_pSemaphoreMPPDisplayFrameReadyWrite)) )
               log_softerror_and_alarm("Failed to signal semaphore for display frame ready.");
            u32 uTimeNow = get_current_timestamp_ms();
            if ( uTimeNow > g_uTimeMPPPeriodicChecks + 1000 )
            {
               g_uTimeMPPPeriodicChecks = uTimeNow;
               _mpp_core_periodic_checks();
            }
            if ( uTimeNow > g_uTimeLastHistogramUpdate + HISTOGRAM_UPDATE_INTERVAL_MS )
            {
               g_uTimeLastHistogramUpdate = uTimeNow;
               _mpp_core_update_histogram();
            }
         }
      }
      
      g_bMPPFrameEOS = (mpp_frame_get_eos(pFrame))?true:false;
      mpp_frame_deinit(&pFrame);
      pFrame = NULL;
      if ( g_bQuit )
         break;
   }

   if ( g_bQuit )
      log_line("[MPPThreadDecoder] Ending decoding thread due to quit signal.");
   if ( g_bMPPFrameEOS )
      log_line("[MPPThreadDecoder] Ending decoding thread due to end of stream.");
   log_line("[MPPThread] Ended.");
   return NULL;
}

int mpp_start_decoding_thread()
{
   pthread_attr_t attr;
   int iRTPriority = -1;
   if ( (g_iMPPRawPriority > 1) && (g_iMPPRawPriority < 100) )
      iRTPriority = 100 - g_iMPPRawPriority;

   if ( (iRTPriority > 0) && (iRTPriority < 98) )
      iRTPriority++;
   if ( (iRTPriority >= 0) && (iRTPriority < 99) )
      hw_init_worker_thread_attrs(&attr, g_iMPPCPUAffinityCoreIndex, -1, SCHED_FIFO, iRTPriority, "mppdisplay");
   else
      hw_init_worker_thread_attrs(&attr, g_iMPPCPUAffinityCoreIndex, -1, SCHED_OTHER, 0, "mppdisplay");
   if ( 0 != pthread_create(&g_MPPUpdateDisplayThread, &attr, _mpp_thread_update_display, NULL) )
      log_error_and_alarm("[MPP] Failed to create frame display thread.");

   pthread_attr_destroy(&attr);

   if ( (iRTPriority > 0) && (iRTPriority < 98) )
      iRTPriority++;
   if ( (iRTPriority >= 0) && (iRTPriority < 99) )
      hw_init_worker_thread_attrs(&attr, g_iMPPCPUAffinityCoreIndex, -1, SCHED_FIFO, iRTPriority, "mppdisplay");
   else
      hw_init_worker_thread_attrs(&attr, g_iMPPCPUAffinityCoreIndex, -1, SCHED_OTHER, 0, "mppdisplay");

   if ( 0 != pthread_create(&g_MPPDecodeThread, &attr, _mpp_thread_frame_decode, NULL) )
      log_error_and_alarm("[MPP] Failed to create frame decoding thread.");

   pthread_attr_destroy(&attr);

   return 0;
}


int mpp_init(bool bUseH265Decoder, int iMPPBuffersSize, u32 uCPUAffinityMask, int iRawPriority)
{
   log_line("[MPP] Doing MPP Initialization (for codec %s, buffers size: %d, cpu affinity mask: %u, raw priority: %d)...", (bUseH265Decoder?"H265":"H264"), iMPPBuffersSize, uCPUAffinityMask, iRawPriority);
   g_uMPPCPUAffinityMask = uCPUAffinityMask;
   g_iMPPRawPriority = iRawPriority;
   g_iMPPCPUAffinityCoreIndex = -1;
   if ( g_uMPPCPUAffinityMask > 0 )
   {
      for( int i=0; i<8; i++ )
      {
         if ( g_uMPPCPUAffinityMask & (0x01 << i) )
         {
            g_iMPPCPUAffinityCoreIndex = i;
            break;
         }
      }
   }
   log_line("[MPP] Core affinity to core: %d", g_iMPPCPUAffinityCoreIndex);

   g_MPPDecodeType = MPP_VIDEO_CodingAVC;
   if ( bUseH265Decoder )
      g_MPPDecodeType = MPP_VIDEO_CodingHEVC;

   g_iMPPBuffersSize = iMPPBuffersSize;
   if ( (g_iMPPBuffersSize < 5) || (g_iMPPBuffersSize >= MAX_VIDEO_FRAMES) )
      g_iMPPBuffersSize = 32;
     
   int iRes = mpp_check_support_format(MPP_CTX_DEC, g_MPPDecodeType);
   if ( iRes != 0 )
   {
      log_error_and_alarm("[MPP] Video decoding type %s not supported (%d). Exit.", (bUseH265Decoder?"H265":"H264"), iRes);
      return -1;
   }

   log_line("[MPP] Done check for codec %s. Success.", (bUseH265Decoder?"H265":"H264"));

   g_bMPPStreamChangedFlag = false;
   g_pInputBuffer = (uint8_t*)malloc(READ_VIDEO_BUF_SIZE);
   if ( NULL == g_pInputBuffer )
   {
      log_error_and_alarm("[MPP] Can't allocate memory. Exit.");
      return -2;
   }

   if ( 0 != mpp_packet_init(&g_MPPInputPacket, g_pInputBuffer, READ_VIDEO_BUF_SIZE) )
   {
      log_error_and_alarm("[MPP] Can't init MPP input packet. Exit.");
      return -3;
   }

   if ( 0 != mpp_create(&g_MPPCtx, &g_pMPPApi) )
   {
      log_error_and_alarm("[MPP] Can't init MPP input packet. Exit.");
      return -3;
   }

   if ( 0 != mpp_init(g_MPPCtx, MPP_CTX_DEC, g_MPPDecodeType) )
   {
      log_error_and_alarm("[MPP] Can't init MPP library for decoding. Exit.");
      return -1;
   }

   if ( NULL != g_pSemaphoreMPPDisplayFrameReadyRead )
      sem_close(g_pSemaphoreMPPDisplayFrameReadyRead);
   if ( NULL != g_pSemaphoreMPPDisplayFrameReadyWrite )
      sem_close(g_pSemaphoreMPPDisplayFrameReadyWrite);
   
   sem_unlink(SEMAPHORE_MPP_DISPLAY_FRAME_READY);
   g_pSemaphoreMPPDisplayFrameReadyWrite = sem_open(SEMAPHORE_MPP_DISPLAY_FRAME_READY, O_CREAT | O_RDWR, S_IWUSR | S_IRUSR, 0);
   if ( (NULL == g_pSemaphoreMPPDisplayFrameReadyWrite) || (SEM_FAILED == g_pSemaphoreMPPDisplayFrameReadyWrite) )
   {
      log_error_and_alarm("[MPP] Failed to create write semaphore: %s, try alternative.", SEMAPHORE_MPP_DISPLAY_FRAME_READY);
      g_pSemaphoreMPPDisplayFrameReadyWrite = sem_open(SEMAPHORE_MPP_DISPLAY_FRAME_READY, O_CREAT, S_IWUSR | S_IRUSR, 0); 
      if ( (NULL == g_pSemaphoreMPPDisplayFrameReadyWrite) || (SEM_FAILED == g_pSemaphoreMPPDisplayFrameReadyWrite) )
      {
         log_error_and_alarm("[MPP] Failed to create write semaphore: %s", SEMAPHORE_MPP_DISPLAY_FRAME_READY);
         g_pSemaphoreMPPDisplayFrameReadyWrite = NULL;
         return -1;
      }
   }
   if ( (NULL != g_pSemaphoreMPPDisplayFrameReadyWrite) && (SEM_FAILED != g_pSemaphoreMPPDisplayFrameReadyWrite) )
      log_line("[MPP] Opened semaphore for signaling display frame ready: (%s)", SEMAPHORE_MPP_DISPLAY_FRAME_READY);

   g_pSemaphoreMPPDisplayFrameReadyRead = sem_open(SEMAPHORE_MPP_DISPLAY_FRAME_READY, O_RDWR);
   if ( (NULL == g_pSemaphoreMPPDisplayFrameReadyRead) || (SEM_FAILED == g_pSemaphoreMPPDisplayFrameReadyRead) )
   {
      log_error_and_alarm("[MPP] Failed to create read semaphore: %s, try alternative.", SEMAPHORE_MPP_DISPLAY_FRAME_READY);
      g_pSemaphoreMPPDisplayFrameReadyRead = sem_open(SEMAPHORE_MPP_DISPLAY_FRAME_READY, O_CREAT, S_IWUSR | S_IRUSR, 0); 
      if ( (NULL == g_pSemaphoreMPPDisplayFrameReadyRead) || (SEM_FAILED == g_pSemaphoreMPPDisplayFrameReadyRead) )
      {
         log_error_and_alarm("[MPP] Failed to create read semaphore: %s", SEMAPHORE_MPP_DISPLAY_FRAME_READY);
         g_pSemaphoreMPPDisplayFrameReadyRead = NULL;
         return -1;
      }
   }
   if ( (NULL != g_pSemaphoreMPPDisplayFrameReadyRead) && (SEM_FAILED != g_pSemaphoreMPPDisplayFrameReadyRead) )
      log_line("[MPP] Opened semaphore for checking display frame ready: (%s)", SEMAPHORE_MPP_DISPLAY_FRAME_READY);

   int iSemVal = 0;
   if ( 0 == sem_getvalue(g_pSemaphoreMPPDisplayFrameReadyRead, &iSemVal) )
      log_line("[MPP] Display frame ready semaphore initial value: %d", iSemVal);
   else
      log_softerror_and_alarm("[MPP] Failed to get display frame ready semaphore value.");

   // Set MPP configuration and params

   MppDecCfg pMPPConfig = NULL;
   mpp_dec_cfg_init(&pMPPConfig);

   iRes = g_pMPPApi->control(g_MPPCtx, MPP_DEC_GET_CFG, pMPPConfig);
   if ( iRes )
   {
      log_error_and_alarm("[MPP] Failed to get MPP decoder config. Returned error: %d", iRes);
      return -4;
   }

   RK_U32 split_video_input = 1;
   iRes = mpp_dec_cfg_set_u32(pMPPConfig, "base:split_parse", split_video_input);
   if ( iRes )
   {
      log_error_and_alarm("[MPP] Failed to modify MPP config to split input frames. Returned error: %d", iRes);
      return -5;
   }
   iRes = g_pMPPApi->control(g_MPPCtx, MPP_DEC_SET_CFG, pMPPConfig);
   if ( iRes )
   {
      log_error_and_alarm("[MPP] Failed to set MPP config to split input frames. Returned error: %d", iRes);
      return -6;
   }

   _mpp_send_command(MPP_DEC_SET_PARSER_SPLIT_MODE, 0xffff);
   _mpp_send_command(MPP_DEC_SET_DISABLE_ERROR, 0xffff);
   _mpp_send_command(MPP_DEC_SET_IMMEDIATE_OUT, 0xffff);
   _mpp_send_command(MPP_DEC_SET_ENABLE_FAST_PLAY, 0xffff);
   _mpp_send_command(MPP_SET_OUTPUT_BLOCK, MPP_POLL_BLOCK);

   // Use faster parallel hardware decoding? false for now
   int iFastDec = 1;
   _mpp_send_command(MPP_DEC_SET_PARSER_FAST_MODE, iFastDec);

   log_line("[MPP] Done MPP Initialization.");
   return 0;
}

int mpp_uninit()
{
   log_line("[MPP] Doing MPP Un-initialization...");

   g_iMPPFrameBufferIndexToDisplay = -1;
   if ( (NULL == g_pSemaphoreMPPDisplayFrameReadyWrite) || (0 != sem_post(g_pSemaphoreMPPDisplayFrameReadyWrite)) )
      log_softerror_and_alarm("Failed to signal semaphore for display frame to quit.");
  
   g_pMPPApi->reset(g_MPPCtx);
   if ( g_MPPBufferGroup )
   {
      mpp_buffer_group_put(g_MPPBufferGroup);
      g_MPPBufferGroup = NULL;
      for (int i=0; i<g_iMPPBuffersSize; i++)
      {
         //drmModeRmFB(drm_fd, mpi.frame_to_drm[i].fb_id);
         //struct drm_mode_destroy_dumb dmdd;
         //memset(&dmdd, 0, sizeof(dmdd));
         //dmdd.handle = mpi.frame_to_drm[i].handle;
         //do {
         //ret = ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
         //} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
      }
   }
  
   mpp_packet_deinit(&g_MPPInputPacket);
   mpp_destroy(g_MPPCtx);
   free(g_pInputBuffer);

   if ( NULL != g_pSemaphoreMPPDisplayFrameReadyRead )
      sem_close(g_pSemaphoreMPPDisplayFrameReadyRead);
   if ( NULL != g_pSemaphoreMPPDisplayFrameReadyWrite )
      sem_close(g_pSemaphoreMPPDisplayFrameReadyWrite);

   g_pSemaphoreMPPDisplayFrameReadyRead = NULL;
   g_pSemaphoreMPPDisplayFrameReadyWrite = NULL;
   g_bMPPFramesBuffersInitialised = false;
   g_uTimeFirstFrame = 0;
   log_line("[MPP] Done MPP Un-initialization.");
   return 0;
}

void mpp_enable_vsync(bool bEnableVSync)
{
   g_bMPPEnableVSync = bEnableVSync;
   ruby_drm_enable_vsync(g_bMPPEnableVSync?1:0);
}


int mpp_mark_end_of_stream()
{
   log_line("[MPP] Marking end of stream...");
   g_bMPPFrameEOS = true;
   mpp_packet_set_eos(g_MPPInputPacket);
   mpp_packet_set_length(g_MPPInputPacket, 0);
   while ( MPP_OK != g_pMPPApi->decode_put_packet(g_MPPCtx, g_MPPInputPacket) )
   {
      hardware_sleep_micros(10000);
   }
   g_bMPPFrameEOS = true;
   log_line("[MPP] Marked end of stream.");
   return 0;
}

bool mpp_get_clear_stream_changed_flag()
{
   bool bRet = g_bMPPStreamChangedFlag;
   g_bMPPStreamChangedFlag = false;
   return bRet;
}
// Licence: you can copy, edit, change or do whatever you wish with the code in this file
//
// RGB Histogram OSD plugin.
//
// The OSD render engine (RenderEngineUI) only exposes vector drawing calls and has
// no API to read back live video pixels, so this plugin can't sample the video feed
// by itself. It gets histogram data from two possible sources, in priority order:
//
//  1. Live data: HISTOGRAM_LIVE_FILE_PATH, a small binary file written every
//     HISTOGRAM_UPDATE_INTERVAL_MS by the controller's own video decoder
//     (code/r_player/mpp_core.cpp), which samples the already-decoded NV12
//     frame directly. Used only while "fresh" (updated in the last
//     HISTOGRAM_LIVE_STALE_SECONDS).
//  2. Static image: HISTOGRAM_IMAGE_PATH, a 24 bit binary PPM (P6) file, useful
//     for testing without the core plugin / radio link.
//
// If neither is available, a synthetic test histogram is shown instead so the
// plugin still renders something useful.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#include "../public/render_engine_ui.h"
#include "../public/telemetry_info.h"
#include "../public/settings_info.h"

// Defaults match a Radxa ground station layout; for a Raspberry Pi controller
// change "/home/radxa/ruby/" to "/home/pi/ruby/" in both paths below.
#define HISTOGRAM_IMAGE_PATH "/home/radxa/ruby/media/histogram_source.ppm"
#define HISTOGRAM_LIVE_FILE_PATH "/home/radxa/ruby/tmp/histogram_live.bin"
#define HISTOGRAM_LIVE_STALE_SECONDS 5
#define HISTOGRAM_RECHECK_INTERVAL_MS 300
#define HISTOGRAM_MAX_BINS 64
#define HISTOGRAM_LIVE_MAGIC 0x52474248u // 'RGBH', must match the core plugin

typedef struct
{
   unsigned int uMagic;
   unsigned int uVersion;
   unsigned int uNumBins;
   unsigned int uTimestampSec;
   unsigned int uHistR[HISTOGRAM_MAX_BINS];
   unsigned int uHistG[HISTOGRAM_MAX_BINS];
   unsigned int uHistB[HISTOGRAM_MAX_BINS];
} __attribute__((packed)) HistogramLiveData;

PLUGIN_VAR RenderEngineUI* g_pEngine = NULL;
PLUGIN_VAR const char* g_szPluginName = "RGB Histogram";
PLUGIN_VAR const char* g_szUID = "9K3F2A-RGBHIST-01Q7-RUBYX"; // must be unique among plugins

PLUGIN_VAR const char* g_szOptionBins = "Bins";
PLUGIN_VAR const char* g_szOptionLog = "Logarithmic scale";
PLUGIN_VAR const char* g_szBinsOption0 = "16";
PLUGIN_VAR const char* g_szBinsOption1 = "32";
PLUGIN_VAR const char* g_szBinsOption2 = "64";

static int s_nHistR[256];
static int s_nHistG[256];
static int s_nHistB[256];
static bool s_bHaveRealHistogram = false;
static bool s_bUsingLiveData = false;
static long s_lLastCheckTimeMs = 0;
static time_t s_tLastFileMTime = 0;

static long _get_time_ms()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

#ifdef __cplusplus
extern "C" {
#endif

static void _generate_fallback_histogram()
{
   // Synthetic bell-shaped distributions per channel, used when no source image is available.
   for( int i=0; i<256; i++ )
   {
      double dr = (i-150.0)/38.0;
      double dg = (i-115.0)/48.0;
      double db = (i-95.0)/34.0;
      s_nHistR[i] = (int)(2000.0*exp(-0.5*dr*dr));
      s_nHistG[i] = (int)(2200.0*exp(-0.5*dg*dg));
      s_nHistB[i] = (int)(1800.0*exp(-0.5*db*db));
   }
}

static int _ppm_read_int(FILE* fd)
{
   int c = fgetc(fd);
   while ( (c == '#') || isspace(c) )
   {
      if ( c == '#' )
      {
         while ( (c != '\n') && (c != EOF) )
            c = fgetc(fd);
      }
      c = fgetc(fd);
   }
   int nValue = 0;
   bool bHasDigits = false;
   while ( (c >= '0') && (c <= '9') )
   {
      nValue = nValue*10 + (c - '0');
      bHasDigits = true;
      c = fgetc(fd);
   }
   if ( ! bHasDigits )
      return -1;
   return nValue;
}

static bool _load_ppm_histogram(const char* szFile)
{
   FILE* fd = fopen(szFile, "rb");
   if ( NULL == fd )
      return false;

   int cMagic1 = fgetc(fd);
   int cMagic2 = fgetc(fd);
   if ( (cMagic1 != 'P') || (cMagic2 != '6') )
   {
      fclose(fd);
      return false;
   }

   int nWidth = _ppm_read_int(fd);
   int nHeight = _ppm_read_int(fd);
   int nMaxVal = _ppm_read_int(fd);
   if ( (nWidth <= 0) || (nHeight <= 0) || (nMaxVal <= 0) || (nMaxVal > 255) )
   {
      fclose(fd);
      return false;
   }
   fgetc(fd); // single whitespace byte separating header from binary pixel data

   int nTmpR[256];
   int nTmpG[256];
   int nTmpB[256];
   memset(nTmpR, 0, sizeof(nTmpR));
   memset(nTmpG, 0, sizeof(nTmpG));
   memset(nTmpB, 0, sizeof(nTmpB));

   long nPixels = (long)nWidth * (long)nHeight;
   unsigned char uPixel[3];
   long nReadCount = 0;
   for( long i=0; i<nPixels; i++ )
   {
      if ( 3 != fread(uPixel, 1, 3, fd) )
         break;
      nTmpR[uPixel[0]]++;
      nTmpG[uPixel[1]]++;
      nTmpB[uPixel[2]]++;
      nReadCount++;
   }
   fclose(fd);

   if ( nReadCount <= 0 )
      return false;

   memcpy(s_nHistR, nTmpR, sizeof(s_nHistR));
   memcpy(s_nHistG, nTmpG, sizeof(s_nHistG));
   memcpy(s_nHistB, nTmpB, sizeof(s_nHistB));
   return true;
}

static bool _try_load_live_histogram()
{
   struct stat st;
   if ( 0 != stat(HISTOGRAM_LIVE_FILE_PATH, &st) )
      return false;

   time_t tNow = time(NULL);
   if ( (tNow - st.st_mtime) > HISTOGRAM_LIVE_STALE_SECONDS )
      return false; // core plugin stopped updating it (link down, vehicle disarmed, etc)

   FILE* fd = fopen(HISTOGRAM_LIVE_FILE_PATH, "rb");
   if ( NULL == fd )
      return false;

   HistogramLiveData data;
   size_t nRead = fread(&data, 1, sizeof(data), fd);
   fclose(fd);

   if ( (nRead != sizeof(data)) || (data.uMagic != HISTOGRAM_LIVE_MAGIC) || (data.uNumBins != HISTOGRAM_MAX_BINS) )
      return false;

   // Spread the 64 live bins back across the internal 256 slot arrays (used by both
   // sources) so render() can re-bin them into whatever display bin count the user picked.
   int nSpread = 256/HISTOGRAM_MAX_BINS;
   for( int i=0; i<256; i++ )
   {
      int bin = i/nSpread;
      s_nHistR[i] = data.uHistR[bin]/nSpread;
      s_nHistG[i] = data.uHistG[bin]/nSpread;
      s_nHistB[i] = data.uHistB[bin]/nSpread;
   }
   return true;
}

static void _maybe_reload_histogram()
{
   long lNowMs = _get_time_ms();
   if ( (lNowMs - s_lLastCheckTimeMs) < HISTOGRAM_RECHECK_INTERVAL_MS )
      return;
   s_lLastCheckTimeMs = lNowMs;

   if ( _try_load_live_histogram() )
   {
      s_bUsingLiveData = true;
      s_bHaveRealHistogram = true;
      return;
   }
   s_bUsingLiveData = false;

   struct stat st;
   if ( 0 != stat(HISTOGRAM_IMAGE_PATH, &st) )
   {
      if ( ! s_bHaveRealHistogram )
         _generate_fallback_histogram();
      return;
   }

   if ( s_bHaveRealHistogram && (st.st_mtime == s_tLastFileMTime) )
      return;

   if ( _load_ppm_histogram(HISTOGRAM_IMAGE_PATH) )
   {
      s_bHaveRealHistogram = true;
      s_tLastFileMTime = st.st_mtime;
   }
   else if ( ! s_bHaveRealHistogram )
      _generate_fallback_histogram();
}

void init(void* pEngine)
{
   g_pEngine = (RenderEngineUI*)pEngine;
   _generate_fallback_histogram();
   s_bHaveRealHistogram = false;
   s_bUsingLiveData = false;
   s_lLastCheckTimeMs = 0;
   s_tLastFileMTime = 0;
}

char* getName()
{
   return (char*)g_szPluginName;
}

int getVersion()
{
   return 1;
}

char* getUID()
{
   return (char*)g_szUID;
}

int getPluginSettingsCount()
{
   return 2;
}

char* getPluginSettingName(int settingIndex)
{
   if ( 0 == settingIndex )
      return (char*)g_szOptionBins;
   if ( 1 == settingIndex )
      return (char*)g_szOptionLog;
   return NULL;
}

int getPluginSettingType(int settingIndex)
{
   if ( 0 == settingIndex )
      return PLUGIN_SETTING_TYPE_ENUM;
   return PLUGIN_SETTING_TYPE_BOOL;
}

int getPluginSettingMinValue(int settingIndex)
{
   return 0;
}

int getPluginSettingMaxValue(int settingIndex)
{
   if ( 0 == settingIndex )
      return 2;
   return 1;
}

int getPluginSettingDefaultValue(int settingIndex)
{
   if ( 0 == settingIndex )
      return 2; // 64 bins
   return 1; // logarithmic scale on
}

int getPluginSettingOptionsCount(int settingIndex)
{
   if ( 0 == settingIndex )
      return 3;
   return 0;
}

char* getPluginSettingOptionName(int settingIndex, int optionIndex)
{
   if ( 0 == settingIndex )
   {
      if ( 0 == optionIndex ) return (char*)g_szBinsOption0;
      if ( 1 == optionIndex ) return (char*)g_szBinsOption1;
      if ( 2 == optionIndex ) return (char*)g_szBinsOption2;
   }
   return NULL;
}

float getDefaultWidth()
{
   if ( NULL == g_pEngine )
      return 0.32;
   return 0.32/g_pEngine->getAspectRatio();
}

float getDefaultHeight()
{
   return 0.16;
}

void render(vehicle_and_telemetry_info_t* pTelemetryInfo, plugin_settings_info_t2* pCurrentSettings, float xPos, float yPos, float fWidth, float fHeight)
{
   if ( (NULL == g_pEngine) || (NULL == pCurrentSettings) )
      return;

   _maybe_reload_histogram();

   int nBinsOptions[3] = {16,32,64};
   int nBinsIndex = pCurrentSettings->nSettingsValues[0];
   if ( (nBinsIndex < 0) || (nBinsIndex > 2) )
      nBinsIndex = 2;
   int nBins = nBinsOptions[nBinsIndex];
   bool bLogScale = (0 != pCurrentSettings->nSettingsValues[1]);

   float fBackgroundAlpha = pCurrentSettings->fBackgroundAlpha;

   // Background panel

   g_pEngine->setStroke(0,0,0,fBackgroundAlpha);
   g_pEngine->setFill(0,0,0,fBackgroundAlpha);
   g_pEngine->setStrokeSize(1.0);
   g_pEngine->drawRoundRect(xPos, yPos, fWidth, fHeight, 0.02);

   // Legend (top area of the widget)

   u32 fontId = g_pEngine->getFontIdSmall();
   float fTextH = g_pEngine->textHeight(fontId);

   float fLegendY = yPos + fHeight*0.04;
   float fLegendBoxSize = fTextH*0.7;
   float fLegendX = xPos + fWidth*0.04;

   const char* szLabels[3] = {"R","G","B"};
   double dColors[3][4] = {
      {230,60,60,0.9},
      {60,220,90,0.9},
      {70,130,255,0.9}
   };
   for( int c=0; c<3; c++ )
   {
      g_pEngine->setFill(dColors[c][0], dColors[c][1], dColors[c][2], dColors[c][3]);
      g_pEngine->setStroke(0,0,0,0);
      g_pEngine->setStrokeSize(0.0);
      g_pEngine->drawRect(fLegendX, fLegendY, fLegendBoxSize, fLegendBoxSize);
      g_pEngine->setColors(g_pEngine->getColorOSDText());
      g_pEngine->drawText(fLegendX + fLegendBoxSize*1.4, fLegendY - fTextH*0.15, fontId, szLabels[c]);
      fLegendX += fLegendBoxSize*1.4 + g_pEngine->textWidth(fontId, szLabels[c]) + fWidth*0.03;
   }

   if ( s_bUsingLiveData )
   {
      const char* szLive = "LIVE";
      float wLive = g_pEngine->textWidth(fontId, szLive);
      double dLiveColor[4] = {80,240,120,0.95};
      g_pEngine->setColors(dLiveColor);
      g_pEngine->drawText(xPos + fWidth - fWidth*0.04 - wLive, fLegendY - fTextH*0.15, fontId, szLive);
   }

   // Plot area (below the legend)

   float fPlotX = xPos + fWidth*0.04;
   float fPlotY = yPos + fHeight*0.20;
   float fPlotW = fWidth*0.92;
   float fPlotH = fHeight*0.72;
   float fBaseline = fPlotY + fPlotH;

   // Bin the 256 channel values down to nBins display bars and find the peak for scaling

   int nSamplesPerBin = 256/nBins;
   float fBinR[HISTOGRAM_MAX_BINS];
   float fBinG[HISTOGRAM_MAX_BINS];
   float fBinB[HISTOGRAM_MAX_BINS];
   float fMaxVal = 0.0001;

   for( int b=0; b<nBins; b++ )
   {
      int nSumR = 0, nSumG = 0, nSumB = 0;
      for( int i=0; i<nSamplesPerBin; i++ )
      {
         int idx = b*nSamplesPerBin + i;
         nSumR += s_nHistR[idx];
         nSumG += s_nHistG[idx];
         nSumB += s_nHistB[idx];
      }
      float vR = bLogScale ? log(1.0+(double)nSumR) : (float)nSumR;
      float vG = bLogScale ? log(1.0+(double)nSumG) : (float)nSumG;
      float vB = bLogScale ? log(1.0+(double)nSumB) : (float)nSumB;
      fBinR[b] = vR;
      fBinG[b] = vG;
      fBinB[b] = vB;
      if ( vR > fMaxVal ) fMaxVal = vR;
      if ( vG > fMaxVal ) fMaxVal = vG;
      if ( vB > fMaxVal ) fMaxVal = vB;
   }

   float fBinW = fPlotW/(float)nBins;

   g_pEngine->setStrokeSize(0.0);
   g_pEngine->setStroke(0,0,0,0);

   for( int b=0; b<nBins; b++ )
   {
      float x = fPlotX + b*fBinW;

      float hR = fPlotH * (fBinR[b]/fMaxVal);
      float hG = fPlotH * (fBinG[b]/fMaxVal);
      float hB = fPlotH * (fBinB[b]/fMaxVal);

      g_pEngine->setFill(dColors[0][0], dColors[0][1], dColors[0][2], 0.55);
      g_pEngine->drawRect(x, fBaseline-hR, fBinW, hR);

      g_pEngine->setFill(dColors[1][0], dColors[1][1], dColors[1][2], 0.55);
      g_pEngine->drawRect(x, fBaseline-hG, fBinW, hG);

      g_pEngine->setFill(dColors[2][0], dColors[2][1], dColors[2][2], 0.55);
      g_pEngine->drawRect(x, fBaseline-hB, fBinW, hB);
   }

   // Baseline

   g_pEngine->setColors(g_pEngine->getColorOSDOutline());
   g_pEngine->setStrokeSize(1.0);
   g_pEngine->drawLine(fPlotX, fBaseline, fPlotX+fPlotW, fBaseline);

   g_pEngine->setColors(g_pEngine->getColorOSDText());
}

#ifdef __cplusplus
}
#endif

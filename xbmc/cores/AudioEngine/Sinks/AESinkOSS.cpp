/*
 *      Copyright (C) 2005-2010 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "AESinkOSS.h"
#include <stdint.h>
#include <limits.h>

#include "Utils/AEUtil.h"
#include "utils/StdString.h"
#include "utils/log.h"
#include "threads/SingleLock.h"

#include <sys/ioctl.h>

#if defined(OSS4) || defined(__FreeBSD__)
  #include <sys/soundcard.h>
#else
  #include <linux/soundcard.h>
#endif

#define OSS_FRAMES 256

static enum AEChannel OSSChannelMap[7] =
  {AE_CH_FL, AE_CH_FR, AE_CH_BL, AE_CH_BR, AE_CH_FC, AE_CH_LFE, AE_CH_NULL};

CAESinkOSS::CAESinkOSS() :
  m_channelLayout(NULL)
{
}

CAESinkOSS::~CAESinkOSS()
{
}

CStdString CAESinkOSS::GetDeviceUse(AEAudioFormat format, CStdString device)
{
#ifdef OSS4
  if (format.m_dataFormat == AE_FMT_RAW)
  {
    if (device.find_first_of('/') != 0)
      return "/dev/dsp_ac3";
    return device;
  }
 
  if (device.find_first_of('/') != 0)
    return "/dev/dsp_multich";
#else
  if (device.find_first_of('/') != 0)
    return "/dev/dsp";
#endif

  return device;
}

bool CAESinkOSS::Initialize(AEAudioFormat &format, CStdString &device)
{
  m_initFormat = format;
  device = GetDeviceUse(format, device);

  /* try to open in exclusive mode first (no software mixing) */
  m_fd = open(device.c_str(), O_WRONLY | O_EXCL, 0);
  if (!m_fd)
    m_fd = open(device.c_str(), O_WRONLY, 0);
  if (!m_fd)
  {
    CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to open the audio device: %s", device.c_str());
    return false;
  }

  int format_mask;
  if (ioctl(m_fd, SNDCTL_DSP_GETFMTS, &format_mask) == -1)
  {
    close(m_fd);
    CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to get supported formats, assuming AFMT_S16_NE");
    return false;
  }

#ifdef OSS4
  bool useCooked = true;
#endif

  int oss_fmt = 0;
#ifdef OSS4
       if ((format.m_dataFormat == AE_FMT_FLOAT) && (format_mask & AFMT_FLOAT )) oss_fmt = AFMT_FLOAT;
  else if ((format.m_dataFormat == AE_FMT_S32NE) && (format_mask & AFMT_S32_NE)) oss_fmt = AFMT_S32_NE;
  else if ((format.m_dataFormat == AE_FMT_S32BE) && (format_mask & AFMT_S32_BE)) oss_fmt = AFMT_S32_BE;
  else if ((format.m_dataFormat == AE_FMT_S32LE) && (format_mask & AFMT_S32_LE)) oss_fmt = AFMT_S32_LE;
  else if ((format.m_dataFormat == AE_FMT_S24NE) && (format_mask & AFMT_S24_NE)) oss_fmt = AFMT_S24_NE;
  else if ((format.m_dataFormat == AE_FMT_S24BE) && (format_mask & AFMT_S24_BE)) oss_fmt = AFMT_S24_BE;
  else if ((format.m_dataFormat == AE_FMT_S24LE) && (format_mask & AFMT_S24_LE)) oss_fmt = AFMT_S24_LE; else
#endif
       if ((format.m_dataFormat == AE_FMT_S16NE) && (format_mask & AFMT_S16_NE)) oss_fmt = AFMT_S16_NE;
  else if ((format.m_dataFormat == AE_FMT_S16BE) && (format_mask & AFMT_S16_BE)) oss_fmt = AFMT_S16_BE;
  else if ((format.m_dataFormat == AE_FMT_S16LE) && (format_mask & AFMT_S16_LE)) oss_fmt = AFMT_S16_LE;
  else if ((format.m_dataFormat == AE_FMT_S8   ) && (format_mask & AFMT_S8    )) oss_fmt = AFMT_S8;
  else if ((format.m_dataFormat == AE_FMT_U8   ) && (format_mask & AFMT_U8    )) oss_fmt = AFMT_U8;
  else if ((AE_IS_RAW(format.m_dataFormat)     ) && (format_mask & AFMT_AC3   )) oss_fmt = AFMT_AC3;
  else if (AE_IS_RAW(format.m_dataFormat))
  {
    close(m_fd);
    CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to find a suitable RAW output format");
    return false; 
  }
  else
  {
    CLog::Log(LOGINFO, "CAESinkOSS::Initialize - Your hardware does not support %s, trying other formats", CAEUtil::DataFormatToStr(format.m_dataFormat));

    /* fallback to the best supported format */
#ifdef OSS4
         if (format_mask & AFMT_FLOAT ) {oss_fmt = AFMT_FLOAT ; format.m_dataFormat = AE_FMT_FLOAT; }
    else if (format_mask & AFMT_S32_NE) {oss_fmt = AFMT_S32_NE; format.m_dataFormat = AE_FMT_S32NE; }
    else if (format_mask & AFMT_S32_BE) {oss_fmt = AFMT_S32_BE; format.m_dataFormat = AE_FMT_S32BE; }
    else if (format_mask & AFMT_S32_LE) {oss_fmt = AFMT_S32_LE; format.m_dataFormat = AE_FMT_S32LE; }
    else if (format_mask & AFMT_S24_NE) {oss_fmt = AFMT_S24_NE; format.m_dataFormat = AE_FMT_S24NE; }
    else if (format_mask & AFMT_S24_BE) {oss_fmt = AFMT_S24_BE; format.m_dataFormat = AE_FMT_S24BE; }
    else if (format_mask & AFMT_S24_LE) {oss_fmt = AFMT_S24_LE; format.m_dataFormat = AE_FMT_S24LE; } else
#endif
         if (format_mask & AFMT_S16_NE) {oss_fmt = AFMT_S16_NE; format.m_dataFormat = AE_FMT_S16NE; }
    else if (format_mask & AFMT_S16_BE) {oss_fmt = AFMT_S16_BE; format.m_dataFormat = AE_FMT_S16BE; }
    else if (format_mask & AFMT_S16_LE) {oss_fmt = AFMT_S16_LE; format.m_dataFormat = AE_FMT_S16LE; }
    else if (format_mask & AFMT_S8    ) {oss_fmt = AFMT_S8;     format.m_dataFormat = AE_FMT_S8; }
    else if (format_mask & AFMT_U8    ) {oss_fmt = AFMT_U8;     format.m_dataFormat = AE_FMT_U8; }
    else
    {
      CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to find a suitable native output format, will try to use AE_FMT_S16NE anyway");
      oss_fmt             = AFMT_S16_NE;
      format.m_dataFormat = AE_FMT_S16NE;
#ifdef OSS4
      /* dont use cooked if we did not find a native format, OSS might be able to convert */
      useCooked           = false;
#endif
    }
  }

#ifdef OSS4
  if (useCooked)
  {
    int oss_cooked = 1;
    if (ioctl(m_fd, SNDCTL_DSP_COOKEDMODE, &oss_cooked) == -1)
      CLog::Log(LOGWARNING, "CAESinkOSS::Initialize - Failed to set cooked mode");
  }
#endif

  if (ioctl(m_fd, SNDCTL_DSP_SETFMT, &oss_fmt) == -1)
  {
    close(m_fd);
    CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to set the data format (%s)", CAEUtil::DataFormatToStr(format.m_dataFormat));
    return false;
  }

#ifndef OSS4
  int mask = 0;
  for(unsigned int i = 0; format.m_channelLayout.Count(); ++i)
    switch(format.m_channelLayout[i])
    {
      case AE_CH_FL:
      case AE_CH_FR:
        mask |= DSP_BIND_FRONT;
        break;

      case AE_CH_BL:
      case AE_CH_BR:
        mask |= DSP_BIND_SURR;
        break;

      case AE_CH_FC:
      case AE_CH_LFE:
        mask |= DSP_BIND_CENTER_LFE;
        break;

      default:
        break;
    }

#ifndef __FreeBSD__
  /* try to set the channel mask, not all cards support this */
  if (ioctl(m_fd, SNDCTL_DSP_BIND_CHANNEL, &mask) == -1)
  {
    CLog::Log(LOGWARNING, "CAESinkOSS::Initialize - Failed to set the channel mask");
    /* get the configured channel mask */
    if (ioctl(m_fd, SNDCTL_DSP_GETCHANNELMASK, &mask) == -1)
    {
      /* as not all cards support this so we just assume stereo if it fails */
      CLog::Log(LOGWARNING, "CAESinkOSS::Initialize - Failed to get the channel mask, assuming stereo");
      mask = DSP_BIND_FRONT;
    }
  }
#endif

  /* fix the channel count */
  format.m_channelLayout.Reset();
  if (mask & DSP_BIND_FRONT)
  {
    format.m_channelLayout += AE_CH_FL;
    format.m_channelLayout += AE_CH_FR;
  }
  
  if (mask & DSP_BIND_SURR)
  {
    format.m_channelLayout += AE_CH_BL;
    format.m_channelLayout += AE_CH_BR;
  }
  
  if (mask & DSP_BIND_CENTER_LFE)
  {
    format.m_channelLayout += AE_CH_FC;
    format.m_channelLayout += AE_CH_LFE;
  }
  
#else /* OSS4 */
  unsigned long long order = 0;
  for(unsigned int i = 0; format.m_channelLayout[i] != AE_CH_NULL; ++i)
    switch(format.m_channelLayout[i])
    {
      case AE_CH_FL : order = (order << 4) | CHID_L  ; break;
      case AE_CH_FR : order = (order << 4) | CHID_R  ; break;
      case AE_CH_FC : order = (order << 4) | CHID_C  ; break;
      case AE_CH_LFE: order = (order << 4) | CHID_LFE; break;
      case AE_CH_SL : order = (order << 4) | CHID_LS ; break;
      case AE_CH_SR : order = (order << 4) | CHID_RS ; break;
      case AE_CH_BL : order = (order << 4) | CHID_LR ; break;
      case AE_CH_BR : order = (order << 4) | CHID_RR ; break;

      default:
        continue;
    }

  if (ioctl(m_fd, SNDCTL_DSP_SET_CHNORDER, &order) == -1)
  {
    if (ioctl(m_fd, SNDCTL_DSP_GET_CHNORDER, &order) == -1)
    {
      CLog::Log(LOGWARNING, "CAESinkOSS::Initialize - Failed to get the channel order, assuming CHNORDER_NORMAL");
      order = CHNORDER_NORMAL;
    }
  }

  /* convert the order to a channelLayout */
  m_channelLayout = new enum AEChannel[9];
  unsigned int use = 0;
  for(unsigned int count = 0; order & 0xF; order = order >> 4)
  {
    switch(order & 0xF)
    {
      case CHID_L  : m_channelLayout[count] = AE_CH_FL ; break;
      case CHID_R  : m_channelLayout[count] = AE_CH_FR ; break;
      case CHID_C  : m_channelLayout[count] = AE_CH_FC ; break;
      case CHID_LFE: m_channelLayout[count] = AE_CH_LFE; break;
      case CHID_LS : m_channelLayout[count] = AE_CH_SL ; break;
      case CHID_RS : m_channelLayout[count] = AE_CH_SR ; break;
      case CHID_LR : m_channelLayout[count] = AE_CH_BL ; break;
      case CHID_RR : m_channelLayout[count] = AE_CH_BR ; break;

      default:
        continue;
    }

    for(unsigned int i = 0; format.m_channelLayout[i] != AE_CH_NULL; ++i)
      if (format.m_channelLayout[i] == m_channelLayout[count])
      {
        use = std::max(use, count);
        break;
      }

    ++count;
  }

  ++use;
  format.m_channelCount = use;
#endif

  /* find the number we need to open to access the channels we need */
  bool found = false;
  int oss_ch = 0;
  for(int ch = format.m_channelLayout.Count(); ch < 9; ++ch)
  {
    oss_ch = ch;
    if (ioctl(m_fd, SNDCTL_DSP_CHANNELS, &oss_ch) != -1 && oss_ch >= (int)format.m_channelLayout.Count())
    {
      found = true;
      break;
    }
  }

  m_channelLayout.Reset();
  for(int i = 0; i < oss_ch; ++i)
    m_channelLayout += OSSChannelMap[i];

  if (!found)
    CLog::Log(LOGWARNING, "CAESinkOSS::Initialize - Failed to access the number of channels required, falling back");


  int tmp = (CAEUtil::DataFormatToBits(format.m_dataFormat) >> 3) * format.m_channelLayout.Count() * OSS_FRAMES;
  int pos = 0;
  while((tmp & 0x1) == 0x0)
  {
    tmp = tmp >> 1;
    ++pos;
  }

  int oss_frag = (4 << 16) | pos;
  if (ioctl(m_fd, SNDCTL_DSP_SETFRAGMENT, &oss_frag) == -1)
    CLog::Log(LOGWARNING, "CAESinkOSS::Initialize - Failed to set the fragment size");


  int oss_sr = format.m_sampleRate;
  if (ioctl(m_fd, SNDCTL_DSP_SPEED, &oss_sr) == -1)
  {
    close(m_fd);
    CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to set the sample rate");
    return false;
  }

  audio_buf_info bi;
  if (ioctl(m_fd, SNDCTL_DSP_GETOSPACE, &bi) == -1)
  {
    close(m_fd);
    CLog::Log(LOGERROR, "CAESinkOSS::Initialize - Failed to get the output buffer size");
    return false;
  }

  format.m_sampleRate    = oss_sr;
  format.m_frameSize     = (CAEUtil::DataFormatToBits(format.m_dataFormat) >> 3) * format.m_channelLayout.Count();
  format.m_frames        = bi.fragsize / format.m_frameSize;
  format.m_frameSamples  = format.m_frames * format.m_channelLayout.Count();
  format.m_channelLayout = m_channelLayout;

  m_device = device;
  m_format = format;
  return true; 
}

void CAESinkOSS::Deinitialize()
{
  close(m_fd);
}

bool CAESinkOSS::IsCompatible(AEAudioFormat format, const CStdString device)
{
  return (
    format.m_sampleRate    == m_initFormat.m_sampleRate    &&
    format.m_dataFormat    == m_initFormat.m_dataFormat    &&
    format.m_channelLayout == m_initFormat.m_channelLayout &&
    GetDeviceUse(format, device) == m_device
  );
}

void CAESinkOSS::Stop()
{
}

float CAESinkOSS::GetDelay()
{
  int delay;
  if (ioctl(m_fd, SNDCTL_DSP_GETODELAY, &delay) == -1)
    return 0.0f;

  return (float)delay / (m_format.m_frameSize * m_format.m_sampleRate);
}

unsigned int CAESinkOSS::AddPackets(uint8_t *data, unsigned int frames)
{
  int size = frames * m_format.m_frameSize;
  int wrote = write(m_fd, data, size);
  if (wrote < 0)
  {
    CLog::Log(LOGERROR, "CAESinkOSS::AddPackets - Failed to write");
    return frames;
  }

  return wrote / m_format.m_frameSize;
}

void CAESinkOSS::EnumerateDevices (AEDeviceList &devices, bool passthrough)
{
}


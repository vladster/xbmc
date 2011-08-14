/*
 *      Copyright (C) 2005-2010 Team XBMC
 *      http://xbmc.org
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

#include "system.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "DllAvCore.h"

#include "AEFactory.h"
#include "Utils/AEUtil.h"

#include "SoftAE.h"
#include "SoftAEStream.h"

#define SOFTAE_FRAMES 1024

/* typecast AE to CSoftAE */
#define AE (*((CSoftAE*)CAEFactory::AE))

using namespace std;

CSoftAEStream::CSoftAEStream(enum AEDataFormat dataFormat, unsigned int sampleRate, CAEChannelInfo channelLayout, unsigned int options) :
  m_convertBuffer   (NULL ),
  m_valid           (false),
  m_delete          (false),
  m_volume          (1.0f ),
  m_rgain           (1.0f ),
  m_refillBuffer    (0    ),
  m_convertFn       (NULL ),
  m_frameBuffer     (NULL ),
  m_frameBufferSize (0    ),
  m_ssrc            (NULL ),
  m_framesBuffered  (0    ),
  m_vizPacketPos    (NULL ),
  m_draining        (false),
  m_vizBufferSamples(0    ),
  m_audioCallback   (NULL ),
  m_fadeRunning     (false)
{
  m_ssrcData.data_out = NULL;

  m_initDataFormat    = dataFormat;
  m_initSampleRate    = sampleRate;
  m_initChannelLayout = channelLayout;
  m_chLayoutCount     = channelLayout.Count();
  m_forceResample     = (options & AESTREAM_FORCE_RESAMPLE) != 0;
  m_paused            = (options & AESTREAM_PAUSED) != 0;
  
  ASSERT(m_initChannelLayout.Count());  
}

void CSoftAEStream::InitializeRemap()
{
  CSingleLock lock(m_critSection);
  if (!AE_IS_RAW(m_initDataFormat))
  {
    /* re-init the remappers */
    m_remap   .Initialize(m_initChannelLayout, AE.GetChannelLayout()           , false);
    m_vizRemap.Initialize(m_initChannelLayout, CAEChannelInfo(AE_CH_LAYOUT_2_0), false, true);

    /*
    if the layout has changed we need to drop data that was already remapped
    */
    if (AE.GetChannelLayout() != m_aeChannelLayout)
    {
      InternalFlush();
      m_aeChannelLayout = AE.GetChannelLayout();
      m_aeChannelCount  = AE.GetChannelCount();
      m_aePacketSamples = SOFTAE_FRAMES * m_aeChannelCount;
    }
  }
}

void CSoftAEStream::Initialize()
{
  CSingleLock lock(m_critSection);
  if (m_valid)
  {
    InternalFlush();
    _aligned_free(m_newPacket.data   );
    _aligned_free(m_newPacket.vizData);

    if (m_convert)
      _aligned_free(m_convertBuffer);

    if (m_resample)
    {
      _aligned_free(m_ssrcData.data_out);
      m_ssrcData.data_out = NULL;
    }
  }

  enum AEDataFormat useDataFormat = m_initDataFormat;
  if (AE_IS_RAW(m_initDataFormat))
  {
    /* we are raw, which means we need to work in the output format */
    useDataFormat       = ((CSoftAE*)&AE)->GetSinkDataFormat();
    m_initChannelLayout = ((CSoftAE*)&AE)->GetSinkChLayout  ();
  }
  else
  {
    if (!m_initChannelLayout.Count())
    {
      m_valid = false;
      return;
    }
  }

  m_bytesPerSample  = (CAEUtil::DataFormatToBits(useDataFormat) >> 3);
  m_bytesPerFrame   = m_bytesPerSample * m_initChannelLayout.Count();

  m_aeChannelLayout = AE.GetChannelLayout();
  m_aeChannelCount  = AE.GetChannelCount();
  m_aePacketSamples = SOFTAE_FRAMES * m_aeChannelCount;
  m_waterLevel      = SOFTAE_FRAMES * 8;

  m_format.m_dataFormat    = useDataFormat;
  m_format.m_sampleRate    = m_initSampleRate;
  m_format.m_channelLayout = m_initChannelLayout;
  m_format.m_frames        = SOFTAE_FRAMES;
  m_format.m_frameSamples  = m_format.m_frames * m_initChannelLayout.Count();
  m_format.m_frameSize     = m_bytesPerFrame;

  if (!AE_IS_RAW(m_initDataFormat))
  {
    if (
      !m_remap   .Initialize(m_initChannelLayout, m_aeChannelLayout               , false) ||
      !m_vizRemap.Initialize(m_initChannelLayout, CAEChannelInfo(AE_CH_LAYOUT_2_0), false, true))
    {
      m_valid = false;
      return;
    }

    m_newPacket.data = (uint8_t*)_aligned_malloc(m_format.m_frameSamples * sizeof(float), 16);
  }
  else
    m_newPacket.data = (uint8_t*)_aligned_malloc(m_format.m_frames * m_format.m_frameSize, 16);

  m_newPacket.vizData = NULL;
  m_newPacket.samples = 0;
  m_packet.samples    = 0;
  m_packet.data       = NULL;
  m_packet.vizData    = NULL;

  if (!m_frameBuffer)
    m_frameBuffer = (uint8_t*)_aligned_malloc(m_format.m_frames * m_bytesPerFrame, 16);

  m_resample      = (m_forceResample || m_initSampleRate != AE.GetSampleRate()) && !AE_IS_RAW(m_initDataFormat);
  m_convert       = m_initDataFormat != AE_FMT_FLOAT && !AE_IS_RAW(m_initDataFormat);

  /* if we need to convert, set it up */
  if (m_convert)
  {
    /* get the conversion function and allocate a buffer for the data */
    CLog::Log(LOGDEBUG, "CSoftAEStream::CSoftAEStream - Converting from %s to AE_FMT_FLOAT", CAEUtil::DataFormatToStr(m_initDataFormat));
    m_convertFn = CAEConvert::ToFloat(m_initDataFormat);
    if (m_convertFn) m_convertBuffer = (float*)_aligned_malloc(m_format.m_frameSamples * sizeof(float), 16);
    else             m_valid         = false;
  }
  else
    m_convertBuffer = (float*)m_frameBuffer;

  /* if we need to resample, set it up */
  if (m_resample)
  {
    int err;
    m_ssrc                   = src_new(SRC_SINC_MEDIUM_QUALITY, m_initChannelLayout.Count(), &err);
    m_ssrcData.data_in       = m_convertBuffer;
    m_ssrcData.src_ratio     = (double)AE.GetSampleRate() / (double)m_initSampleRate;
    m_ssrcData.data_out      = (float*)_aligned_malloc(m_format.m_frameSamples * std::ceil(m_ssrcData.src_ratio) * sizeof(float), 16);
    m_ssrcData.output_frames = m_format.m_frames * std::ceil(m_ssrcData.src_ratio);
    m_ssrcData.end_of_input  = 0;
  }

  m_chLayoutCount = m_format.m_channelLayout.Count();  
  m_valid = true;
}

void CSoftAEStream::Destroy()
{
  CSingleLock lock(m_critSection);
  m_valid       = false;
  m_delete      = true;
}

CSoftAEStream::~CSoftAEStream()
{
  CSingleLock lock(m_critSection);

  InternalFlush();
  _aligned_free(m_frameBuffer);
  if (m_convert)
    _aligned_free(m_convertBuffer);

  if (m_resample)
  {
    _aligned_free(m_ssrcData.data_out);
    src_delete(m_ssrc);
    m_ssrc = NULL;
  }

  _aligned_free(m_newPacket.data);

  CLog::Log(LOGDEBUG, "CSoftAEStream::~CSoftAEStream - Destructed");
}

unsigned int CSoftAEStream::GetSpace()
{
  CSingleLock lock(m_critSection);
  if (!m_valid || m_draining) return 0;  

  if (m_framesBuffered >= m_waterLevel)
    return 0;

  return (m_format.m_frames * m_bytesPerFrame) - m_frameBufferSize;
}

unsigned int CSoftAEStream::AddData(void *data, unsigned int size)
{
  CSingleLock lock(m_critSection);
  if (!m_valid || size == 0 || data == NULL || m_draining) return 0;  

  if (m_framesBuffered >= m_waterLevel)
    return 0;

  uint8_t *ptr = (uint8_t*)data;
  while(size)
  {
    size_t room = (m_format.m_frames * m_bytesPerFrame) - m_frameBufferSize;
    size_t copy = std::min((size_t)size, room);
    if (copy == 0)
      break;

    memcpy(m_frameBuffer + m_frameBufferSize, ptr, copy);
    size              -= copy;
    m_frameBufferSize += copy;
    ptr               += copy;

    if(m_frameBufferSize / m_bytesPerSample < m_format.m_frameSamples)
      continue;

    unsigned int consumed = ProcessFrameBuffer();
    if (consumed)
    {
      m_frameBufferSize -= consumed;
      memmove(m_frameBuffer + consumed, m_frameBuffer, m_frameBufferSize);
      continue;
    }
  }

  return ptr - (uint8_t*)data;
}

unsigned int CSoftAEStream::ProcessFrameBuffer()
{
  uint8_t     *data;
  unsigned int frames, consumed;

  /* convert the data if we need to */
  unsigned int samples;
  if (m_convert)
  {
    data    = (uint8_t*)m_convertBuffer;
    samples = m_convertFn(m_frameBuffer, m_frameBufferSize / m_bytesPerSample, m_convertBuffer);
  }
  else
  {
    data    = (uint8_t*)m_frameBuffer;
    samples = m_frameBufferSize / m_bytesPerSample;
  }

  if (samples == 0)
    return 0;

  /* resample it if we need to */
  if (m_resample)
  {
    m_ssrcData.input_frames = samples / m_chLayoutCount;
    if (src_process(m_ssrc, &m_ssrcData) != 0) return 0;
    data     = (uint8_t*)m_ssrcData.data_out;
    frames   = m_ssrcData.output_frames_gen;
    consumed = m_ssrcData.input_frames_used * m_bytesPerFrame;
    if (!frames)
      return consumed;

    samples = frames * m_chLayoutCount;
  }
  else
  {
    data     = (uint8_t*)m_convertBuffer;
    frames   = samples / m_chLayoutCount;
    samples  = frames * m_chLayoutCount;
    consumed = frames * m_bytesPerFrame;
  }

  /* buffer the data */
  m_framesBuffered += frames;
  while(samples)
  {
    unsigned int room = m_format.m_frameSamples - m_newPacket.samples;
    unsigned int copy = std::min(room, samples);

    if (AE_IS_RAW(m_initDataFormat))
    {
      unsigned int size = copy * m_bytesPerSample;
      memcpy(m_newPacket.data + (m_newPacket.samples * m_bytesPerSample), data, size);
      data += size;
    }
    else
    {
      unsigned int size = copy * sizeof(float);
      memcpy((float*)m_newPacket.data + m_newPacket.samples, data, size);
      data += size;
    }

    m_newPacket.samples += copy;
    samples             -= copy;

    /* if we have a full block of data */
    if (m_newPacket.samples == m_format.m_frameSamples)
    {
      if (AE_IS_RAW(m_initDataFormat))
      {
        m_outBuffer.push_back(m_newPacket);
        m_newPacket.samples = 0;
        m_newPacket.data    = (uint8_t*)_aligned_malloc(m_format.m_frames * m_format.m_frameSize, 16);
      }
      else
      {
        /* downmix/remap the data */
        PPacket pkt;
        pkt.samples = m_aePacketSamples;
        pkt.data    = (uint8_t*)_aligned_malloc(m_aePacketSamples * sizeof(float), 16);
        m_remap.Remap((float*)m_newPacket.data, (float*)pkt.data, m_format.m_frames);

        /* downmix for the viz if we have one */
        if (m_audioCallback)
        {
          pkt.vizData = (float*)_aligned_malloc(m_format.m_frames * 2 * sizeof(float), 16);
          m_vizRemap.Remap((float*)m_newPacket.data, pkt.vizData, m_format.m_frames);
        }
        else
          pkt.vizData = NULL;

        /* add the packet to the output */
        m_outBuffer.push_back(pkt);
        m_newPacket.samples = 0;
      }
    }
  }

  if (m_refillBuffer)
    m_refillBuffer = (unsigned int)std::max((int)m_refillBuffer - (int)frames, 0);

  return consumed;
}

uint8_t* CSoftAEStream::GetFrame()
{
  CSingleLock lock(m_critSection);

  /* if we have been deleted or are refilling but not draining */
  if (!m_valid || m_delete || (m_refillBuffer && !m_draining)) return NULL;

  /* if the packet is empty, advance to the next one */
  if(!m_packet.samples)
  {
    _aligned_free(m_packet.data   );
    _aligned_free(m_packet.vizData);
    m_packet.data    = NULL;
    m_packet.vizData = NULL;
    
    /* no more packets, return null */
    if (m_outBuffer.empty())
    {
      if (m_draining)
        return NULL;
      else
      {
        /* underrun, we need to refill our buffers */
        m_refillBuffer = m_waterLevel;
        return NULL;
      }
    }

    /* get the next packet */
    m_packet = m_outBuffer.front();
    m_outBuffer.pop_front();

    m_packetPos    = m_packet.data;
    m_vizPacketPos = m_packet.vizData;
  }
  
  /* fetch one frame of data */
  uint8_t *ret      = (uint8_t*)m_packetPos;
  float   *vizData  = m_vizPacketPos;

  m_packet.samples -= m_aeChannelCount;
  if (AE_IS_RAW(m_initDataFormat))
    m_packetPos += m_bytesPerFrame;
  else
  {
    m_packetPos += m_aeChannelCount * sizeof(float);
    if(vizData)
      m_vizPacketPos += 2;

    /* if we are fading */
    if (m_fadeRunning)
    {
      m_volume += m_fadeStep;
      m_volume = std::min(1.0f, std::max(0.0f, m_volume));
      if (m_fadeDirUp)
      {
        if (m_volume >= m_fadeTarget)
          m_fadeRunning = false;
      }
      else
      {
        if (m_volume <= m_fadeTarget)
          m_fadeRunning = false;
      }
    }
  }

  --m_framesBuffered;

  /* we have a frame, if we have a viz we need to hand the data to it */
  if (m_audioCallback && vizData)
  {
    memcpy(m_vizBuffer + m_vizBufferSamples, vizData, 2 * sizeof(float));
    m_vizBufferSamples += 2;
    if (m_vizBufferSamples == 512)
    {
      m_audioCallback->OnAudioData(m_vizBuffer, 512);
      m_vizBufferSamples = 0;
    }
  }

  return ret;
}

float CSoftAEStream::GetDelay()
{
  if (m_delete) return 0.0f;
  float delay = (float)m_framesBuffered / (float)AE.GetSampleRate();
  return AE.GetDelay() + delay;
}

float CSoftAEStream::GetCacheTime()
{
  if (m_delete) return 0.0f;
  return (float)std::max((int)m_waterLevel - (int)m_refillBuffer, 0) / (float)AE.GetSampleRate();
}

float CSoftAEStream::GetCacheTotal()
{
  if (m_delete) return 0.0f;
  return (float)m_waterLevel / (float)AE.GetSampleRate();
}

void CSoftAEStream::Drain()
{
  CSingleLock lock(m_critSection);
  m_draining = true;
}

bool CSoftAEStream::IsDrained()
{
  CSingleLock lock(m_critSection);  
  return (!m_packet.samples && m_outBuffer.empty());
}

void CSoftAEStream::Flush()
{
  CSingleLock lock(m_critSection);
  InternalFlush();

  /* internal flush does not do this as these samples are still valid if we are re-initializing */
  m_frameBufferSize = 0;
}

void CSoftAEStream::InternalFlush()
{
  /* reset the resampler */
  if (m_resample) {
    m_ssrcData.end_of_input = 0;
    src_reset(m_ssrc);
  }
  
  /* invalidate any incoming samples */
  m_newPacket.samples = 0;
  
  /*
    clear the current buffered packet we cant delete the data as it may be
    in use by the AE thread, so we just set the packet count to 0, it will
    get freed by the next call to GetFrame or destruction
  */
  m_packet.samples = 0;

  /* clear any other buffered packets */
  while(!m_outBuffer.empty()) {    
    PPacket p = m_outBuffer.front();
    m_outBuffer.pop_front();    
    _aligned_free(p.data);
    _aligned_free(p.vizData);
    p.data    = NULL;
    p.vizData = NULL;
  };
 
  /* reset our counts */
  m_framesBuffered = 0;
}

double CSoftAEStream::GetResampleRatio()
{
  if (!m_resample)
    return 1.0f;

  CSingleLock lock(m_critSection);
  return m_ssrcData.src_ratio;
}

void CSoftAEStream::SetResampleRatio(double ratio)
{
  if (!m_resample)
    return;

  CSingleLock lock(m_critSection);

  int oldRatioInt = std::ceil(m_ssrcData.src_ratio);

  src_set_ratio(m_ssrc, ratio);
  m_ssrcData.src_ratio = ratio;

  //Check the resample buffer size and resize if necessary.
  if(oldRatioInt < std::ceil(m_ssrcData.src_ratio))
  {
    _aligned_free(m_ssrcData.data_out);
    m_ssrcData.data_out      = (float*)_aligned_malloc(m_format.m_frameSamples * std::ceil(m_ssrcData.src_ratio) * sizeof(float), 16);
    m_ssrcData.output_frames = m_format.m_frames * std::ceil(m_ssrcData.src_ratio);
  }
}

void CSoftAEStream::RegisterAudioCallback(IAudioCallback* pCallback)
{
  CSingleLock lock(m_critSection);
  m_vizBufferSamples = 0;
  m_audioCallback = pCallback;
  if (m_audioCallback)
    m_audioCallback->OnInitialize(2, m_initSampleRate, 32);
}

void CSoftAEStream::UnRegisterAudioCallback()
{
  CSingleLock lock(m_critSection);
  m_audioCallback = NULL;
  m_vizBufferSamples = 0;
}

void CSoftAEStream::FadeVolume(float from, float target, unsigned int time)
{
  float delta   = target - from;
  m_fadeDirUp   = target > from;
  m_fadeTarget  = target;
  m_fadeStep    = delta / (((float)AE.GetSampleRate() / 1000.0f) * (float)time);
  m_fadeRunning = true;
}

bool CSoftAEStream::IsFading()
{
  return m_fadeRunning;
}

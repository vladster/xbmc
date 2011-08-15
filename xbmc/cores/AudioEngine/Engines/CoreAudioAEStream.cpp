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
#include "DllAvCore.h"

#include "Interfaces/AE.h"
#include "AEFactory.h"
#include "Utils/AEUtil.h"

#include "CoreAudioAE.h"
#include "CoreAudioAEStream.h"

#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "MathUtils.h"

/* typecast AE to CCoreAudioAE */
#define AE (*(CCoreAudioAE*)CAEFactory::AE)

using namespace std;

template <class AudioDataType>
static inline void _ReorderSmpteToCA(AudioDataType *buf, uint frames)
{
	AudioDataType tmpLS, tmpRS, tmpRLs, tmpRRs, *buf2;
	for (uint i = 0; i < frames; i++)
	{
		buf = buf2 = buf + 4;
		tmpRLs = *buf++;
		tmpRRs = *buf++;
		tmpLS = *buf++;
		tmpRS = *buf++;
		
		*buf2++ = tmpLS;
		*buf2++ = tmpRS;
		*buf2++ = tmpRLs;
		*buf2++ = tmpRRs;
	}
}

void CCoreAudioAEStream::ReorderSmpteToCA(void *buf, uint frames, AEDataFormat dataFormat)
{
	switch((CAEUtil::DataFormatToBits(dataFormat) >> 3))
	{
		case 8: _ReorderSmpteToCA((unsigned char *)buf, frames); break;
		case 16: _ReorderSmpteToCA((short *)buf, frames); break;
		default: _ReorderSmpteToCA((int *)buf, frames); break;
	}
}

CCoreAudioAEStream::CCoreAudioAEStream(enum AEDataFormat dataFormat, unsigned int sampleRate, CAEChannelInfo channelLayout, unsigned int options) :
  m_convertBuffer   (NULL ),
  m_valid           (false),
  m_delete          (false),
  m_volume          (1.0f ),
  m_rgain           (1.0f ),
  m_convertFn       (NULL ),
  m_ssrc            (NULL ),
  m_draining        (false),
  m_audioCallback   (NULL ),
  m_AvgBytesPerSec  (0    ),
  m_Buffer          (NULL ),
  m_fadeRunning     (false),
  m_outputUnit      (NULL ),
  m_frameSize       (0    ),
  m_doRemap         (true )
{
  m_ssrcData.data_out             = NULL;

  m_StreamFormat.m_dataFormat     = dataFormat;
  m_StreamFormat.m_sampleRate     = sampleRate;
  m_StreamFormat.m_channelLayout  = channelLayout;
  m_chLayoutCountStream           = m_StreamFormat.m_channelLayout.Count();
  
  m_OutputFormat                  = AE.GetAudioFormat();
  m_chLayoutCountOutput           = m_OutputFormat.m_channelLayout.Count();
  
  m_forceResample                 = (options & AESTREAM_FORCE_RESAMPLE) != 0;
  m_paused                        = (options & AESTREAM_PAUSED) != 0;

  m_vizRemapBufferSize            = m_remapBufferSize = m_resampleBufferSize = m_convertBufferSize = 16*1024;
  m_convertBuffer                 = (float*)_aligned_malloc(m_convertBufferSize,16);
  m_resampleBuffer                = (float*)_aligned_malloc(m_resampleBufferSize,16);
  m_remapBuffer                   = (uint8_t *)_aligned_malloc(m_remapBufferSize,16); 
  m_vizRemapBuffer                = (uint8_t*)_aligned_malloc(m_vizRemapBufferSize,16);

  m_isRaw                         = COREAUDIO_IS_RAW(dataFormat);
}

CCoreAudioAEStream::~CCoreAudioAEStream()
{
  CloseConverter();
  
  m_delete = true;
  m_valid = false;
  
  InternalFlush();
  
  _aligned_free(m_convertBuffer);
  _aligned_free(m_resampleBuffer);
  _aligned_free(m_remapBuffer);
  _aligned_free(m_vizRemapBuffer);
  
  if(m_Buffer)
    delete m_Buffer;
  
  /*
  if (m_resample)
  {
    _aligned_free(m_ssrcData.data_out);
    src_delete(m_ssrc);
    m_ssrc = NULL;
  }
  */
  
  CLog::Log(LOGDEBUG, "CCoreAudioAEStream::~CCoreAudioAEStream - Destructed");
}

void CCoreAudioAEStream::InitializeRemap()
{
//#if defined(TARGET_DARWIN_OSX)
  if (!m_isRaw)
  {
    if (m_OutputFormat.m_channelLayout != AE.GetChannelLayout())
    {
      m_OutputFormat            = AE.GetAudioFormat();
      m_chLayoutCountOutput     = m_OutputFormat.m_channelLayout.Count();
      m_OutputBytesPerSample    = (CAEUtil::DataFormatToBits(m_OutputFormat.m_dataFormat) >> 3);
      
      /* re-init the remappers */
      m_remap   .Initialize(m_StreamFormat.m_channelLayout, m_OutputFormat.m_channelLayout, false);
      m_vizRemap.Initialize(m_StreamFormat.m_channelLayout, CAEChannelInfo(AE_CH_LAYOUT_2_0), false, true);

      InternalFlush();
    }
  }
//#endif

}

void CCoreAudioAEStream::ReinitConverter()
{
  CloseConverter();
  OpenConverter();
}

// The source logic is in the HAL. The only thing we have to do here
// is to allocate the convrter and set the direct input call.
void CCoreAudioAEStream::CloseConverter()
{
  // we have a converter, delete it
  if(m_outputUnit)
    m_outputUnit = (CAUOutputDevice *) AE.GetHAL()->DestroyUnit(m_outputUnit);

  // it is save to unregister any direct input. the HAL takes care about it.
  AE.GetHAL()->SetDirectInput(NULL, m_OutputFormat);
}

void CCoreAudioAEStream::OpenConverter()
{
  // we allways allocate a converter
  // the HAL decides if we get converter. 
  // if there is already a converter delete it.
  if(m_outputUnit)
    m_outputUnit = (CAUOutputDevice *) AE.GetHAL()->DestroyUnit(m_outputUnit);

  m_outputUnit = (CAUOutputDevice *) AE.GetHAL()->CreateUnit(this, m_OutputFormat);

  // it is save to register any direct input. the HAL takes care about it.
  AE.GetHAL()->SetDirectInput(this, m_OutputFormat);
}

void CCoreAudioAEStream::Initialize()
{    
  if (m_valid)
  {
    InternalFlush();
  }
  
  m_OutputFormat            = AE.GetAudioFormat();
  m_chLayoutCountOutput     = m_OutputFormat.m_channelLayout.Count();
  m_OutputBytesPerSample    = (CAEUtil::DataFormatToBits(m_OutputFormat.m_dataFormat) >> 3);

  if(m_isRaw)
  {
    /* we are raw, which means we need to work in the output format */
    m_StreamFormat                = AE.GetAudioFormat();
    m_chLayoutCountStream         = m_StreamFormat.m_channelLayout.Count();
    m_StreamBytesPerSample        = (CAEUtil::DataFormatToBits(m_StreamFormat.m_dataFormat) >> 3);
  }
  else
  {
    if (!m_chLayoutCountStream)
    {
      m_valid = false;
      return;
    }
    /* Work around a bug in TrueHD and DTSHD deliver */
    if(m_StreamFormat.m_dataFormat == AE_FMT_TRUEHD || m_StreamFormat.m_dataFormat == AE_FMT_DTSHD)
    {
      m_StreamBytesPerSample        = (CAEUtil::DataFormatToBits(AE_FMT_S16NE) >> 3);
    }
    else
    {
      m_StreamBytesPerSample        = (CAEUtil::DataFormatToBits(m_StreamFormat.m_dataFormat) >> 3);
    }
    m_StreamFormat.m_frameSize    = m_StreamBytesPerSample * m_chLayoutCountStream;
  }

  if (!m_isRaw)
  {
    if (
      !m_remap.Initialize(m_StreamFormat.m_channelLayout, m_OutputFormat.m_channelLayout, false) ||
      !m_vizRemap.Initialize(m_OutputFormat.m_channelLayout, CAEChannelInfo(AE_CH_LAYOUT_2_0), false, true))
    {
      m_valid = false;
      return;
    }
#if defined(TARGET_DARWIN_OSX)
    m_doRemap  = true;
#else
    // on ios remap when channel count != 2. our output is always 2 channels.
    m_doRemap  = m_chLayoutCountStream != 2;
#endif
  }

  m_convert       = m_StreamFormat.m_dataFormat != AE_FMT_FLOAT && !m_isRaw;
#if defined(TARGET_DARWIN_OSX)
  m_resample      = (m_StreamFormat.m_sampleRate != m_OutputFormat.m_sampleRate) && !m_isRaw;
#else
  m_resample      = false;
#endif

  // Test
  /*
  m_resample  = false;
  m_convert   = false;
  m_doRemap   = false;
  */

  /* if we need to convert, set it up */
  if (m_convert)
  {
    /* get the conversion function and allocate a buffer for the data */
    CLog::Log(LOGDEBUG, "CCoreAudioAEStream::CCoreAudioAEStream - Converting from %s to AE_FMT_FLOAT", CAEUtil::DataFormatToStr(m_StreamFormat.m_dataFormat));
    m_convertFn = CAEConvert::ToFloat(m_StreamFormat.m_dataFormat);
    
    if (!m_convertFn)
      m_valid         = false;
  }
  
  /* if we need to resample, set it up */
  if (m_resample)
  {
    int err;
    m_ssrc                   = src_new(SRC_SINC_MEDIUM_QUALITY, m_chLayoutCountStream, &err);
    m_ssrcData.src_ratio     = (double)m_OutputFormat.m_sampleRate / (double)m_StreamFormat.m_sampleRate;
    m_ssrcData.data_in       = m_convertBuffer;
    m_ssrcData.end_of_input  = 0;
  }
  
  // m_AvgBytesPerSec is calculated based on the output format.
  // we have to keep in mind that we convert our data to the output format
  m_AvgBytesPerSec =  m_OutputFormat.m_frameSize * m_OutputFormat.m_sampleRate;
  //m_AvgBytesPerSec =  m_StreamFormat.m_frameSize * m_StreamFormat.m_sampleRate;

  if(m_Buffer)
    delete m_Buffer;
  
  m_Buffer = new CoreAudioRingBuffer(m_AvgBytesPerSec);

  m_fadeRunning = false;

  OpenConverter();
  
  m_valid = true;
}

void CCoreAudioAEStream::Destroy()
{
  m_valid       = false;
  m_delete      = true;
  InternalFlush();
}

unsigned int CCoreAudioAEStream::AddData(void *data, unsigned int size)
{
  unsigned int frames   = size / m_StreamFormat.m_frameSize;
  unsigned int samples  = size / m_StreamBytesPerSample;
  uint8_t     *adddata  = (uint8_t *)data;
  unsigned int addsize  = size;
  
  if (!m_valid || size == 0 || data == NULL || m_draining || !m_Buffer)
  {
    return 0; 
  }

  /* convert the data if we need to */
  if (m_convert)
  {
    CheckOutputBufferSize((void **)&m_convertBuffer, &m_convertBufferSize, frames * m_chLayoutCountStream  * sizeof(float) * 2);

    samples     = m_convertFn(adddata, size / m_StreamBytesPerSample, m_convertBuffer);
    adddata     = (uint8_t *)m_convertBuffer;
    addsize     = frames * sizeof(float) * m_chLayoutCountStream;
  }
  else
  {
    samples     = size / m_StreamBytesPerSample;
    adddata     = (uint8_t *)data;
    addsize     = size;
  }

  if (samples == 0)
    return 0;
  
  /* resample it if we need to */
  if (m_resample)
  {
    unsigned int resample_frames = samples / m_chLayoutCountStream;
    
    CheckOutputBufferSize((void **)&m_resampleBuffer, &m_resampleBufferSize, 
                          resample_frames * std::ceil(m_ssrcData.src_ratio) * sizeof(float) * 2);
    
    m_ssrcData.input_frames   = resample_frames;
    m_ssrcData.output_frames  = resample_frames * std::ceil(m_ssrcData.src_ratio);
    m_ssrcData.data_in        = (float *)adddata;
    m_ssrcData.data_out       = m_resampleBuffer;
        
    if (src_process(m_ssrc, &m_ssrcData) != 0) 
      return 0;
    
    frames    = m_ssrcData.output_frames_gen;    
    samples   = frames * m_chLayoutCountStream;
    adddata   = (uint8_t *)m_ssrcData.data_out;
  }
  else
  {
    frames    = samples / m_chLayoutCountStream;
    samples   = frames * m_chLayoutCountStream;
  }

  if (!m_isRaw && m_doRemap)
  {
    addsize = frames * m_OutputBytesPerSample * m_chLayoutCountOutput;
    
    CheckOutputBufferSize((void **)&m_remapBuffer, &m_remapBufferSize, addsize * 2);
        
    // downmix/remap the data
    m_remap.Remap((float *)adddata, (float *)m_remapBuffer, frames);
    adddata   = (uint8_t *)m_remapBuffer;
  }

  if(m_chLayoutCountOutput == 8 && !m_isRaw)
    ReorderSmpteToCA(adddata, addsize / m_OutputFormat.m_frameSize, m_OutputFormat.m_dataFormat);

  //if(m_chLayoutCountStream == 8 && !m_isRaw)
  //  ReorderSmpteToCA(adddata, addsize / m_StreamFormat.m_frameSize, m_StreamFormat.m_dataFormat);

  unsigned int room = m_Buffer->GetWriteSize();
  /*
  while(addsize > room)
  {
    // we got deleted
    if (!m_valid || size == 0 || data == NULL || m_draining || !m_Buffer)
    {
      return 0;
    }
    // sleep buffer half empty
    Sleep(100);
    room = m_Buffer->GetWriteSize();
  }
  */
  if(addsize > room)
  {
    //CLog::Log(LOGDEBUG, "CCoreAudioAEStream::AddData failed : free size %d add size %d", room, addsize);
    size = 0;
  }
  else 
  {
    m_Buffer->Write(adddata, addsize);
  }
    
  //printf("AddData  : %d %d\n", addsize, m_Buffer->GetWriteSize());

  return size;    
}

unsigned int CCoreAudioAEStream::GetFrames(uint8_t *buffer, unsigned int size)
{  
  /* if we have been deleted */
  if (!m_valid || m_delete || !m_Buffer)
  {
    return 0;
  }

  unsigned int readsize = std::min(m_Buffer->GetReadSize(), size);
  m_Buffer->Read(buffer, readsize);

  /* we have a frame, if we have a viz we need to hand the data to it.
     Keep in mind that our buffer is already in output format. 
     So we remap output format to viz format !!!*/
//#if defined(TARGET_DARWIN_OSX)
  if(!m_isRaw && (m_OutputFormat.m_dataFormat == AE_FMT_FLOAT))
  {
    // TODO : Why the hell is vizdata limited ?
    unsigned int samples   = readsize / m_OutputBytesPerSample;
    unsigned int frames    = samples / m_chLayoutCountOutput;

    if(samples) {
      // Viz channel count is 2
      CheckOutputBufferSize((void **)&m_vizRemapBuffer, &m_vizRemapBufferSize, frames * 2 * sizeof(float));
      
      samples  = (samples > 512) ? 512 : samples;
      
      m_vizRemap.Remap((float*)buffer, (float*)m_vizRemapBuffer, frames);
      if (m_audioCallback)
      {
        m_audioCallback->OnAudioData((float *)m_vizRemapBuffer, samples);
      }
    }
  }
//#endif

  /* if we are fading */
  if (m_fadeRunning)
  {
    // TODO: check if we correctly respect the amount of our blockoperation
    m_volume += (m_fadeStep * ((float)readsize / (float)m_StreamFormat.m_frameSize));
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
  //m_fadeRunning = false;

  /*
  if(readsize == 0)
  {
    printf("buffer size zero\n");
  }
  */

  return readsize;  
}

const unsigned int CCoreAudioAEStream::GetFrameSize() const
{
  return m_OutputFormat.m_frameSize;
}

unsigned int CCoreAudioAEStream::GetSpace()
{
  if(!m_valid || m_draining) 
    return 0;
  
  return m_Buffer->GetWriteSize();
}

float CCoreAudioAEStream::GetDelay()
{
  if (m_delete || !m_Buffer) return 0.0f;
  
  float delay = (float)(m_Buffer->GetReadSize()) / (float)m_AvgBytesPerSec;
  
  delay += AE.GetDelay();
  
  return delay;
}

float CCoreAudioAEStream::GetCacheTime()
{
  if (m_delete || !m_Buffer) return 0.0f;

  return (float)(m_Buffer->GetReadSize()) / (float)m_AvgBytesPerSec;
}

float CCoreAudioAEStream::GetCacheTotal()
{
  if (m_delete || !m_Buffer) return 0.0f;
  
  return (float)m_Buffer->GetMaxSize() / (float)m_AvgBytesPerSec;
}

bool CCoreAudioAEStream::IsPaused()
{
  return m_paused;
}

bool CCoreAudioAEStream::IsDraining () 
{
  return m_draining;
}

bool CCoreAudioAEStream::IsDestroyed()
{ 
  return m_delete;
}

bool CCoreAudioAEStream::IsValid() 
{ 
  return m_valid;
}

void CCoreAudioAEStream::Pause()
{
  m_paused = true;
}

void CCoreAudioAEStream::Resume()
{
  m_paused = false;
}

void CCoreAudioAEStream::Drain()
{
  m_draining = true;
}


bool CCoreAudioAEStream::IsDrained()
{
  if(m_Buffer->GetReadSize() > 0)
    return false;
  else
    return true;
}

void CCoreAudioAEStream::Flush()
{
  InternalFlush();
}

float CCoreAudioAEStream::GetVolume()
{
  return m_volume;
}

float CCoreAudioAEStream::GetReplayGain()
{ 
  return m_rgain;
}

void  CCoreAudioAEStream::SetVolume(float volume)
{
  m_volume = std::max( 0.0f, std::min(1.0f, volume));
}

void  CCoreAudioAEStream::SetReplayGain(float factor)
{
  m_rgain  = std::max(-1.0f, std::max(1.0f, factor));
}

void CCoreAudioAEStream::InternalFlush()
{
  /* reset the resampler */
  if (m_resample) {
    m_ssrcData.end_of_input = 0;
    src_reset(m_ssrc);
  }

  // Read the buffer empty to avoid Reset
  // Reset is not lock free.
  if(m_Buffer)
  {    
    unsigned int readsize = m_Buffer->GetReadSize();
  
    if(readsize)
    {
      uint8_t *buffer = (uint8_t *)_aligned_malloc(readsize, 16);
      m_Buffer->Read(buffer, readsize);
      _aligned_free(buffer);
    }
  }
  
  //if(m_Buffer)
  //  m_Buffer->Reset();
}

const unsigned int CCoreAudioAEStream::GetChannelCount() const
{
  return m_chLayoutCountStream;
}

const unsigned int CCoreAudioAEStream::GetSampleRate() const
{
  return m_StreamFormat.m_sampleRate;
}

const enum AEDataFormat CCoreAudioAEStream::GetDataFormat() const
{
  return m_StreamFormat.m_dataFormat;
}

const bool CCoreAudioAEStream::IsRaw() const
{
  return m_isRaw;
}

double CCoreAudioAEStream::GetResampleRatio()
{
  if (!m_resample)
    return 1.0f;

  double ret = m_ssrcData.src_ratio;
  return ret;
}

void CCoreAudioAEStream::SetResampleRatio(double ratio)
{
  if (!m_resample)
    return;

  src_set_ratio(m_ssrc, ratio);
  m_ssrcData.src_ratio = ratio;
}

void CCoreAudioAEStream::RegisterAudioCallback(IAudioCallback* pCallback)
{
  m_audioCallback = pCallback;
  if (m_audioCallback)
    m_audioCallback->OnInitialize(2, m_StreamFormat.m_sampleRate, 32);
}

void CCoreAudioAEStream::UnRegisterAudioCallback()
{
  m_audioCallback = NULL;
}

void CCoreAudioAEStream::FadeVolume(float from, float target, unsigned int time)
{
  float delta   = target - from;
  m_fadeDirUp   = target > from;
  m_fadeTarget  = target;
  m_fadeStep    = delta / (((float)m_OutputFormat.m_sampleRate / 1000.0f) * (float)time);
  m_fadeRunning = true;
}

bool CCoreAudioAEStream::IsFading()
{
  return m_fadeRunning;
}

OSStatus CCoreAudioAEStream::OnRender(AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
  // the index is important if we run encoded
  unsigned outputBufferIndex = AE.GetHAL()->GetBufferIndex();
  
  // if we have no valid data output silence
  if (!m_valid || m_delete || !m_Buffer)
  {
    ioData->mBuffers[outputBufferIndex].mDataByteSize = 0;
    if(ioActionFlags)
      *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
  }

  unsigned int size = inNumberFrames * m_OutputFormat.m_frameSize;
  //unsigned int size = inNumberFrames * m_StreamFormat.m_frameSize;
  
  ioData->mBuffers[outputBufferIndex].mDataByteSize  = GetFrames((unsigned char *)ioData->mBuffers[outputBufferIndex].mData, size);
  if(!ioData->mBuffers[outputBufferIndex].mDataByteSize && ioActionFlags)
    *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
  
  return noErr;
}
OSStatus CCoreAudioAEStream::Render(AudioUnitRenderActionFlags* actionFlags, const AudioTimeStamp* pTimeStamp, UInt32 busNumber, UInt32 frameCount, AudioBufferList* pBufList)
{
  
  OSStatus ret = noErr;
  
  ret = OnRender(actionFlags, pTimeStamp, busNumber, frameCount, pBufList);

  return ret;  
}

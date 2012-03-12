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

#include <string.h>

#include "system.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "utils/MathUtils.h"
#include "threads/SingleLock.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "DllAvCore.h"

#include "SoftAE.h"
#include "SoftAESound.h"
#include "SoftAEStream.h"
#include "AESinkFactory.h"
#include "Interfaces/AESink.h"
#include "Utils/AEUtil.h"
#include "Encoders/AEEncoderFFmpeg.h"

using namespace std;

CSoftAE::CSoftAE():
  m_thread             (NULL ),
  m_audiophile         (true ),
  m_running            (false),
  m_reOpen             (false),
  m_reOpened           (false),
  m_delay              (0    ),
  m_sink               (NULL ),
  m_transcode          (false),
  m_rawPassthrough     (false),
  m_bufferSize         (0    ),
  m_buffer             (NULL ),
  m_encoder            (NULL ),
  m_encodedBuffer      (NULL ),
  m_encodedBufferSize  (0    ),
  m_encodedBufferPos   (0    ),
  m_encodedBufferFrames(0    ),
  m_remapped           (NULL ),
  m_remappedSize       (0    ),
  m_converted          (NULL ),
  m_convertedSize      (0    ),
  m_masterStream       (NULL ),
  m_outputStageFn      (NULL ),
  m_streamStageFn      (NULL )
{

}

CSoftAE::~CSoftAE()
{
  Deinitialize();

  /* free the streams */
  CSingleLock streamLock(m_streamLock);
  while(!m_streams.empty())
  {
    CSoftAEStream *s = m_streams.front();
    delete s;
  }

  /* free the sounds */
  CSingleLock soundLock(m_soundLock);
  while(!m_sounds.empty())
  {
    CSoftAESound *s = m_sounds.front();
    m_sounds.pop_front();
    delete s;
  }
}

IAESink *CSoftAE::GetSink(AEAudioFormat &newFormat, bool passthrough, CStdString &device)
{
  device = passthrough ? m_passthroughDevice : m_device;

  /* if we are raw, force the sample rate */
  if (AE_IS_RAW(newFormat.m_dataFormat))
  {
    switch(newFormat.m_dataFormat)
    {
        case AE_FMT_AC3:
        case AE_FMT_DTS:
          break;

        case AE_FMT_EAC3:
          newFormat.m_sampleRate = 192000;
          break;

        case AE_FMT_TRUEHD:
        case AE_FMT_DTSHD:
          newFormat.m_sampleRate = 192000;
          break;

        default:
          break;
    }
  }

  IAESink *sink = CAESinkFactory::Create(device, newFormat, passthrough);
  return sink;
}

/* this method MUST be called while holding m_streamLock */
inline CSoftAEStream *CSoftAE::GetMasterStream()
{
  /* if there are no streams, just return NULL */
  if (m_streams.empty())
    return NULL;

  /* if we already know the master stream return it */
  if (m_masterStream && !m_masterStream->IsDestroyed())
    return m_masterStream;

  /* determine the master stream */
  m_masterStream = NULL;
  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end();)
  {
    CSoftAEStream *stream = *itt;

    /* remove any destroyed streams */
    if (stream->IsDestroyed())
    {
      RemoveStream(m_playingStreams, stream);
      RemoveStream(m_streams       , stream);
      delete stream;
      continue;
    }

    /* raw streams get priority */
    if (stream->IsRaw())
      m_masterStream = stream;

    if (!m_masterStream || !m_masterStream->IsRaw())
    {
      /* get the first/last stream in the list depending on audiophile setting */
      if (m_audiophile)
        m_masterStream = stream;
      else
        if (!m_masterStream)
          m_masterStream = stream;
    }

    ++itt;
  }

  return m_masterStream;
}

/* this must NEVER be called from outside the main thread or Initialization */
bool CSoftAE::OpenSink()
{
  /* save off our raw/passthrough mode for checking */
  bool wasTranscode           = m_transcode;
  bool wasRawPassthrough      = m_rawPassthrough;
  bool reInit                 = false;

  LoadSettings();

  /* initialize for analog output */
  m_rawPassthrough = false;
  m_streamStageFn = &CSoftAE::RunStreamStage;
  m_outputStageFn = &CSoftAE::RunOutputStage;

  /* initialize the new format for basic 2.0 output */
  AEAudioFormat newFormat;
  newFormat.m_dataFormat    = AE_FMT_FLOAT;
  newFormat.m_sampleRate    = 44100;
  newFormat.m_channelLayout = AE_CH_LAYOUT_2_0;

  CSingleLock streamLock(m_streamLock);
  CSoftAEStream *masterStream = GetMasterStream();
  if (masterStream)
  {
    /* choose the sample rate & channel layout based on the master stream */
    newFormat.m_sampleRate    = masterStream->GetSampleRate();
    newFormat.m_channelLayout = masterStream->m_initChannelLayout;    

    if (masterStream->IsRaw())
    {
      newFormat.m_dataFormat = masterStream->GetDataFormat();
      m_rawPassthrough       = true;
      m_streamStageFn        = &CSoftAE::RunRawStreamStage;
      m_outputStageFn        = &CSoftAE::RunRawOutputStage;
    }
    else
    {      
      if (!m_transcode)
        newFormat.m_channelLayout.ResolveChannels(m_stdChLayout);
    }
  }

  if (!m_rawPassthrough && m_transcode)
    newFormat.m_dataFormat = AE_FMT_AC3;

  streamLock.Leave();

  CStdString device, driver;
  if (m_transcode || m_rawPassthrough)
    device = m_passthroughDevice;
  else
    device = m_device;
  
  CAESinkFactory::ParseDevice(device, driver);
  if (driver.IsEmpty() && m_sink)
    driver = m_sink->GetName();

       if (m_rawPassthrough) CLog::Log(LOGINFO, "CSoftAE::OpenSink - RAW passthrough enabled");
  else if (m_transcode     ) CLog::Log(LOGINFO, "CSoftAE::OpenSink - Transcode passthrough enabled");

  /*
    try to use 48000hz if we are going to transcode, this prevents the sink
    from being re-opened repeatedly when switching sources, which locks up
    some receivers & crappy integrated sound drivers.
  */
  if (m_transcode && !m_rawPassthrough)
  {
    newFormat.m_sampleRate = 48000;
    m_outputStageFn = &CSoftAE::RunTranscodeStage;
  }

  /*
    if there is an audio resample rate set, use it, this MAY NOT be honoured as
    the audio sink may not support the requested format, and may change it.
  */
  if (g_advancedSettings.m_audioResample)
  {
    newFormat.m_sampleRate = g_advancedSettings.m_audioResample;
    CLog::Log(LOGINFO, "CSoftAE::OpenSink - Forcing samplerate to %d", newFormat.m_sampleRate);
  }

  /* only re-open the sink if its not compatible with what we need */
  if (!m_sink || ((CStdString)m_sink->GetName()).ToUpper() != driver || !m_sink->IsCompatible(newFormat, device))
  {
    CLog::Log(LOGINFO, "CSoftAE::OpenSink - sink incompatible, re-starting");

    /* let the thread know we have re-opened the sink */
    m_reOpened = true;
    reInit = true;
    
    /* we are going to open, so close the old sink if it was open */
    if (m_sink)
    {
      m_sink->Drain();
      m_sink->Deinitialize();
      delete m_sink;
      m_sink = NULL;
    }

    /* if we already have a driver, prepend it to the device string */
    if (!driver.IsEmpty())
      device = driver + ":" + device;
    
    /* create the new sink */
    m_sink = GetSink(newFormat, m_transcode || m_rawPassthrough, device);

    CLog::Log(LOGINFO, "CSoftAE::OpenSink - %s Initialized:", m_sink->GetName());
    CLog::Log(LOGINFO, "  Output Device : %s", device.c_str());
    CLog::Log(LOGINFO, "  Sample Rate   : %d", newFormat.m_sampleRate);
    CLog::Log(LOGINFO, "  Sample Format : %s", CAEUtil::DataFormatToStr(newFormat.m_dataFormat));
    CLog::Log(LOGINFO, "  Channel Count : %d", newFormat.m_channelLayout.Count());
    CLog::Log(LOGINFO, "  Channel Layout: %s", ((CStdString)newFormat.m_channelLayout).c_str());
    CLog::Log(LOGINFO, "  Frames        : %d", newFormat.m_frames);
    CLog::Log(LOGINFO, "  Frame Samples : %d", newFormat.m_frameSamples);
    CLog::Log(LOGINFO, "  Frame Size    : %d", newFormat.m_frameSize);

    m_sinkFormat = newFormat;

    /* invalidate the buffer */
    m_bufferSamples = 0;
  }
  else
    CLog::Log(LOGINFO, "CSoftAE::OpenSink - keeping old sink");

  size_t neededBufferSize = 0;
  if (m_rawPassthrough)
  {
    if (!wasRawPassthrough)
    {
      /* invalidate the buffer */
      m_bufferSamples = 0;
    }
    
    reInit = (reInit || m_chLayout != m_sinkFormat.m_channelLayout);
    m_chLayout       = m_sinkFormat.m_channelLayout;
    m_convertFn      = NULL;
    m_bytesPerSample = CAEUtil::DataFormatToBits(m_sinkFormat.m_dataFormat) >> 3;  
    m_frameSize      = m_sinkFormat.m_frameSize;
    neededBufferSize = m_sinkFormat.m_frames * m_sinkFormat.m_frameSize;
  }
  else
  {
    reInit = (reInit || m_chLayout != m_sinkFormat.m_channelLayout);
    m_chLayout = m_sinkFormat.m_channelLayout;

    /* if we are transcoding */
    if (m_transcode)
    {
      if (!wasTranscode || wasRawPassthrough)
      {
        /* invalidate the buffer */
        m_bufferSamples = 0;
        if (m_encoder)
          m_encoder->Reset();
      }

      /* configure the encoder */
      AEAudioFormat encoderFormat;
      encoderFormat.m_sampleRate    = m_sinkFormat.m_sampleRate;
      encoderFormat.m_dataFormat    = AE_FMT_FLOAT;
      encoderFormat.m_channelLayout = m_chLayout;
      if (!m_encoder || !m_encoder->IsCompatible(encoderFormat))
      {
        m_bufferSamples = 0;
        SetupEncoder(encoderFormat);
        m_encoderFormat = encoderFormat;
      }
      
      /* remap directly to the format we need for encode */
      reInit = (reInit || m_chLayout != m_encoderFormat.m_channelLayout);
      m_chLayout       = m_encoderFormat.m_channelLayout;
      m_convertFn      = CAEConvert::FrFloat(m_encoderFormat.m_dataFormat);
      neededBufferSize = m_encoderFormat.m_frames * sizeof(float) * m_chLayout.Count();
      
      CLog::Log(LOGDEBUG, "CSoftAE::Initialize - Encoding using layout: %s", ((CStdString)m_chLayout).c_str());
    }
    else
    {
      m_convertFn      = CAEConvert::FrFloat(m_sinkFormat.m_dataFormat);
      neededBufferSize = m_sinkFormat.m_frames * sizeof(float) * m_chLayout.Count();      
      CLog::Log(LOGDEBUG, "CSoftAE::Initialize - Using speaker layout: %s", CAEUtil::GetStdChLayoutName(m_stdChLayout));
    }

    m_bytesPerSample = CAEUtil::DataFormatToBits(AE_FMT_FLOAT) >> 3;
    m_frameSize      = m_bytesPerSample * m_chLayout.Count();
  }

  if (m_bufferSize < neededBufferSize)
  {
    m_bufferSamples = 0;
    _aligned_free(m_buffer);
    m_buffer = _aligned_malloc(neededBufferSize, 16);
    m_bufferSize = neededBufferSize;
  }

  m_remap.Initialize(m_chLayout, m_sinkFormat.m_channelLayout, true);
  
  /* if we in raw passthrough, we are finished */
  if (m_rawPassthrough)
    return true;

  if (reInit)
  {
    /* re-init sounds */
    CSingleLock soundLock(m_soundLock);
    StopAllSounds();
    for(SoundList::iterator itt = m_sounds.begin(); itt != m_sounds.end(); ++itt)
      (*itt)->Initialize();

    /* re-init streams */
    streamLock.Enter();
    for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
      (*itt)->Initialize();
    streamLock.Leave();
  }
  
  return (m_sink != NULL);
}

void CSoftAE::ResetEncoder()
{
  if (m_encoder)
    m_encoder->Reset();

  if (m_encodedBuffer)
    free(m_encodedBuffer);

  m_encodedBuffer       = NULL;
  m_encodedBufferSize   = 0;
  m_encodedBufferPos    = 0;
  m_encodedBufferFrames = 0;
}

bool CSoftAE::SetupEncoder(AEAudioFormat &format)
{
  ResetEncoder();
  delete m_encoder;
  m_encoder = NULL;

  if (!m_transcode)
    return false;

  m_encoder = new CAEEncoderFFmpeg();
  if (m_encoder->Initialize(format))
    return true;

  delete m_encoder;
  m_encoder = NULL;
  return false;
}

void CSoftAE::Shutdown()
{
  Deinitialize();
}

bool CSoftAE::Initialize()
{
  /* get the current volume level */
  m_volume = g_settings.m_fVolumeLevel;

  /* we start even if we failed to open a sink */
  OpenSink();
  m_running = true;
  m_thread  = new CThread(this);
  m_thread->Create();
  m_thread->SetPriority(THREAD_PRIORITY_ABOVE_NORMAL);

  return true;
}

void CSoftAE::OnSettingsChange(CStdString setting)
{
  if (setting == "audiooutput.dontnormalizelevels")
  {
    /* re-init streams reampper */
    CSingleLock streamLock(m_streamLock);
    for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
      (*itt)->InitializeRemap();
  }

  if (setting == "audiooutput.passthroughdevice" ||
      setting == "audiooutput.custompassthrough" ||
      setting == "audiooutput.audiodevice"       ||
      setting == "audiooutput.customdevice"      ||
      setting == "audiooutput.mode"              ||
      setting == "audiooutput.ac3passthrough"    ||
      setting == "audiooutput.dtspassthrough"    ||
      setting == "audiooutput.channellayout"     ||
      setting == "audiooutput.useexclusivemode"  ||
      setting == "audiooutput.multichannellpcm")
  {
    m_reOpen = true;
  }
}

void CSoftAE::LoadSettings()
{
  m_audiophile = g_advancedSettings.m_audioAudiophile;
  if (m_audiophile)
    CLog::Log(LOGINFO, "CSoftAE::LoadSettings - Audiophile switch enabled");

  /* load the configuration */
  m_stdChLayout = AE_CH_LAYOUT_2_0;
  switch(g_guiSettings.GetInt("audiooutput.channellayout"))
  {
    default:
    case  0: m_stdChLayout = AE_CH_LAYOUT_2_0; break; /* dont alow 1_0 output */
    case  1: m_stdChLayout = AE_CH_LAYOUT_2_0; break;
    case  2: m_stdChLayout = AE_CH_LAYOUT_2_1; break;
    case  3: m_stdChLayout = AE_CH_LAYOUT_3_0; break;
    case  4: m_stdChLayout = AE_CH_LAYOUT_3_1; break;
    case  5: m_stdChLayout = AE_CH_LAYOUT_4_0; break;
    case  6: m_stdChLayout = AE_CH_LAYOUT_4_1; break;
    case  7: m_stdChLayout = AE_CH_LAYOUT_5_0; break;
    case  8: m_stdChLayout = AE_CH_LAYOUT_5_1; break;
    case  9: m_stdChLayout = AE_CH_LAYOUT_7_0; break;
    case 10: m_stdChLayout = AE_CH_LAYOUT_7_1; break;
  }

#if defined(_WIN32)
  m_passthroughDevice = g_guiSettings.GetString("audiooutput.audiodevice");
#else
  m_passthroughDevice = g_guiSettings.GetString("audiooutput.passthroughdevice");
  if (m_passthroughDevice == "custom")
    m_passthroughDevice = g_guiSettings.GetString("audiooutput.custompassthrough");

  if (m_passthroughDevice.IsEmpty())
    m_passthroughDevice = g_guiSettings.GetString("audiooutput.audiodevice");

  if (m_passthroughDevice.IsEmpty())
    m_passthroughDevice = "default";
#endif

  m_device = g_guiSettings.GetString("audiooutput.audiodevice");
  if (m_device == "custom")
    m_device = g_guiSettings.GetString("audiooutput.customdevice");

  if (m_device.IsEmpty())
    m_device = "default";

  m_transcode = (
    g_guiSettings.GetBool("audiooutput.ac3passthrough") /*||
    g_guiSettings.GetBool("audiooutput.dtspassthrough") */
  ) && (
      (g_guiSettings.GetInt("audiooutput.mode") == AUDIO_IEC958) ||
      (g_guiSettings.GetInt("audiooutput.mode") == AUDIO_HDMI && !g_guiSettings.GetBool("audiooutput.multichannellpcm"))
  );
}

void CSoftAE::Deinitialize()
{
  if (m_thread)
  {
    Stop();
    m_thread->StopThread(true);
    delete m_thread;
    m_thread = NULL;
  }

  if (m_sink)
  {
    /* shutdown the sink */
    m_sink->Deinitialize();
    delete m_sink;
    m_sink = NULL;
  }

  delete m_encoder;
  m_encoder = NULL;

  ResetEncoder();

  _aligned_free(m_buffer);
  m_buffer = NULL;

  _aligned_free(m_converted);
  m_converted = NULL;
  m_convertedSize = 0;

  _aligned_free(m_remapped);
  m_remapped = NULL;
  m_remappedSize = 0;
}

void CSoftAE::EnumerateOutputDevices(AEDeviceList &devices, bool passthrough)
{
  CAESinkFactory::Enumerate(devices, passthrough);
}

bool CSoftAE::SupportsRaw()
{
  /* CSoftAE supports raw formats */
  return true;
}

void CSoftAE::PauseStream(CSoftAEStream *stream)
{  
  CSingleLock streamLock(m_streamLock);
  RemoveStream(m_playingStreams, stream);
}

void CSoftAE::ResumeStream(CSoftAEStream *stream)
{
  CSingleLock streamLock(m_streamLock);
  m_playingStreams.push_back(stream);
  streamLock.Leave();

  m_reOpen = true;
}

void CSoftAE::Stop()
{
  m_running = false;

  /* wait for the thread to stop */
  CSingleLock lock(m_runningLock);
}

IAEStream *CSoftAE::MakeStream(enum AEDataFormat dataFormat, unsigned int sampleRate, CAEChannelInfo channelLayout, unsigned int options/* = 0 */)
{
  CAEChannelInfo channelInfo(channelLayout);
  CLog::Log(LOGINFO, "CSoftAE::MakeStream - %s, %u, %s",
    CAEUtil::DataFormatToStr(dataFormat),
    sampleRate,
    ((CStdString)channelInfo).c_str()
  );

  CSingleLock streamLock(m_streamLock);
  CSoftAEStream *stream = new CSoftAEStream(dataFormat, sampleRate, channelLayout, options);

  bool wasEmpty = false;
  if (m_streams.empty())
  {
    wasEmpty       = true;
    m_masterStream = stream;
  }  

  m_streams.push_back(stream);
  if ((options & AESTREAM_PAUSED) == 0)
    m_playingStreams.push_back(stream);

  streamLock.Leave();

  /* dont re-open if the new stream is paused */
  if ((options & AESTREAM_PAUSED) == 0)
  {
    if ((AE_IS_RAW(dataFormat)) || wasEmpty || m_audiophile)
      m_reOpen = true;
  }

  /* if the stream was not initialized, do it now */
  if (!stream->IsValid())
    stream->Initialize();

  return stream;
}

IAESound *CSoftAE::MakeSound(CStdString file)
{
  CSingleLock soundLock(m_soundLock);

  CSoftAESound *sound = new CSoftAESound(file);
  if (!sound->Initialize())
  {
    delete sound;
    return NULL;
  }

  m_sounds.push_back(sound);
  return sound;
}

void CSoftAE::PlaySound(IAESound *sound)
{
   float *samples = ((CSoftAESound*)sound)->GetSamples();
   if (!samples)
     return;

   /* add the sound to the play list */
   CSingleLock soundSampleLock(m_soundSampleLock);
   SoundState ss = {
      ((CSoftAESound*)sound),
      samples,
      ((CSoftAESound*)sound)->GetSampleCount()
   };
   m_playing_sounds.push_back(ss);
}

void CSoftAE::FreeSound(IAESound *sound)
{
  if (!sound) return;

  sound->Stop();
  CSingleLock soundLock(m_soundLock);
  for(SoundList::iterator itt = m_sounds.begin(); itt != m_sounds.end(); ++itt)
    if (*itt == sound)
    {
      m_sounds.erase(itt);
      break;
    }

  delete (CSoftAESound*)sound;
}

void CSoftAE::GarbageCollect()
{
}

unsigned int CSoftAE::GetSampleRate()
{
  if (m_transcode && m_encoder && !m_rawPassthrough)
    return m_encoderFormat.m_sampleRate;
  
  return m_sinkFormat.m_sampleRate;
}

void CSoftAE::StopSound(IAESound *sound)
{
  CSingleLock lock(m_soundSampleLock);
  for(SoundStateList::iterator itt = m_playing_sounds.begin(); itt != m_playing_sounds.end(); )
  {
    if ((*itt).owner == sound)
    {
      (*itt).owner->ReleaseSamples();
      itt = m_playing_sounds.erase(itt);
    }
    else ++itt;
  }
}

IAEStream *CSoftAE::FreeStream(IAEStream *stream)
{
  CSingleLock lock(m_streamLock);

  RemoveStream(m_playingStreams, (CSoftAEStream*)stream);
  RemoveStream(m_streams       , (CSoftAEStream*)stream);
  delete (CSoftAEStream*)stream;

  /* if it was the master stream */
  if (m_masterStream == stream)
  {
    m_masterStream = NULL;
    GetMasterStream();
  }

  /* re-open the sink if required */
  m_reOpen = true;

  return NULL;
}

float CSoftAE::GetDelay()
{
  if (!m_running)
    return 0.0f;

  return m_delay;
}

float CSoftAE::GetVolume()
{
  return m_volume;
}

void CSoftAE::SetVolume(float volume)
{
  g_settings.m_fVolumeLevel = volume;
  m_volume = volume;
}

void CSoftAE::StopAllSounds()
{
  CSingleLock lock(m_soundSampleLock);
  while(!m_playing_sounds.empty())
  {
    SoundState *ss = &(*m_playing_sounds.begin());
    ss->owner->ReleaseSamples();
    m_playing_sounds.pop_front();
  }
}

void CSoftAE::Run()
{
  /* we release this when we exit the thread unblocking anyone waiting on "Stop" */
  CSingleLock runningLock(m_runningLock);

  uint8_t *out = NULL;
  size_t   outSize = 0;

  CLog::Log(LOGINFO, "CSoftAE::Run - Thread Started");


  while(m_running)
  {
    m_reOpened = false;

    /* output the buffer to the sink */
    (this->*m_outputStageFn)();

    /* make sure we have enough room to fetch a frame */
    if(m_frameSize > outSize)
    {
      /* allocate space for the samples */
      _aligned_free(out);
      out = (uint8_t *)_aligned_malloc(m_frameSize, 16);
      outSize = m_frameSize;
    }
    memset(out, 0, m_frameSize);

    /* run the stream stage */
    bool restart = false;
    CSoftAEStream *oldMaster = m_masterStream;
    unsigned int mixed = (this->*m_streamStageFn)(m_chLayout.Count(), out, restart);
 
    /* if in audiophile mode and the master stream has changed, flag for restart */
    if (m_audiophile && oldMaster != m_masterStream)
      restart = true;

    /* if we are told to restart */
    if (m_reOpen || restart)
    {
      CLog::Log(LOGDEBUG, "CSoftAE::Run - Sink restart flagged");
      m_reOpen = false;
      OpenSink();
    }

    if(!m_reOpened)
    {
      if (!m_rawPassthrough && mixed)
        RunNormalizeStage(m_chLayout.Count(), out, mixed);
    }

    /* buffer the samples into the output buffer */
    if (!m_reOpened)
      RunBufferStage(out);

    /* update the current delay */
    m_delay = m_sink->GetDelay();
    if (m_transcode && m_encoder && !m_rawPassthrough)
      m_delay += m_encoder->GetDelay(m_encodedBufferFrames - m_encodedBufferPos);
    unsigned int buffered = m_bufferSamples / m_chLayout.Count();
    m_delay += (float)buffered / (float)m_sinkFormat.m_sampleRate;
  }

  /* free the frame storage */
  if(out)
    _aligned_free(out);
}

void CSoftAE::MixSounds(float *buffer, unsigned int samples)
{
  SoundStateList::iterator itt;

  CSingleLock lock(m_soundSampleLock);
  for(itt = m_playing_sounds.begin(); itt != m_playing_sounds.end(); )
  {
    SoundState *ss = &(*itt);

    /* no more frames, so remove it from the list */
    if (ss->sampleCount == 0)
    {
      ss->owner->ReleaseSamples();
      itt = m_playing_sounds.erase(itt);
      continue;
    }

    float volume = ss->owner->GetVolume();
    unsigned int mixSamples = std::min(ss->sampleCount, samples);

    #ifdef __SSE__
      CAEUtil::SSEMulAddArray(buffer, ss->samples, volume, mixSamples);
    #else
      for(unsigned int i = 0; i < mixSamples; ++i)
        buffer[i] = (buffer[i] + (ss->samples[i] * volume));
    #endif

    ss->sampleCount -= mixSamples;
    ss->samples     += mixSamples;

    ++itt;
  }
}

void CSoftAE::FinalizeSamples(float *buffer, unsigned int samples)
{
  MixSounds(buffer, samples);

  #ifdef __SSE__
    CAEUtil::SSEMulClampArray(buffer, m_volume, samples);
  #else
    for(unsigned int i = 0; i < samples; ++i)
      buffer[i] = CAEUtil::SoftClamp(buffer[i] * m_volume);
  #endif
}

void CSoftAE::RunOutputStage()
{
  const unsigned int rSamples = m_sinkFormat.m_frames * m_sinkFormat.m_channelLayout.Count();
  const unsigned int samples  = m_sinkFormat.m_frames * m_chLayout.Count();

  /* this normally only loops once */
  while(m_bufferSamples >= samples)
  {
    int wroteFrames;

    if(m_remappedSize < rSamples)
    {
      _aligned_free(m_remapped);
      m_remapped = (float *)_aligned_malloc(rSamples * sizeof(float), 16);
      m_remappedSize = rSamples;
    }

    m_remap.Remap((float *)m_buffer, m_remapped, m_sinkFormat.m_frames);
    FinalizeSamples(m_remapped, rSamples);

    if (m_convertFn)
    {
      unsigned int newSize = m_sinkFormat.m_frames * m_sinkFormat.m_frameSize;
      if(m_convertedSize < newSize)
      {
        _aligned_free(m_converted);
        m_converted = (uint8_t *)_aligned_malloc(newSize, 16);
        m_convertedSize = newSize;
      }
      m_convertFn(m_remapped, rSamples, m_converted);
      wroteFrames = m_sink->AddPackets(m_converted, m_sinkFormat.m_frames);
    }
    else
    {
      wroteFrames = m_sink->AddPackets((uint8_t*)m_remapped, m_sinkFormat.m_frames);
    }

    int wroteSamples = wroteFrames * m_chLayout.Count();
    int bytesLeft    = (m_bufferSamples - wroteSamples) * m_bytesPerSample;
    memmove((float*)m_buffer, (float*)m_buffer + wroteSamples, bytesLeft);
    m_bufferSamples -= wroteSamples;
  }
}

void CSoftAE::RunRawOutputStage()
{
  unsigned int samples = m_sinkFormat.m_frames * m_sinkFormat.m_channelLayout.Count();

  /* this normally only loops once */
  while(m_bufferSamples >= samples)
  {
    int wroteFrames;
    uint8_t *rawBuffer = (uint8_t*)m_buffer;
    wroteFrames = m_sink->AddPackets(rawBuffer, m_sinkFormat.m_frames);

    int wroteSamples = wroteFrames * m_sinkFormat.m_channelLayout.Count();;
    int bytesLeft    = (m_bufferSamples - wroteSamples) * m_bytesPerSample;
    memmove(rawBuffer, rawBuffer + (wroteSamples * m_bytesPerSample), bytesLeft);
    m_bufferSamples -= wroteSamples;
  }
}

void CSoftAE::RunTranscodeStage()
{
  /* if we dont have enough samples to encode yet, return */
  if (m_bufferSamples >= m_encoderFormat.m_frameSamples && m_encodedBufferFrames < m_sinkFormat.m_frames * 2)
  {
    FinalizeSamples((float*)m_buffer, m_encoderFormat.m_frameSamples);    

    void *buffer;
    if (m_convertFn)
    {
      unsigned int newsize = m_encoderFormat.m_frames * m_encoderFormat.m_frameSize;
      if(m_convertedSize < newsize)
      {
        _aligned_free(m_converted);
        m_converted     = (uint8_t *)_aligned_malloc(newsize, 16);
        m_convertedSize = newsize;
      }
      m_convertFn((float*)m_buffer, m_encoderFormat.m_frames * m_encoderFormat.m_channelLayout.Count(), m_converted);
      buffer = m_converted;
    }
    else
      buffer = m_buffer;

    int encodedFrames  = m_encoder->Encode((float*)buffer, m_encoderFormat.m_frames);
    int encodedSamples = encodedFrames * m_encoderFormat.m_channelLayout.Count();
    int bytesLeft      = (m_bufferSamples - encodedSamples) * m_bytesPerSample;

    memmove((float*)m_buffer, ((float*)m_buffer) + encodedSamples, bytesLeft);
    m_bufferSamples -= encodedSamples;

    uint8_t *packet;
    unsigned int size = m_encoder->GetData(&packet);

    /* if there is not enough space for another encoded packet enlarge the buffer */
    if (m_encodedBufferSize - m_encodedBufferPos < size)
    {
      m_encodedBufferSize += size - (m_encodedBufferSize - m_encodedBufferPos);
      m_encodedBuffer     = (uint8_t*)realloc(m_encodedBuffer, m_encodedBufferSize);
    }

    memcpy(m_encodedBuffer + m_encodedBufferPos, packet, size);
    m_encodedBufferPos    += size;
    m_encodedBufferFrames += size / m_sinkFormat.m_frameSize;
  }

  /* if we have enough data to write */
  if(m_encodedBufferFrames >= m_sinkFormat.m_frames)
  {
    unsigned int wrote;
        
    wrote = m_sink->AddPackets(m_encodedBuffer, m_sinkFormat.m_frames);
    m_encodedBufferPos    -= wrote * m_sinkFormat.m_frameSize;
    m_encodedBufferFrames -= wrote;
    memmove(m_encodedBuffer, m_encodedBuffer + wrote * m_sinkFormat.m_frameSize, m_encodedBufferPos);
  }
}

unsigned int CSoftAE::RunRawStreamStage(unsigned int channelCount, void *out, bool &restart)
{
  StreamList resumeStreams;
  static StreamList::iterator itt;

  /* identify the masterStream */
  CSingleLock streamLock(m_streamLock);
  CSoftAEStream *masterStream = GetMasterStream();

  /* if the master is not RAW anymore flag for restart */
  if (!masterStream || !masterStream->IsRaw())
  {
    restart = true;
    return 0;
  }

  /* handle playing streams */
  for(itt = m_playingStreams.begin(); itt != m_playingStreams.end(); ++itt)
  {
    CSoftAEStream *sitt = *itt;
    if (sitt == masterStream)
      continue;

    /* consume data from streams even though we cant use it */
    sitt->GetFrame();

    /* flag the stream's slave to be resumed if it has drained */
    if (sitt->IsDrained() && sitt->m_slave && sitt->m_slave->m_paused)
      resumeStreams.push_back(sitt);
  }

  /* get the frame and append it to the output */
  uint8_t *frame = masterStream->GetFrame();
  if (!frame)
  {
    ResumeStreams(resumeStreams);
    return 0;
  }

  memcpy(out, frame, m_sinkFormat.m_frameSize);

  if (masterStream->IsDrained() && masterStream->m_slave && masterStream->m_slave->m_paused)
    resumeStreams.push_back(masterStream);

  ResumeStreams(resumeStreams);
  return 1;
}

unsigned int CSoftAE::RunStreamStage(unsigned int channelCount, void *out, bool &restart)
{
  StreamList resumeStreams;
  static StreamList::iterator itt;

  float *dst = (float*)out;
  unsigned int mixed = 0;

  /* identify the master stream */
  CSingleLock streamLock(m_streamLock);
  CSoftAEStream *masterStream = GetMasterStream();

  /* if the master stream is now RAW, flag for restart */
  if (masterStream && masterStream->IsRaw())
  {
    restart = true;
    return 0;
  }

  /* mix in any running streams */
  for(itt = m_playingStreams.begin(); itt != m_playingStreams.end(); ++itt)
  {
    CSoftAEStream *stream = *itt;

    float *frame = (float*)stream->GetFrame();
    if (!frame)
      continue;

    if (stream->IsDrained() && stream->m_slave && stream->m_slave->IsPaused())
      resumeStreams.push_back(stream);

    float volume = stream->GetVolume() * stream->GetReplayGain();
    #ifdef __SSE__
    if (channelCount > 1)
      CAEUtil::SSEMulAddArray(dst, frame, volume, channelCount);
    else
    #endif
    {
      for(unsigned int i = 0; i < channelCount; ++i)
        dst[i] += frame[i] * volume;
    }

    ++mixed;
  }

  ResumeStreams(resumeStreams);
  return mixed;
}

inline void CSoftAE::ResumeStreams(StreamList &streams)
{
  /* resume any streams that need to be */
  for(StreamList::iterator itt = streams.begin(); itt != streams.end(); ++itt)
  {
    CSoftAEStream *stream = *itt;
    m_playingStreams.push_back(stream->m_slave);
    stream->m_slave->m_paused = false;
    stream->m_slave = NULL;
  }
}

inline void CSoftAE::RunNormalizeStage(unsigned int channelCount, void *out, unsigned int mixed)
{
  return;
  if (mixed <= 0) return;

  float *dst = (float*)out;
  float mul = 1.0f / mixed;
  #ifdef __SSE__
  if (channelCount > 1)
    CAEUtil::SSEMulArray(dst, mul, channelCount);
  else
  #endif
  {
    for(unsigned int i = 0; i < channelCount; ++i)
      dst[i] *= mul;
  }
}

inline void CSoftAE::RunBufferStage(void *out)
{
  if (m_rawPassthrough)
  {
    /* check for buffer overflow */
    ASSERT(m_bufferSize - m_bufferSamples * m_bytesPerSample >= m_sinkFormat.m_frameSize);

    uint8_t *rawBuffer = (uint8_t*)m_buffer;
    memcpy(rawBuffer + (m_bufferSamples * m_bytesPerSample), out, m_sinkFormat.m_frameSize);
  }
  else
  {
    /* check for buffer overflow */
    ASSERT(m_bufferSize - m_bufferSamples * sizeof(float) >= m_frameSize);

    float *floatBuffer = (float*)m_buffer;
    memcpy(floatBuffer + m_bufferSamples, out, m_frameSize);
  }

  m_bufferSamples += m_chLayout.Count();
}

inline void CSoftAE::RemoveStream(StreamList &streams, CSoftAEStream *stream)
{
  StreamList::iterator f = std::find(streams.begin(), streams.end(), stream);
  if (f != streams.end())
    streams.erase(f);
}


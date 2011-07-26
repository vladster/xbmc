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

#include "AESound.h"

#include <samplerate.h>
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/EndianSwap.h"

#include "AEFactory.h"
#include "AEAudioFormat.h"
#include "AEConvert.h"
#include "AERemap.h"
#include "AEUtil.h"
#include "DllAvCore.h"

#include "CoreAudioAE.h"
#include "CoreAudioAESound.h"

/* typecast AE to CCoreAudioAE */
#define AE (*(CCoreAudioAE*)CAEFactory::AE)

typedef struct
{
  char     chunk_id[4];
  uint32_t chunksize;
} WAVE_CHUNK;

CCoreAudioAESound::CCoreAudioAESound(const CStdString &filename) :
  IAESound         (filename),
  m_volume         (1.0f    ),
  m_inUse          (0       )
{
  m_filename = filename;
}

CCoreAudioAESound::~CCoreAudioAESound()
{
  DeInitialize();
}


CStdString CCoreAudioAESound::GetFileName()
{
  return m_filename;
}

void CCoreAudioAESound::DeInitialize()
{
  m_wavLoader.DeInitialize();
}

bool CCoreAudioAESound::Initialize(AEAudioFormat &outputFormat)
{
  
  DeInitialize();
  
  int sampleRate = outputFormat.m_sampleRate;

  if (!m_wavLoader.Initialize(m_filename, sampleRate))
    return false;

  return m_wavLoader.Remap(AE.GetChannelLayout());
}

void CCoreAudioAESound::SetVolume(float volume)
{ 
  m_volume = std::max(0.0f, std::min(1.0f, volume));
}

float CCoreAudioAESound::GetVolume()
{
  return m_volume;
}

unsigned int CCoreAudioAESound::GetSampleCount()
{
  m_sampleLock.lock_shared();
  int sampleCount = 0;
  if (m_wavLoader.IsValid())
    sampleCount = m_wavLoader.GetSampleCount();
  m_sampleLock.unlock_shared();
  return sampleCount;
}

float* CCoreAudioAESound::GetSamples()
{
  m_sampleLock.lock_shared();
  if (!m_wavLoader.IsValid())
  {
    m_sampleLock.unlock_shared();
    return NULL;
  }

  ++m_inUse;
  
  return m_wavLoader.GetSamples();
}

void CCoreAudioAESound::Lock()
{
  m_sampleLock.lock();
}

void CCoreAudioAESound::UnLock()
{
  m_sampleLock.unlock();
}

void CCoreAudioAESound::ReleaseSamples()
{
  --m_inUse;
  m_sampleLock.unlock_shared();
}

bool CCoreAudioAESound::IsPlaying()
{
  m_sampleLock.lock_shared();
  bool playing = m_inUse > 0;
  m_sampleLock.unlock_shared();

  return playing;
}

void CCoreAudioAESound::Play()
{
  AE.PlaySound(this);
}

void CCoreAudioAESound::Stop()
{
  AE.StopSound(this);
}

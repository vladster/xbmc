#pragma once
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

#include "Interfaces/AESink.h"
#include <stdint.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>

#include "threads/CriticalSection.h"

class CAESinkWASAPI : public IAESink
{
public:
  virtual const char *GetName() { return "WASAPI"; }

  CAESinkWASAPI();
  virtual ~CAESinkWASAPI();

  virtual bool Initialize  (AEAudioFormat &format, CStdString &device);
  virtual void Deinitialize();
  virtual bool IsCompatible(const AEAudioFormat format, const CStdString device);

  virtual void         Stop             ();
  virtual float        GetDelay         ();
  virtual unsigned int AddPackets       (uint8_t *data, unsigned int frames);
  static  void         EnumerateDevices (AEDeviceList &devices, bool passthrough);
private:
  bool         InitializeShared(AEAudioFormat &format);
  bool         InitializeExclusive(AEAudioFormat &format);
  void         AEChannelsFromSpeakerMask(DWORD speakers);
  DWORD        SpeakerMaskFromAEChannels(const CAEChannelInfo &channels);
  void         BuildWaveFormatExtensible(AEAudioFormat &format, WAVEFORMATEXTENSIBLE &wfxex);
  void         BuildWaveFormatExtensibleIEC61397(AEAudioFormat &format, WAVEFORMATEXTENSIBLE_IEC61937 &wfxex);

  static const char  *WASAPIErrToStr(HRESULT err);

  IMMDevice          *m_pDevice;
  IAudioClient       *m_pAudioClient;
  IAudioRenderClient *m_pRenderClient;

  AEAudioFormat       m_format;
  enum AEDataFormat   m_encodedFormat;
  unsigned int        m_encodedChannels;
  unsigned int        m_encodedSampleRate;
  CAEChannelInfo      m_channelLayout;
  CStdString          m_device;

  bool                m_running;
  bool                m_initialized;
  bool                m_isExclusive;
  CCriticalSection    m_runLock;

  unsigned int        m_uiBufferLen; // wasapi endpoint buffer size, in frames
};

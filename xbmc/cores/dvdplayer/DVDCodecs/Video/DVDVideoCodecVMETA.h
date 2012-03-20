/*
 *      Copyright (C) 2012 Team XBMC
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

#pragma once

#if defined(HAS_MARVELL_DOVE)
#include "DVDVideoCodec.h"

#include "DllVMETA.h"

#include <queue>
#include <vector>

#define ALIGN(x, n) (((x) + (n) - 1) & (~((n) - 1)))

class CBitstreamConverter;
class DllLibMiscGen;
class DllLibVMETA;

class CDVDVideoCodecVMETA : public CDVDVideoCodec
{
public:
  CDVDVideoCodecVMETA();
  ~CDVDVideoCodecVMETA();

  // Required overrides
  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  void Dispose(void);
  IppCodecStatus SendCodecConfig();
  IppCodecStatus DecodeInternal(uint8_t *pData, unsigned int *iSize, double dts, double pts);
  int  DecodeInternal(uint8_t *demuxer_content, int demuxer_bytes);
  int  Decode(uint8_t *pData, int iSize, double dts, double pts);
  void Reset(void);
  void SetDropState(bool bDrop);
  bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  bool ClearPicture(DVDVideoPicture* pDvdVideoPicture);
  int  GetFrameCount() { return m_Frames; };
  const char* GetName() { return m_video_codec_name.c_str(); };
protected:
  // Video format
  bool                            m_drop_state;
  unsigned int                    m_decoded_width;
  unsigned int                    m_decoded_height;
  unsigned int                    m_picture_width;
  unsigned int                    m_picture_height;
  bool                            m_is_open;
  bool                            m_Pause;
  bool                            m_setStartTime;
  uint8_t                         *m_extradata;
  int                             m_extrasize;
  CBitstreamConverter             *m_converter;
  bool                            m_video_convert;
  CStdString                      m_video_codec_name;

  MiscGeneralCallbackTable        *m_pCbTable;
  IppVmetaDecParSet               m_VDecParSet;
  IppVmetaDecInfo                 m_VDecInfo;
  void                            *m_pDecState;

  std::queue<IppVmetaBitstream*>  m_input_avaliable;
  std::vector<IppVmetaBitstream*> m_input_buffers;
  unsigned int m_input_size;

  std::queue<IppVmetaPicture*>    m_output_ready;
  std::queue<IppVmetaPicture*>    m_output_avaliable;
  std::vector<IppVmetaPicture*>   m_output_buffers;
  std::queue<double>              m_pts_queue;

  unsigned int                    m_Frames;

  DllLibMiscGen                   *m_DllMiscGen;
  DllLibVMETA                     *m_DllVMETA;
};
#endif

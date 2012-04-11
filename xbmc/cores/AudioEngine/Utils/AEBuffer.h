#pragma once
/*
 *      Copyright (C) 2005-2012 Team XBMC
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

#include "system.h"

/**
 * This class wraps a block of 16 byte aligned memory for simple buffer
 * operations, if _DEBUG is defined then size is always verified.
 */
class CAEBuffer
{
private:
  uint8_t *m_buffer;
  size_t   m_bufferSize;
  size_t   m_bufferPos;
  size_t   m_cursorPos;

public:
  CAEBuffer();
  ~CAEBuffer();

  /* initialize methods */
  void Alloc  (const size_t size);
  void ReAlloc(const size_t size);
  void DeAlloc();

  /* usage methods */
  inline size_t Size () { return m_bufferSize; }
  inline size_t Used () { return m_bufferPos ; }
  inline size_t Free () { return m_bufferSize - m_bufferPos; }
  inline void   Empty() { m_bufferPos = 0; }

  /* write methods */
  void Write  (const void *src, const size_t size);
  void Push   (const void *src, const size_t size);
  void UnShift(const void *src, const size_t size);

  /* raw methods */
  void* Raw(const size_t size);

  /* read methods */
  void  Read   (void *dst, const size_t size);
  void  Pop    (void *dst, const size_t size);
  void  Shift  (void *dst, const size_t size);

  /* cursor methods */
  inline void  CursorReset() { m_cursorPos = 0; }
  inline bool  CursorEnd  () { return m_cursorPos == m_bufferSize; }
  void         CursorSeek (const size_t pos );
  void*        CursorRead (const size_t size);
};

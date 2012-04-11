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

#include "AEBuffer.h"
#include "utils/StdString.h" /* needed for ASSERT */
#include <algorithm>

CAEBuffer::CAEBuffer() :
  m_buffer    (NULL),
  m_bufferSize(0   ),
  m_bufferPos (0   ),
  m_cursorPos (0   )
{
}

CAEBuffer::~CAEBuffer()
{
  DeAlloc();
}

void CAEBuffer::Alloc(const size_t size)
{
  DeAlloc();
  m_buffer     = (uint8_t*)_aligned_malloc(size, 16);
  m_bufferSize = size;
  m_bufferPos  = 0;
}

void CAEBuffer::ReAlloc(const size_t size)
{
#ifdef _WIN32
  m_buffer = (uint8_t*)_aligned_realloc(m_buffer, size, 16);
#else
  uint8_t* tmp = (uint8_t*)_aligned_malloc(size, 16);
  if (m_buffer)
  {
    size_t copy = std::min(size, m_bufferSize);
    memcpy(tmp, m_buffer, copy);
    _aligned_free(m_buffer);
  }
  m_buffer = tmp;
#endif

  m_bufferSize = size;
  m_bufferPos  = std::min(m_bufferPos, m_bufferSize);
}

void CAEBuffer::DeAlloc()
{
  if (m_buffer)
    _aligned_free(m_buffer);
  m_buffer     = NULL;
  m_bufferSize = 0;
  m_bufferPos  = 0;
}

void CAEBuffer::Write(const void *src, const size_t size)
{
#ifdef _DEBUG
  ASSERT(src);
  ASSERT(size <= m_bufferSize);
#endif
  memcpy(m_buffer, src, size);
  m_bufferPos = 0;
}

void CAEBuffer::Push(const void *src, const size_t size)
{
#ifdef _DEBUG
  ASSERT(src);
  ASSERT(size <= m_bufferSize - m_bufferPos);
#endif
  memcpy(m_buffer + m_bufferPos, src, size);
  m_bufferPos += size;
}

void CAEBuffer::UnShift(const void *src, const size_t size)
{
#ifdef _DEBUG
  ASSERT(src);
  ASSERT(size < m_bufferSize - m_bufferPos);
#endif
  memmove(m_buffer + size, m_buffer, m_bufferSize - size);
  memcpy (m_buffer, src, size);
  m_bufferPos += size;
}

void CAEBuffer::Read(void *dst, const size_t size)
{
#ifdef _DEBUG
  ASSERT(size <= m_bufferSize);
  ASSERT(dst);
#endif
  memcpy(dst, m_buffer, size);
}

void CAEBuffer::Pop(void *dst, const size_t size)
{
#ifdef _DEBUG
  ASSERT(size <= m_bufferPos);
#endif
  m_bufferPos -= size;
  if (dst)
    memcpy(dst, m_buffer + m_bufferPos, size);
}

void CAEBuffer::Shift(void *dst, const size_t size)
{
#ifdef _DEBUG
  ASSERT(size <= m_bufferPos);
#endif
  if (dst)
    memcpy(dst, m_buffer, size);
  memmove(m_buffer, m_buffer + size, m_bufferSize - size);
  m_bufferPos -= size;
}

void* CAEBuffer::Raw(const size_t size)
{
#ifdef _DEBUG
  ASSERT(size <= m_bufferSize);
#endif
  return m_buffer;
}

void CAEBuffer::CursorSeek(const size_t pos)
{
#ifdef _DEBUG
  ASSERT(pos <= m_bufferSize);
#endif
  m_cursorPos = pos;
}

void* CAEBuffer::CursorRead(const size_t size)
{
#ifdef _DEBUG
  ASSERT(m_cursorPos + size <= m_bufferSize);
#endif

  uint8_t* out = m_buffer + m_cursorPos;
  m_cursorPos += size;
  return out;
}

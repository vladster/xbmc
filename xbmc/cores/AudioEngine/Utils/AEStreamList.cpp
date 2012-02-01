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

#include "AEStreamList.h"

CAEStreamList::CAEStreamList()
{
  /* pre-allocate room for 16 streams */
  m_streams.reserve(16);
}

CAEStreamList::~CAEStreamList()
{
}

void CAEStreamList::AddStream(IAEStream *stream)
{
  ASSERT(stream);
  CSingleLock lock(m_lock);
 
  /* ensure the stream is not already in the list */
  ASSERT(FindStream(stream) == m_streams.end());

  /* add the stream to the list */
  ListItem i;
  i.m_stream   = stream;
  i.m_flagged  = false;
  i.m_newstate = false;
  i.m_erase    = false;
  m_streams.push_back(i);
}

void CAEStreamList::RemStream(IAEStream *stream, const bool commit/* = true */)
{
  ASSERT(stream);
  CSingleLock lock(m_lock);

  /* find and erase the stream from the main stream list */
  StreamList::iterator itt = FindStream(stream);
  ASSERT(itt != m_streams.end());

  if (commit) m_streams.erase(itt);
  else        itt->m_erase = true;
}

void CAEStreamList::SetFlag(IAEStream *stream, const bool flag, const bool commit/* = true */)
{
  ASSERT(stream);
  CSingleLock lock(m_lock);

  /* find and ensure we have the stream in our list */
  StreamList::iterator itt = FindStream(stream);
  ASSERT(itt != m_streams.end());

  /* set the new state */
  itt->m_newstate = flag;
  if (commit)
    itt->m_flagged = flag;
}

void CAEStreamList::CommitChanges()
{
  CSingleLock lock(m_lock);
  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end();)
  {
    if (itt->m_erase)
    {
      itt = m_streams.erase(itt);
      continue;
    }

    itt->m_flagged = itt->m_newstate; 
    ++itt;
  }
}

IAEStream* CAEStreamList::GetMasterStream()
{
  /* TODO: implement this */
  return NULL;
}

inline CAEStreamList::StreamList::iterator CAEStreamList::FindStream(IAEStream *stream)
{
  StreamList::iterator itt;
  for(itt = m_streams.begin(); itt != m_streams.end(); ++itt)
    if (itt->m_stream == stream) break;
  return itt;
}

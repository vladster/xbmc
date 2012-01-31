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
#include "AEFactory.h"

CAEStreamList::CAEStreamList(const bool ownsStreams/* = false */) :
  m_ownsStreams(ownsStreams)
{
}

CAEStreamList::~CAEStreamList()
{
  /* if we own the streams, free them before destruction */
  if (m_ownsStreams)
    for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
      CAEFactory::AE->FreeStream(itt->m_stream);
}

void CAEStreamList::AddStream(IAEStream *stream)
{
  ASSERT(stream);
  CExclusiveLock lock(m_lock);
 
  /* ensure the stream is not already in the list */
  ASSERT(FindStream(stream) == m_streams.end());

  /* add the stream to the list */
  ListItem i;
  i.m_stream   = stream;
  i.m_flagged  = false;
  i.m_newstate = false;
  m_streams.push_back(i);
}

void CAEStreamList::RemStream(IAEStream *stream)
{
  ASSERT(stream);
  CExclusiveLock lock(m_lock);

  /* find and erase the stream from the main stream list */
  StreamList::iterator itt = FindStream(stream);
  ASSERT(itt != m_streams.end());
  m_streams.erase(itt);
}

void CAEStreamList::DelStream(IAEStream *stream)
{
  RemStream(stream);
  CAEFactory::AE->FreeStream(stream);
}

void CAEStreamList::SetFlag(IAEStream *stream, bool flag, bool commit/* = true */)
{
  ASSERT(stream);
  CExclusiveLock lock(m_lock);

  /* find and ensure we have the stream in our list */
  StreamList::iterator itt = FindStream(stream);
  ASSERT(itt != m_streams.end());

  /* set the new state */
  itt->m_newstate = flag;
  if (commit)
    itt->m_flagged = flag;
}

void CAEStreamList::CommitFlags()
{
  CExclusiveLock lock(m_lock);
  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
    itt->m_flagged = itt->m_newstate; 
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

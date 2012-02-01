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

#include "Interfaces/AEStream.h"
#include "threads/CriticalSection.h"
#include "threads/SingleLock.h"
#include "settings/AdvancedSettings.h"

/**
 * CAEStreamList is a thread safe IAEStream list utility
 * that allows flagging of streams and deferred commit of
 * flag and removal of entries.
 */
class CAEStreamList
{
private:
  typedef struct
  {
    IAEStream *m_stream;
    bool       m_flagged;
    bool       m_newstate;
    bool       m_erase;
  } ListItem;


  typedef std::vector<ListItem> StreamList;

public:
  /**
   * CAEStreamList Constructor
   */
  CAEStreamList();
  ~CAEStreamList();

  /**
   * Add a stream to the list, new streams are not playing by default
   * @param stream The stream to add
   */
  void AddStream(IAEStream *stream);

  /**
   * Remove a stream from the list
   * @param stream The stream to remove
   */
  void RemStream(IAEStream *stream, const bool commit = true);

  /**
   * Flag a stream
   * @param stream The stream to flag
   * @param flag   The flag value
   * @param commit Commit the new value now
   */
  void SetFlag(IAEStream *stream, const bool flag, const bool commit = true);

  /**
   * Commit any pending changes
   */
  void CommitChanges();

  /**
   * Lock the stream for thread safe update
   */
  void ReadLock()
  {
     
  }

  /**
   * Gets the master stream based on raw status and the audiophile setting
   */
  IAEStream* GetMasterStream();

  /*
   * Iterator
   */
  class flagged_iterator
  {
    public:
      flagged_iterator(StreamList *streamList, StreamList::iterator itt) :
        m_streamList(streamList),
        m_itt       (itt       )
      {
        /* move to the first flagged entry */
        while(m_itt != m_streamList->end() && (!m_itt->m_flagged || m_itt->m_erase))
          ++m_itt;
      }

      inline IAEStream* operator*() const { return m_itt->m_stream; }
      inline void operator++() { while(++m_itt != m_streamList->end() && (!m_itt->m_flagged || m_itt->m_erase)) {}; }
      inline bool operator==(const flagged_iterator     &rhs) { return m_itt == rhs.m_itt; }
      inline bool operator!=(const flagged_iterator     &rhs) { return m_itt != rhs.m_itt; }
      inline bool operator==(const StreamList::iterator &rhs) { return m_itt == m_itt; }
      inline bool operator!=(const StreamList::iterator &rhs) { return m_itt != m_itt; }

    private:
      friend class CAEStreamList;
      StreamList           *m_streamList;
      StreamList::iterator m_itt;
  };

  inline flagged_iterator begin() { return flagged_iterator(&m_streams, m_streams.begin()); }
  inline flagged_iterator end  () { return flagged_iterator(&m_streams, m_streams.end  ()); }
  inline flagged_iterator erase(const flagged_iterator &itt, bool commit = true)
  {
    if (commit)
      return flagged_iterator(&m_streams, m_streams.erase(itt.m_itt));

    itt.m_itt->m_erase = true;
    return flagged_iterator(&m_streams, itt.m_itt);
  }

private:
  CCriticalSection m_lock;
  StreamList       m_streams;

  StreamList::iterator FindStream(IAEStream *stream);
};

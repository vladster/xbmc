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
#include "threads/SharedSection.h"
#include "settings/AdvancedSettings.h"

/**
 * CAEStreamList is a thread safe IAEStream list utility
 */
class CAEStreamList
{
public:
  /**
   * CAEStreamList Constructor
   * @param ownsStreams	Set this value to true to free the streams on destruction
   */
  CAEStreamList(const bool ownsStreams = false);
  ~CAEStreamList();

  /**
   * Add a stream to the list, new streams are not playing by default
   * @param stream	The stream to add
   */
  void AddStream(IAEStream *stream);

  /**
   * Remove a stream from the list
   * @param stream	The stream to remove
   */
  void RemStream(IAEStream *stream);

  /**
   * Remove and free a stream from the list
   * @param stream	The stream to remove and free
   */
  void DelStream(IAEStream *stream);

  /**
   * Flag a stream
   * @param stream	The stream to flag
   * @param flag	The flag value
   * @param commit	Commit the new value now
   */
  void SetFlag(IAEStream *stream, bool flag, bool commit = true);

  /**
   * Commit any pending flag changes
   */
  void CommitFlags();

  /**
   * Gets the master stream based on raw status and the audiophile setting
   */
  IAEStream* GetMasterStream();


private:
  typedef struct
  {
    IAEStream *m_stream;
    bool       m_flagged;
    bool       m_newstate;
  } ListItem;

  typedef std::vector<ListItem> StreamList;

  CSharedSection m_lock;
  bool           m_ownsStreams;
  StreamList     m_streams;

  StreamList::iterator FindStream(IAEStream *stream);
};

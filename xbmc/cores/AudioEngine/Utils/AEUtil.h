#pragma once
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

#include "../AEAudioFormat.h"
#include "utils/StdString.h"
#include <math.h>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

class CAEUtil
{
public:
  static CAEChannelInfo          GuessChLayout     (const unsigned int channels);
  static const char*             GetStdChLayoutName(const enum AEStdChLayout layout);
  static const unsigned int      DataFormatToBits  (const enum AEDataFormat dataFormat);
  static const char*             DataFormatToStr   (const enum AEDataFormat dataFormat);

  static inline float SoftClamp(float x)
  {
    static const double k = 0.9f;
    /* perform a soft clamp */
         if (x >  k) x = (float) (tanh((x - k) / (1 - k)) * (1 - k) + k);
    else if (x < -k) x = (float) (tanh((x + k) / (1 - k)) * (1 - k) - k);

    /* hard clamp anything still outside the bounds */
    if (x >  1.0f) return  1.0f;
    if (x < -1.0f) return -1.0f;

    /* return the final sample */
    return x;
  }

  #ifdef __SSE__
  static void SSEMulAddArray(float *data, float *add, const float mul, uint32_t count);
  static void SSEMulArray(float *data, const float mul, uint32_t count);
  #endif
};

